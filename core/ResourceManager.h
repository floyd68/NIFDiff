// ResourceManager.h - the app-wide async load manager (design:
// docs/resource-manager-design.md, migration phase 1).
//
// Phase 1 is the shared thread pool + completion queue that replaces the
// per-subsystem threading (today only ThumbnailStrip's per-strip std::thread
// pools use it; textures/NIF caching come in later phases). Work is submitted
// with a priority and a {requester, generation} cancellation token: a worker
// runs the job only while the token is still current, and results are handed
// back to the UI thread through a completion queue drained once per frame.
//
// Threading rule (see the design doc): submitted `work` runs on a pool thread
// and MUST be self-contained (no dereference of the requester - it may have
// been destroyed); it hands a UI-thread callback back via PostCompletion, and
// that callback runs in DrainCompletions ONLY if the token is still current
// (i.e. the requester is alive and its generation unchanged).
#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FD2D { class AsyncRedrawToken; }

namespace nsk
{

class ResourceManager
{
public:
    // Opaque requester identity + a generation snapshot. Bumping a requester's
    // generation invalidates every token taken before the bump.
    struct Token
    {
        const void* requester = nullptr;
        std::uint64_t generation = 0;
    };

    // Lower value = higher priority (picked first).
    enum class Priority
    {
        ActivePane = 0,
        OtherPane = 1,
        TexturePrefetch = 2,
        Thumbnail = 3,
        Count = 4,
    };

    ResourceManager() = default;
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Start the pool (threadCount 0 = auto from hardware_concurrency). The UI
    // wake token lets a worker request a redraw when it posts a completion.
    void Start(unsigned threadCount = 0);
    void SetRedrawToken(std::shared_ptr<FD2D::AsyncRedrawToken> token);
    void Shutdown(); // stop + join; safe to call more than once

    // Generation-based cancellation, keyed by an opaque requester pointer.
    std::uint64_t BumpGeneration(const void* requester); // invalidate old + return new
    std::uint64_t CurrentGeneration(const void* requester) const; // 0 if unknown
    bool IsCurrent(Token token) const;
    void Cancel(const void* requester); // forget it; all its tokens go stale

    // Enqueue worker-side work; skipped at pickup if the token is already stale.
    void Submit(Priority prio, Token token, std::function<void()> work);

    // Called from worker `work` to hand a UI callback back; it runs in
    // DrainCompletions iff `token` is still current.
    void PostCompletion(Token token, std::function<void()> uiCallback);

    // UI thread, once per frame: run the ready completions.
    void DrainCompletions();

private:
    struct Job
    {
        Token token;
        std::function<void()> work;
    };
    struct Completion
    {
        Token token;
        std::function<void()> cb;
    };

    void WorkerLoop();
    bool HasJob() const;            // caller holds m_jobMutex
    Job PopHighestPriority();       // caller holds m_jobMutex, a job exists

    std::vector<std::thread> m_workers;
    std::atomic<bool> m_stop { false };

    mutable std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::array<std::deque<Job>, static_cast<std::size_t>(Priority::Count)> m_jobs;

    mutable std::mutex m_genMutex;
    std::unordered_map<const void*, std::uint64_t> m_generation;

    std::mutex m_completionMutex;
    std::deque<Completion> m_completions;

    std::shared_ptr<FD2D::AsyncRedrawToken> m_redraw;
};

} // namespace nsk
