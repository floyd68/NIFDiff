// TextureRepository.cpp - DDS decode + D3D11 SRV creation via DirectXTex,
// pooled per resolved source (see the header for the dedup rationale; the
// decode-path notes that used to live in TextureCache.cpp apply here:
// DirectXTex handles full mip chains / cube maps / legacy DDS variants and
// takes wide paths natively).
#include "TextureRepository.h"
#include "../core/ResourceResolver.h"
#include "../core/StartupTrace.h"

#include <DirectXTex.h>

#include <algorithm>
#include <execution>
#include <unordered_set>

namespace nsk
{
namespace
{
    // Decodes one mip of img to RGBA8 for CPU inspection. Returns the image
    // to read (either `img`'s own mip or `decoded`), or nullptr.
    const DirectX::Image* DecodeMipToRGBA8(const DirectX::ScratchImage& img,
                                           const DirectX::TexMetadata& meta,
                                           std::size_t mip,
                                           DirectX::ScratchImage& decoded)
    {
        const DirectX::Image* src = img.GetImage(mip, 0, 0);
        if (src == nullptr)
            return nullptr;
        if (DirectX::IsCompressed(meta.format))
        {
            if (SUCCEEDED(DirectX::Decompress(*src, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
                return decoded.GetImage(0, 0, 0);
            return nullptr;
        }
        if (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM)
            return src;
        if (SUCCEEDED(DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM,
                          DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, decoded)))
            return decoded.GetImage(0, 0, 0);
        return nullptr;
    }
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureRepository::LoadFromDisk(const std::wstring& fullPath, DXGI_FORMAT& outFormat)
{
    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage scratch;
    if (FAILED(DirectX::LoadFromDDSFile(fullPath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratch)))
        return nullptr;
    outFormat = metadata.format;

    // Handles 2D/mip-chain/cube-map/array layouts from the DDS metadata
    // (a cube map DDS yields a TEXTURECUBE-dimension SRV, which the lit
    // shader's TextureCube at t4 requires).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(DirectX::CreateShaderResourceView(
            m_device, scratch.GetImages(), scratch.GetImageCount(), metadata, &srv)))
        return nullptr;
    return srv;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureRepository::LoadFromMemory(std::span<const std::uint8_t> data, DXGI_FORMAT& outFormat)
{
    if (data.empty())
        return nullptr;
    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage scratch;
    if (FAILED(DirectX::LoadFromDDSMemory(data.data(), data.size(), DirectX::DDS_FLAGS_NONE, &metadata, scratch)))
        return nullptr;
    outFormat = metadata.format;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(DirectX::CreateShaderResourceView(
            m_device, scratch.GetImages(), scratch.GetImageCount(), metadata, &srv)))
        return nullptr;
    return srv;
}

TextureRepository::Entry* TextureRepository::GetOrLoad(const std::string& relativePath,
                                                       const std::wstring& nifDirectory)
{
    if (relativePath.empty() || m_resolver == nullptr || m_device == nullptr)
        return nullptr;

    using TraceClock = StartupTrace::Clock;
    const auto t0 = TraceClock::now();
    ResourceBytes found = m_resolver->Find(relativePath, nifDirectory);
    const auto t1 = TraceClock::now();
    if (!found.ok())
    {
        NIFLOG_TRACE("[TEXLOAD] miss resolve={:.2f}ms {}",
            std::chrono::duration<double, std::milli>(t1 - t0).count(), relativePath);
        return nullptr;
    }

    if (auto it = m_bySource.find(found.sourceKey); it != m_bySource.end())
    {
        NIFLOG_TRACE("[TEXLOAD] pool-hit resolve={:.2f}ms {} <- {}",
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            relativePath, found.sourceKey);
        return &it->second;
    }

    Entry entry;
    entry.relativePath = relativePath;
    entry.nifDirectory = nifDirectory;
    const char* source = "loose";
    if (!found.diskPath.empty())
    {
        entry.srv = LoadFromDisk(found.diskPath, entry.format);
    }
    else
    {
        entry.srv = LoadFromMemory(found.data, entry.format);
        source = "archive";
    }
    const auto t2 = TraceClock::now();

    NIFLOG_TRACE("[TEXLOAD] {} resolve={:.2f}ms decode+upload={:.2f}ms ok={} {}",
        source,
        std::chrono::duration<double, std::milli>(t1 - t0).count(),
        std::chrono::duration<double, std::milli>(t2 - t1).count(),
        entry.srv != nullptr, relativePath);

    return &m_bySource.emplace(found.sourceKey, std::move(entry)).first->second;
}

void TextureRepository::Prefetch(const std::vector<std::string>& relativePaths, const std::wstring& nifDirectory)
{
    if (m_resolver == nullptr || m_device == nullptr || relativePaths.empty())
        return;

    struct PendingLoad
    {
        std::string sourceKey;
        std::string relativePath;
        ResourceBytes bytes;
    };

    // Phase 1 (calling thread): resolve + dedupe against the pool and
    // within the request itself.
    const auto t0 = StartupTrace::Clock::now();
    std::vector<PendingLoad> pending;
    std::unordered_set<std::string> seen;
    for (const std::string& rel : relativePaths)
    {
        if (rel.empty())
            continue;
        ResourceBytes found = m_resolver->Find(rel, nifDirectory);
        if (!found.ok())
            continue;
        if (m_bySource.find(found.sourceKey) != m_bySource.end() || !seen.insert(found.sourceKey).second)
            continue;
        std::string key = found.sourceKey;
        pending.push_back(PendingLoad { std::move(key), rel, std::move(found) });
    }
    if (pending.empty())
        return;

    // Phase 2 (parallel): the per-texture cost - file read, DDS parse, SRV
    // upload - is independent per source; Load* only touch DirectXTex and
    // the free-threaded device.
    std::vector<Entry> loaded(pending.size());
    std::for_each(std::execution::par, pending.begin(), pending.end(),
        [this, &pending, &loaded, &nifDirectory](PendingLoad& p)
        {
            const std::size_t i = static_cast<std::size_t>(&p - pending.data());
            Entry& e = loaded[i];
            e.relativePath = p.relativePath;
            e.nifDirectory = nifDirectory;
            if (!p.bytes.diskPath.empty())
                e.srv = LoadFromDisk(p.bytes.diskPath, e.format);
            else
                e.srv = LoadFromMemory(p.bytes.data, e.format);
        });

    // Phase 3 (calling thread): publish into the pool.
    std::size_t okCount = 0;
    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        okCount += loaded[i].srv != nullptr ? 1u : 0u;
        m_bySource.emplace(std::move(pending[i].sourceKey), std::move(loaded[i]));
    }
    NIFLOG_INFO("[TEXLOAD] prefetch: {} path(s) -> {} new source(s), {} loaded ok, {:.1f}ms total",
        relativePaths.size(), pending.size(), okCount,
        std::chrono::duration<double, std::milli>(StartupTrace::Clock::now() - t0).count());
}

