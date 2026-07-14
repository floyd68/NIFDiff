// TextureCache.cpp - DDS decode + D3D11 SRV creation via DirectXTex.
//
// History note: the original liteviewer port decoded DDS with gli (a GL-
// oriented image library it inherited from NifSkope's OpenGL lineage) and
// hand-built the D3D11 texture descs/subresources from gli's layout. Now
// that the renderer is D3D11-native, DirectXTex is the canonical path and
// strictly more capable: full mip chains and cube maps come through
// CreateShaderResourceView() automatically (the gli version uploaded 2D +
// cube only, with hand-rolled pitch math), legacy/odd DDS header variants
// that gli rejects are handled, and LoadFromDDSFile takes a wide path
// natively - which also fixes the old toNarrow() '& 0x7F' truncation that
// silently broke loose-file textures under non-ASCII (e.g. Korean) paths.
// ImageCore was considered but its DecodedImage payload is an image-viewer
// contract (mip0-only, 2D-only, premultiplied BGRA8) that cannot carry the
// mips/cubemaps this renderer needs.
#include "TextureCache.h"
#include "../core/ResourceResolver.h"

#include <DirectXTex.h>

namespace nsk
{
namespace
{
    ID3D11ShaderResourceView* UploadScratch(
        ID3D11Device* device,
        const DirectX::ScratchImage& scratch,
        const DirectX::TexMetadata& metadata,
        std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>& cache,
        const std::string& cacheKey)
    {
        if (!device || scratch.GetImageCount() == 0)
            return nullptr;

        // Handles 2D/mip-chain/cube-map/array layouts from the DDS metadata
        // (a cube map DDS yields a TEXTURECUBE-dimension SRV, which the lit
        // shader's TextureCube at t4 requires).
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(DirectX::CreateShaderResourceView(
                device, scratch.GetImages(), scratch.GetImageCount(), metadata, &srv)))
        {
            return nullptr;
        }

        auto [it, inserted] = cache.emplace(cacheKey, srv);
        (void)inserted;
        return it->second.Get();
    }

    // UTF-8 narrowing for cache keys only (never used as a filesystem path -
    // DirectXTex loads via the wide path directly).
    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    }
}

TextureCache::TextureCache(ID3D11Device* device, ResourceResolver* resolver)
    : m_device(device)
    , m_resolver(resolver)
{
}

ID3D11ShaderResourceView* TextureCache::LoadFromDisk(const std::wstring& fullPath)
{
    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage scratch;
    if (FAILED(DirectX::LoadFromDDSFile(fullPath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratch)))
        return nullptr;
    return UploadScratch(m_device, scratch, metadata, m_cache, ToUtf8(fullPath));
}

ID3D11ShaderResourceView* TextureCache::LoadFromMemory(std::span<const std::uint8_t> data, const std::string& cacheKey)
{
    if (data.empty())
        return nullptr;
    DirectX::TexMetadata metadata {};
    DirectX::ScratchImage scratch;
    if (FAILED(DirectX::LoadFromDDSMemory(data.data(), data.size(), DirectX::DDS_FLAGS_NONE, &metadata, scratch)))
        return nullptr;
    return UploadScratch(m_device, scratch, metadata, m_cache, cacheKey);
}

