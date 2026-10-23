#include "ResourceManager.h"

#include "NifDocument.h"
#include "NifLog.h"
#include "StartupTrace.h"

#include <Backplate.h> // FD2D::AsyncRedrawToken
#include <VirtualPath.h>        // Floar: archive-inner path parsing
#include <VirtualFileSystem.h>  // Floar: read a NIF out of a BSA/BA2

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <optional>
#include <system_error>
#include <vector>

namespace nsk
{

bool LoadNifDocument(NifDocument& doc, const std::wstring& path, std::string* error)
{
    // A path that points inside a BSA/BA2 (e.g. "...\foo.ba2\meshes\x.nif") is
    // read through Floar's VFS and parsed from memory; anything else is a plain
    // file on disk. VirtualPath::Parse only reports IsInArchive when an archive
    // extension appears mid-path, so loose files fall straight through.
    if (auto vp = Floar::VirtualPath::Parse(path); vp && vp->IsInArchive())
    {
        std::vector<std::uint8_t> bytes = Floar::VirtualFileSystem::ReadFile(*vp);
        if (bytes.empty())
        {
            if (error) *error = "Could not read archive entry";
            NIFLOG_WARN("LoadNifDocument: archive entry empty/missing");
            return false;
        }
        // Keep filePath() naming the VFS source (labels, session persistence).
        return doc.loadFromMemory(bytes, error, path);
    }
    return doc.loadFromFile(path, error);
}

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

void ResourceManager::Submit(Priority prio, Token token, std::function<void()> work,
                             FailureHandler onFailure)
{
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_jobs[static_cast<std::size_t>(prio)].push_back(
            { token, std::move(work), std::move(onFailure) });
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

std::wstring ResourceManager::NormalizeNifKey(const std::wstring& path)
{
    // Windows paths are case-insensitive and may mix separators, so fold both
    // out; two spellings of the same file must land on one cache entry.
    std::wstring key = std::filesystem::path(path).lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return key;
}

void ResourceManager::SetIoPermits(int permits)
{
    std::lock_guard<std::mutex> lk(m_ioMutex);
    // Difference goes to the free pool (held permits are unaffected). Called
    // before loads begin, but this keeps it correct if retuned live.
    m_ioPermits += (permits - (m_ioPermits + m_ioActive));
    if (m_ioPermits < 0)
        m_ioPermits = 0;
    m_ioCv.notify_all();
}

int ResourceManager::IoPermitsInUse() const
{
    std::lock_guard<std::mutex> lk(m_ioMutex);
    return m_ioActive;
}

int ResourceManager::IoPermitsFree() const
{
    std::lock_guard<std::mutex> lk(m_ioMutex);
    return m_ioPermits;
}

int ResourceManager::IoAcquire(Priority prio)
{
    const std::size_t p = static_cast<std::size_t>(prio);
    std::unique_lock<std::mutex> lk(m_ioMutex);
    ++m_ioWaiting[p];
    m_ioCv.wait(lk, [&]
    {
        if (m_ioPermits <= 0)
            return false;
        // Yield to any higher-priority (lower enum) waiter still queued.
        for (std::size_t hp = 0; hp < p; ++hp)
            if (m_ioWaiting[hp] > 0)
                return false;
        return true;
    });
    --m_ioWaiting[p];
    --m_ioPermits;
    ++m_ioActive;
    m_ioPeak = (std::max)(m_ioPeak, m_ioActive);
    return m_ioActive;
}

void ResourceManager::IoRelease()
{
    {
        std::lock_guard<std::mutex> lk(m_ioMutex);
        ++m_ioPermits;
        --m_ioActive;
    }
    m_ioCv.notify_all(); // wake waiters; the highest-priority one proceeds
}

std::shared_ptr<const NifDocument> ResourceManager::GetOrParseNif(
    const std::wstring& path, std::string* error, Priority prio, bool throttle)
{
    const std::wstring key = NormalizeNifKey(path);

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    const auto stamp = ec ? std::filesystem::file_time_type::min() : mtime;

    const std::string name = std::filesystem::path(path).filename().string();

    std::shared_future<NifPtr> join; // set if we must wait on another parse
    {
        std::unique_lock<std::mutex> lk(m_nifMutex);

        // Cache hit (still fresh): move it to the MRU front and return it.
        if (auto it = m_nifCache.find(key); it != m_nifCache.end())
        {
            if (it->second.mtime == stamp)
            {
                m_nifLru.splice(m_nifLru.begin(), m_nifLru, it->second.lru);
                NIFLOG_INFO("[NIFCACHE] hit   {}", name);
                return it->second.doc;
            }
            // Stale (file changed on disk): drop it and re-parse below.
            m_nifLru.erase(it->second.lru);
            m_nifCache.erase(it);
        }

        // Already being parsed by someone else: join their result.
        if (auto it = m_nifInFlight.find(key); it != m_nifInFlight.end())
        {
            join = it->second;
        }
        else
        {
            // Become the parser: publish a future the joiners can wait on.
            std::promise<NifPtr> promise;
            std::shared_future<NifPtr> fut = promise.get_future().share();
            m_nifInFlight.emplace(key, fut);

            // If anything below throws, the in-flight entry must still go away
            // and the joiners must still be released (with nullptr) - exactly
            // once. Declared after `lk`, so it runs first during unwinding and
            // must not re-lock a mutex this thread may already hold.
            struct InFlightGuard
            {
                std::unique_lock<std::mutex>& lock;
                std::unordered_map<std::wstring, std::shared_future<NifPtr>>& inFlight;
                const std::wstring& key;
                std::promise<NifPtr>& promise;
                bool completed = false;
                ~InFlightGuard()
                {
                    if (completed)
                        return;
                    if (!lock.owns_lock())
                        lock.lock();
                    inFlight.erase(key);
                    promise.set_value(nullptr);
                }
            } inFlight { lk, m_nifInFlight, key, promise };

            // Parse OUTSIDE the lock so concurrent requests for OTHER files (or
            // joiners of this one) aren't blocked by our disk read + parse.
            // Background reads pass through the IoGate (bounded disk
            // concurrency); the synchronous UI path (throttle=false) does not,
            // so it never waits behind background loads.
            lk.unlock();
            NifPtr doc;
            double ms = 0.0;
            int io = 0; // held permits incl. self
            {
                std::optional<IoPermit> permit;
                if (throttle)
                {
                    permit.emplace(*this, prio);
                    io = IoPermitsInUse();
                }
                const auto t0 = StartupTrace::Clock::now();
                auto mutableDoc = std::make_shared<NifDocument>();
                const bool ok = (m_nifLoader ? m_nifLoader(*mutableDoc, path, error)
                                             : LoadNifDocument(*mutableDoc, path, error))
                                && mutableDoc->isValid();
                doc = ok ? NifPtr(std::move(mutableDoc)) : nullptr;
                ms = std::chrono::duration<double, std::milli>(
                    StartupTrace::Clock::now() - t0).count();
            } // permit released here, also when the parse throws
            lk.lock();

            m_nifInFlight.erase(key);
            if (doc && !PublishNifToCache(key, doc, stamp))
            {
                NIFLOG_WARN("[NIFCACHE] not cached (publication failed) {}", name);
            }
            inFlight.completed = true;
            promise.set_value(doc); // release any joiners
            NIFLOG_INFO("[NIFCACHE] parse P{} {} ({:.1f} ms, io={}){}",
                        static_cast<int>(prio), name, ms, io, doc ? "" : " FAILED");
            return doc;
        }
    }

    // Wait for the in-flight parse (its owner isn't blocked, so this returns
    // once the parse finishes; failure yields nullptr).
    NIFLOG_INFO("[NIFCACHE] join  {}", name);
    if (m_nifJoinHook)
        m_nifJoinHook();
    NifPtr doc = join.get();
    if (!doc && error)
        *error = "NIF parse failed";
    return doc;
}

bool ResourceManager::PublishNifToCache(const std::wstring& key, const NifPtr& doc,
                                        std::filesystem::file_time_type stamp)
{
    // Caller holds m_nifMutex. Caching is an optimization: if it cannot
    // allocate, leave the cache and LRU exactly as they were and let the caller
    // hand the parsed document out uncached - nothing may throw while the
    // lock is held (see the in-flight guard in GetOrParseNif).
    bool lruAdded = false;
    try
    {
        if (m_nifPublishHook)
            m_nifPublishHook();
        m_nifLru.push_front(key);
        lruAdded = true;
        m_nifCache[key] = NifEntry { doc, stamp, m_nifLru.begin() };
    }
    catch (...)
    {
        if (lruAdded)
            m_nifLru.pop_front();
        return false;
    }
    EvictNif(); // erase-only; does not allocate
    return true;
}

void ResourceManager::EvictNif()
{
    // Caller holds m_nifMutex. Trim from the LRU back, but keep any doc still
    // referenced elsewhere (a pane/thumbnail holds it) - evicting a pinned doc
    // would only force a re-parse when it's asked for again.
    while (m_nifCache.size() > kNifCacheCap)
    {
        bool evicted = false;
        for (auto rit = m_nifLru.rbegin(); rit != m_nifLru.rend(); ++rit)
        {
            auto cit = m_nifCache.find(*rit);
            if (cit == m_nifCache.end())
                continue;
            if (cit->second.doc.use_count() > 1) // pinned: in active use
                continue;
            m_nifLru.erase(std::next(rit).base());
            m_nifCache.erase(cit);
            evicted = true;
            break;
        }
        if (!evicted)
            break; // everything over the cap is pinned; let the cache grow
    }
}

std::size_t ResourceManager::NifCacheSize() const
{
    std::lock_guard<std::mutex> lk(m_nifMutex);
    return m_nifCache.size();
}

void ResourceManager::ClearNifCache()
{
    std::lock_guard<std::mutex> lk(m_nifMutex);
    m_nifCache.clear();
    m_nifLru.clear();
    // In-flight parses are left alone; they erase themselves on completion.
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
        RunJob(job);
    }
}

void ResourceManager::RunJob(Job& job)
{
    std::string what;
    try
    {
        job.work(); // self-contained; hands results back via PostCompletion
        return;
    }
    catch (const std::exception& e)
    {
        try { what = e.what(); } catch (...) {}
    }
    catch (...)
    {
        try { what = "non-standard exception"; } catch (...) {}
    }

    // The job failed. Delivering the failure result and logging it are
    // independent best-effort steps - a logger that cannot open its file must
    // not cost the requester its failure result - and nothing here may let an
    // exception escape the worker thread.
    bool delivered = !job.onFailure;
    try
    {
        if (job.onFailure)
        {
            PostCompletion(job.token,
                [onFailure = std::move(job.onFailure), what]() { onFailure(what); });
            delivered = true;
        }
    }
    catch (...)
    {
        // e.g. bad_alloc: the failure result cannot be delivered.
    }
    try
    {
        NIFLOG_ERROR("[RESMGR] job failed: {}{}", what,
                     delivered ? "" : " (failure result could not be delivered)");
    }
    catch (...)
    {
        // Logging is diagnostics only.
    }
}

} // namespace nsk