void TextureRepository::EnsureCmProbe(Entry& entry)
{
    if (entry.cmProbed)
        return;
    entry.cmProbed = true; // even if the re-read fails: both verdicts stay false

    if (m_resolver == nullptr)
        return;

    // BC1 pre-filter: 1-bit alpha can never carry the complex-material
    // gloss/height gradient (the ecosystem's _m convention needs a full
    // alpha channel; CM tools emit BC3/BC7). Vanilla env masks are commonly
    // BC1, so this skips the whole re-read + decode on the most frequent
    // "not a complex material" answer.
    if (entry.format == DXGI_FORMAT_BC1_TYPELESS
        || entry.format == DXGI_FORMAT_BC1_UNORM
        || entry.format == DXGI_FORMAT_BC1_UNORM_SRGB)
    {
        NIFLOG_TRACE("[TEXLOAD] CM probe skipped (BC1) {}", entry.relativePath);
        return;
    }

    const auto tCm0 = StartupTrace::Clock::now();
    ResourceBytes found = m_resolver->Find(entry.relativePath, entry.nifDirectory);
    DirectX::TexMetadata meta {};
    DirectX::ScratchImage img;
    HRESULT hr = E_FAIL;
    if (!found.diskPath.empty())
        hr = DirectX::LoadFromDDSFile(found.diskPath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, img);
    else if (!found.data.empty())
        hr = DirectX::LoadFromDDSMemory(found.data.data(), found.data.size(), DirectX::DDS_FLAGS_NONE, &meta, img);
    if (FAILED(hr) || meta.mipLevels == 0)
        return;

    // cmAlpha: examine the coarsest mip - the same one the shader's
    // SampleLevel(15) detection lands on - decoded to RGBA8. Vanilla env
    // masks decode fully opaque; a complex material's average alpha sits
    // meaningfully below 1.
    {
        DirectX::ScratchImage decoded;
        const DirectX::Image* px = DecodeMipToRGBA8(img, meta, meta.mipLevels - 1, decoded);
        if (px != nullptr && px->pixels != nullptr)
        {
            double sum = 0.0;
            std::size_t count = 0;
            for (std::size_t y = 0; y < px->height; ++y)
            {
                const std::uint8_t* row = px->pixels + y * px->rowPitch;
                for (std::size_t x = 0; x < px->width; ++x)
                {
                    sum += row[x * 4 + 3];
                    ++count;
                }
            }
            if (count > 0)
                entry.cmAlpha = (sum / static_cast<double>(count)) / 255.0 < 1.0 - 4.0 / 255.0;
        }
    }

    // cmHeight: whether the alpha actually VARIES (a usable height field).
    // A mid-resolution mip is plenty to judge variation and cheap to
    // decode; index 2 (quarter res) when available.
    {
        const std::size_t mip = meta.mipLevels > 2 ? 2 : meta.mipLevels - 1;
        DirectX::ScratchImage decoded;
        const DirectX::Image* px = DecodeMipToRGBA8(img, meta, mip, decoded);
        if (px != nullptr && px->pixels != nullptr)
        {
            std::uint8_t lo = 255, hi = 0;
            std::size_t midCount = 0;
            for (std::size_t y = 0; y < px->height; ++y)
            {
                const std::uint8_t* row = px->pixels + y * px->rowPitch;
                for (std::size_t x = 0; x < px->width; ++x)
                {
                    const std::uint8_t a = row[x * 4 + 3];
                    lo = a < lo ? a : lo;
                    hi = a > hi ? a : hi;
                    midCount += (a > 8 && a < 247) ? 1 : 0;
                }
            }
            // Two ways an _m alpha varies without being a height field:
            // BC compression wobbles a flat channel by a few steps (require
            // a real spread), and some materials store a binary 0/255 MASK
            // in the alpha (guard-armor trims etc.) - the shader's
            // per-pixel extremes guard skips POM there anyway, so also
            // require a meaningful share of mid-range texels. Authored
            // heights are continuous: sampled vanilla height fields measure
            // ~100% mid-range vs <3% for masks, so the 10% line isn't
            // delicate.
            const std::size_t total = px->width * px->height;
            entry.cmHeight = (hi - lo) >= 16 && total > 0 && midCount * 10 >= total;
        }
    }

    NIFLOG_TRACE("[TEXLOAD] CM probe {:.2f}ms alpha={} height={} {}",
        std::chrono::duration<double, std::milli>(StartupTrace::Clock::now() - tCm0).count(),
        entry.cmAlpha, entry.cmHeight, entry.relativePath);
}

} // namespace nsk