bool TextureCache::HasComplexMaterialAlpha(const std::string& relativePath)
{
    if (relativePath.empty() || m_resolver == nullptr)
        return false;
    auto it = m_cmCache.find(relativePath);
    if (it != m_cmCache.end())
        return it->second;

    bool result = false;
    ResourceBytes found = m_resolver->Find(relativePath, m_nifDirectory);
    DirectX::TexMetadata meta {};
    DirectX::ScratchImage img;
    HRESULT hr = E_FAIL;
    if (!found.diskPath.empty())
        hr = DirectX::LoadFromDDSFile(found.diskPath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, img);
    else if (!found.data.empty())
        hr = DirectX::LoadFromDDSMemory(found.data.data(), found.data.size(), DirectX::DDS_FLAGS_NONE, &meta, img);

    if (SUCCEEDED(hr) && meta.mipLevels > 0)
    {
        // Examine the coarsest mip - the same one the shader's
        // SampleLevel(15) detection lands on - decoded to RGBA8.
        const DirectX::Image* last = img.GetImage(meta.mipLevels - 1, 0, 0);
        DirectX::ScratchImage decoded;
        const DirectX::Image* px = nullptr;
        if (last != nullptr)
        {
            if (DirectX::IsCompressed(meta.format))
            {
                if (SUCCEEDED(DirectX::Decompress(*last, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
                    px = decoded.GetImage(0, 0, 0);
            }
            else if (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                px = last;
            }
            else if (SUCCEEDED(DirectX::Convert(*last, DXGI_FORMAT_R8G8B8A8_UNORM,
                         DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, decoded)))
            {
                px = decoded.GetImage(0, 0, 0);
            }
        }
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
                result = (sum / static_cast<double>(count)) / 255.0 < 1.0 - 4.0 / 255.0;
        }
    }

    m_cmCache.emplace(relativePath, result);
    return result;
}

bool TextureCache::HasComplexMaterialHeight(const std::string& relativePath)
{
    if (relativePath.empty() || m_resolver == nullptr)
        return false;
    auto it = m_cmHeightCache.find(relativePath);
    if (it != m_cmHeightCache.end())
        return it->second;

    bool result = false;
    ResourceBytes found = m_resolver->Find(relativePath, m_nifDirectory);
    DirectX::TexMetadata meta {};
    DirectX::ScratchImage img;
    HRESULT hr = E_FAIL;
    if (!found.diskPath.empty())
        hr = DirectX::LoadFromDDSFile(found.diskPath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, img);
    else if (!found.data.empty())
        hr = DirectX::LoadFromDDSMemory(found.data.data(), found.data.size(), DirectX::DDS_FLAGS_NONE, &meta, img);

    if (SUCCEEDED(hr) && meta.mipLevels > 0)
    {
        // A mid-resolution mip is plenty to judge variation and cheap to
        // decode; index 2 (quarter res) when available.
        const std::size_t mip = meta.mipLevels > 2 ? 2 : meta.mipLevels - 1;
        const DirectX::Image* src = img.GetImage(mip, 0, 0);
        DirectX::ScratchImage decoded;
        const DirectX::Image* px = nullptr;
        if (src != nullptr)
        {
            if (DirectX::IsCompressed(meta.format))
            {
                if (SUCCEEDED(DirectX::Decompress(*src, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
                    px = decoded.GetImage(0, 0, 0);
            }
            else if (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                px = src;
            }
            else if (SUCCEEDED(DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM,
                         DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, decoded)))
            {
                px = decoded.GetImage(0, 0, 0);
            }
        }
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
            result = (hi - lo) >= 16 && total > 0 && midCount * 10 >= total;
        }
    }

    m_cmHeightCache.emplace(relativePath, result);
    return result;
}

ID3D11ShaderResourceView* TextureCache::GetOrLoad(const std::string& relativePath)
{
    auto it = m_cache.find(relativePath);
    if (it != m_cache.end())
        return it->second.Get();

    ID3D11ShaderResourceView* loaded = nullptr;
    if (m_resolver)
    {
        ResourceBytes found = m_resolver->Find(relativePath, m_nifDirectory);
        if (!found.diskPath.empty())
            loaded = LoadFromDisk(found.diskPath);
        else if (!found.data.empty())
            loaded = LoadFromMemory(found.data, "bsa:" + relativePath);
    }

    // Cache the result either way - a null entry records "tried and missing"
    // so failed paths don't re-run the resolver chain on every frame.
    m_cache[relativePath] = loaded;
    return loaded;
}

} // namespace nsk
