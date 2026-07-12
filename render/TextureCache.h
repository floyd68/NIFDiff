// TextureCache.h - D3D11 upload half of NifSkope's TexCache.
// Path resolution (Game Data / overrides / BSA) lives in ResourceResolver;
// this class only decodes DDS (via gli) and creates SRVs.
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace nsk
{

class ResourceResolver;

class TextureCache
{
public:
    TextureCache(ID3D11Device* device, ResourceResolver* resolver);

    void SetResolver(ResourceResolver* resolver) { m_resolver = resolver; }
    void SetNifDirectory(std::wstring nifDir) { m_nifDirectory = std::move(nifDir); }

    // relativePath uses forward slashes (see NifDocument::normalizeSlashes).
    // Returns a cached or freshly loaded SRV; missing textures yield a 1x1
    // gray fallback SRV (never nullptr for "not found").
    ID3D11ShaderResourceView* GetOrLoad(const std::string& relativePath);

    void Clear() { m_cache.clear(); }

private:
    ID3D11ShaderResourceView* LoadFromDisk(const std::wstring& fullPath);
    ID3D11ShaderResourceView* LoadFromMemory(std::span<const std::uint8_t> data, const std::string& cacheKey);
    ID3D11ShaderResourceView* GetOrCreateFallback();

    ID3D11Device* m_device = nullptr;
    ResourceResolver* m_resolver = nullptr;
    std::wstring m_nifDirectory;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_cache;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fallback;
};

} // namespace nsk
