#include "ResourceManager.h"

#include <Backplate.h> // FD2D::AsyncRedrawToken

#include <algorithm>

namespace nsk
{

ResourceManager::~ResourceManager()
{
    Shutdown();
}

void ResourceManager::Start(unsigned threadCount)
{
    if (!m_workers.empty())
        return;
    m_stop = false;
    if (threadCount == 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        threadCount = std::clamp(hw, 2u, 6u); // a small, bounded pool
    }
    m_workers.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i)
        m_workers.emplace_back([this] { WorkerLoop(); });
}

void ResourceManager::SetRedrawToken(std::shared_ptr<FD2D::AsyncRedrawToken> token)
{
    m_redraw = std::move(token);
}

void ResourceManager::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_stop = true;
    }
    m_jobCv.notify_all();
    for (std::thread& t : m_workers)
        if (t.joinable())
            t.join();
    m_workers.clear();
    // Drop any queued work/results (their requesters are going away too).
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        for (auto& q : m_jobs) q.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_completionMutex);
        m_completions.clear();
    }
}

std::uint64_t ResourceManager::BumpGeneration(const void* requester)
{
    std::lock_guard<std::mutex> lk(m_genMutex);
    return ++m_generation[requester]; // 0 -> 1 for a new requester
}

std::uint64_t ResourceManager::CurrentGeneration(const void* requester) const
{
    std::lock_guard<std::mutex> lk(m_genMutex);
    const auto it = m_generation.find(requester);
    return it == m_generation.end() ? 0 : it->second;
}

bool ResourceManager::IsCurrent(Token token) const
{
    std::lock_guard<std::mutex> lk(m_genMutex);
    const auto it = m_generation.find(token.requester);
    return it != m_generation.end() && it->second == token.generation;
}

void ResourceManager::Cancel(const void* requester)
{
    std::lock_guard<std::mutex> lk(m_genMutex);
    m_generation.erase(requester);
}

void ResourceManager::Submit(Priority prio, Token token, std::function<void()> work)
{
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_jobs[static_cast<std::size_t>(prio)].push_back({ token, std::move(work) });
    }
    m_jobCv.notify_one();
}

void ResourceManager::PostCompletion(Token token, std::function<void()> uiCallback)
{
    {
        std::lock_guard<std::mutex> lk(m_completionMutex);
        m_completions.push_back({ token, std::move(uiCallback) });
    }
    if (m_redraw)
        m_redraw->RequestAsyncRedraw(); // wake the UI to drain it
}

void ResourceManager::DrainCompletions()
{
    std::deque<Completion> ready;
    {
        std::lock_guard<std::mutex> lk(m_completionMutex);
        ready.swap(m_completions);
    }
    for (Completion& c : ready)
        if (c.cb && IsCurrent(c.token))
            c.cb();
    // Stale ones are simply dropped (their callback is destroyed, never run).
}

bool ResourceManager::HasJob() const
{
    for (const auto& q : m_jobs)
        if (!q.empty())
            return true;
    return false;
}

ResourceManager::Job ResourceManager::PopHighestPriority()
{
    for (auto& q : m_jobs) // array is ordered by priority (lowest enum first)
    {
        if (!q.empty())
        {
            Job job = std::move(q.front());
            q.pop_front();
            return job;
        }
    }
    return {};
}

void ResourceManager::WorkerLoop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_jobMutex);
            m_jobCv.wait(lk, [this] { return m_stop.load() || HasJob(); });
            if (m_stop.load())
                return;
            job = PopHighestPriority();
        }
        if (!job.work || !IsCurrent(job.token))
            continue; // cancelled before it ran
        job.work();    // self-contained; hands results back via PostCompletion
    }
}

} // namespace nsk
