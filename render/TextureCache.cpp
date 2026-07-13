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
