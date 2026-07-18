#include "ImageGpuResourceCache.h"

namespace nsk
{

ImageGpuResourceCache& ImageGpuResourceCache::Instance()
{
    static ImageGpuResourceCache instance;
    return instance;
}

void ImageGpuResourceCache::ClearUnlocked()
{
    m_cache.clear();
    m_lru.clear();
    m_bytesInUse = 0;
}

size_t ImageGpuResourceCache::EstimateBytes(UINT w, UINT h, DXGI_FORMAT format)
{
    auto blocks = [](UINT x) -> size_t { return (static_cast<size_t>(x) + 3) / 4; };
    switch (format)
    {
    // 8 bytes per 4x4 block (one colour block).
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return blocks(w) * blocks(h) * 8;
    // 16 bytes per 4x4 block (colour + alpha/second block).
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return blocks(w) * blocks(h) * 16;
    default:
        return static_cast<size_t>(w) * static_cast<size_t>(h) * 4; // BGRA8 & friends
    }
}

void ImageGpuResourceCache::EnsureDeviceGenerationUnlocked(uint64_t deviceGeneration)
{
    if (m_deviceGeneration != deviceGeneration)
    {
        ClearUnlocked(); // old SRVs belong to a dead device - drop them all
        m_deviceGeneration = deviceGeneration;
    }
}

void ImageGpuResourceCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ClearUnlocked();
}

bool ImageGpuResourceCache::TryGet(
    const std::wstring& path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
    UINT& outW, UINT& outH, DXGI_FORMAT& outFmt,
    uint64_t deviceGeneration)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    EnsureDeviceGenerationUnlocked(deviceGeneration);

    auto it = m_cache.find(path);
    if (it == m_cache.end() || !it->second.srv)
        return false;

    // Promote to most-recently-used.
    m_lru.erase(it->second.lruIt);
    m_lru.push_front(path);
    it->second.lruIt = m_lru.begin();

    outSrv = it->second.srv;
    outW = it->second.width;
    outH = it->second.height;
    outFmt = it->second.format;
    return true;
}

void ImageGpuResourceCache::Put(
    const std::wstring& path,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
    UINT w, UINT h, DXGI_FORMAT format,
    uint64_t deviceGeneration)
{
    if (!srv)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    EnsureDeviceGenerationUnlocked(deviceGeneration);

    if (auto it = m_cache.find(path); it != m_cache.end())
    {
        m_bytesInUse -= it->second.bytes;
        m_lru.erase(it->second.lruIt);
        m_cache.erase(it);
    }

    m_lru.push_front(path);
    Entry entry {};
    entry.srv = srv;
    entry.width = w;
    entry.height = h;
    entry.format = format;
    entry.bytes = EstimateBytes(w, h, format);
    entry.lruIt = m_lru.begin();
    m_bytesInUse += entry.bytes;
    m_cache.emplace(path, std::move(entry));

    // Evict LRU past EITHER bound (entry count or VRAM budget). Always keep the
    // just-inserted entry, even if it alone exceeds the budget (an 8K/16K image):
    // the current image must stay, the budget only stops accumulation.
    while (m_cache.size() > 1 &&
           (m_cache.size() > m_capacity || m_bytesInUse > m_byteBudget) &&
           !m_lru.empty())
    {
        const std::wstring victimKey = m_lru.back();
        m_lru.pop_back();
        if (auto vit = m_cache.find(victimKey); vit != m_cache.end())
        {
            m_bytesInUse -= vit->second.bytes;
            m_cache.erase(vit);
        }
    }
}

} // namespace nsk
