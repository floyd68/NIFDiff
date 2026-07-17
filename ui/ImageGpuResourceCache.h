// ImageGpuResourceCache.h - process-wide LRU cache of texture path ->
// ID3D11ShaderResourceView for the standalone image view (ImagePane).
//
// Browsing a folder/archive of textures in a pane's thumbnail strip flips
// through images repeatedly; without this, every re-selection re-runs the
// async ImageCore decode AND rebuilds the GPU texture. This caches the
// uploaded SRV keyed by the exact path ImagePane loaded, so stepping back to a
// recently-viewed texture applies its SRV synchronously - no decode round-trip,
// no re-upload.
//
// Only the GPU (block-compressed DDS) path is cached: those SRVs are device
// resources safe to share, and they are the expensive 4K game textures the
// thumbnail strip browses. PNG/JPG go the CPU/D2D-bitmap route and are not
// pooled here (ImageCore's own decode cache already covers their re-decode).
//
// The cache self-clears when the D3D device generation changes (device loss /
// recreation), so a stale SRV from a dead device is never handed back.
//
// NOTE ON NIF TEXTURES: textures referenced BY a .nif are cached separately by
// TextureRepository (keyed on the RESOLVED source, with Bethesda search-order
// resolution + async prefetch + complex-material probes). That is the richer,
// pre-existing equivalent of this cache for the NIF path; the two intentionally
// stay distinct because they key on different things (an explicit user-picked
// path here vs. a resolved engine-order source there).
#pragma once

#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nsk
{

class ImageGpuResourceCache
{
public:
    static ImageGpuResourceCache& Instance();

    ImageGpuResourceCache(const ImageGpuResourceCache&) = delete;
    ImageGpuResourceCache& operator=(const ImageGpuResourceCache&) = delete;

    // Fetch the cached SRV for `path` if present and still valid for
    // `deviceGeneration` (a mismatch flushes the whole cache first). Promotes
    // the entry to most-recently-used. Returns false on miss.
    bool TryGet(const std::wstring& path,
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                UINT& outW, UINT& outH, DXGI_FORMAT& outFmt,
                uint64_t deviceGeneration);

    // Insert/refresh `path`'s SRV (ignored when srv is null). Evicts the
    // least-recently-used entry past the capacity cap.
    void Put(const std::wstring& path,
             const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
             UINT w, UINT h, DXGI_FORMAT format,
             uint64_t deviceGeneration);

    void Clear();

private:
    ImageGpuResourceCache() = default;

    void ClearUnlocked();
    void EnsureDeviceGenerationUnlocked(uint64_t deviceGeneration);

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
        UINT width { 0 };
        UINT height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        std::list<std::wstring>::iterator lruIt {};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, Entry> m_cache;
    std::list<std::wstring> m_lru; // front = most-recently-used
    size_t m_capacity { 64 };
    uint64_t m_deviceGeneration { 0 };
};

} // namespace nsk
