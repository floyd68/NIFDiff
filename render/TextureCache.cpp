#include "TextureCache.h"
#include "../core/ResourceResolver.h"

#include <gli/gli.hpp>
#include <gli/dx.hpp>
#include <vector>

namespace nsk
{
namespace
{
    std::string toNarrow(const std::wstring& w)
    {
        std::string s(w.size(), '\0');
        for (std::size_t i = 0; i < w.size(); ++i)
            s[i] = static_cast<char>(w[i] & 0x7Fu);
        return s;
    }

    ID3D11ShaderResourceView* UploadGli(
        ID3D11Device* device,
        gli::texture tex,
        std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>& cache,
        const std::string& cacheKey)
    {
        if (!device || tex.empty() || tex.target() != gli::TARGET_2D)
            return nullptr;

        gli::dx dxTranslator;
        gli::dx::format const& fmt = dxTranslator.translate(tex.format());
        DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(fmt.DXGIFormat.DDS);
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
            return nullptr;

        gli::texture2d tex2d(tex);
        const D3D11_TEXTURE2D_DESC desc {
            .Width = static_cast<UINT>(tex2d.extent(0).x),
            .Height = static_cast<UINT>(tex2d.extent(0).y),
            .MipLevels = static_cast<UINT>(tex2d.levels()),
            .ArraySize = 1,
            .Format = dxgiFormat,
            .SampleDesc = { .Count = 1 },
            .Usage = D3D11_USAGE_IMMUTABLE,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };

        std::vector<D3D11_SUBRESOURCE_DATA> subresources(desc.MipLevels);
        for (UINT level = 0; level < desc.MipLevels; ++level)
        {
            gli::extent2d extent = tex2d.extent(level);
            gli::ivec3 blockExtent = gli::block_extent(tex2d.format());
            std::size_t blockSize = gli::block_size(tex2d.format());
            std::size_t blocksWide = static_cast<std::size_t>((extent.x + blockExtent.x - 1) / blockExtent.x);
            if (blocksWide == 0) blocksWide = 1;

            subresources[level].pSysMem = tex2d.data(0, 0, level);
            subresources[level].SysMemPitch = static_cast<UINT>(blocksWide * blockSize);
            subresources[level].SysMemSlicePitch = static_cast<UINT>(tex2d.size(level));
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTex;
        if (FAILED(device->CreateTexture2D(&desc, subresources.data(), &d3dTex)))
            return nullptr;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(d3dTex.Get(), nullptr, &srv)))
            return nullptr;

        auto [it, inserted] = cache.emplace(cacheKey, srv);
        (void)inserted;
        return it->second.Get();
    }
}

TextureCache::TextureCache(ID3D11Device* device, ResourceResolver* resolver)
    : m_device(device)
    , m_resolver(resolver)
{
}

ID3D11ShaderResourceView* TextureCache::GetOrCreateFallback()
{
    if (m_fallback)
        return m_fallback.Get();
    if (!m_device)
        return nullptr;

    std::uint32_t grayPixel = 0xFF808080u;
    const D3D11_TEXTURE2D_DESC td {
        .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    const D3D11_SUBRESOURCE_DATA sd { .pSysMem = &grayPixel, .SysMemPitch = sizeof(grayPixel) };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&td, &sd, &tex)))
        return nullptr;
    m_device->CreateShaderResourceView(tex.Get(), nullptr, &m_fallback);
    return m_fallback.Get();
}

ID3D11ShaderResourceView* TextureCache::LoadFromDisk(const std::wstring& fullPath)
{
    std::string narrowPath = toNarrow(fullPath);
    return UploadGli(m_device, gli::load(narrowPath), m_cache, narrowPath);
}

ID3D11ShaderResourceView* TextureCache::LoadFromMemory(std::span<const std::uint8_t> data, const std::string& cacheKey)
{
    if (data.empty())
        return nullptr;
    return UploadGli(m_device, gli::load(reinterpret_cast<char const*>(data.data()), data.size()), m_cache, cacheKey);
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

    if (loaded)
    {
        m_cache[relativePath] = loaded;
        return loaded;
    }

    ID3D11ShaderResourceView* fallback = GetOrCreateFallback();
    m_cache[relativePath] = fallback;
    return fallback;
}

} // namespace nsk
