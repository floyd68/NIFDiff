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
        m_lru.erase(it->second.lruIt);
        m_cache.erase(it);
    }

    m_lru.push_front(path);
    Entry entry {};
    entry.srv = srv;
    entry.width = w;
    entry.height = h;
    entry.format = format;
    entry.lruIt = m_lru.begin();
    m_cache.emplace(path, std::move(entry));

    while (m_cache.size() > m_capacity && !m_lru.empty())
    {
        const std::wstring victimKey = m_lru.back();
        m_lru.pop_back();
        m_cache.erase(victimKey);
    }
}

} // namespace nsk
