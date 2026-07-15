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
#include <filesystem>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FD2D { class AsyncRedrawToken; }

namespace nsk
{

class NifDocument;

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

    // --- NIF parse cache + in-flight coalescing (migration phase 2) ----------
    // Get-or-parse a .nif, shared and de-duplicated: the same file is parsed
    // exactly once even when several panes/thumbnails ask for it concurrently
    // (a second request for a file already parsing joins the first instead of
    // starting its own). Callable from a worker OR the UI thread; it BLOCKS the
    // caller until the parse (its own, or another thread's in-flight one) is
    // done. Returns nullptr if the file fails to load. Thread-safe.
    //
    // The returned shared_ptr keeps the doc alive; a holder therefore pins its
    // cache entry against eviction. The cache re-parses when the file's
    // last-write time changes.
    //
    // `prio` orders IoGate permits (higher priority gets a freed disk permit
    // first); `throttle` gates the disk read through the IoGate. Background
    // (pool-thread) callers throttle so they don't thrash the disk; a
    // synchronous UI-thread load passes throttle=false so it is never made to
    // wait behind background reads.
    std::shared_ptr<const NifDocument> GetOrParseNif(const std::wstring& path,
                                                     std::string* error = nullptr,
                                                     Priority prio = Priority::Thumbnail,
                                                     bool throttle = true);
    void ClearNifCache(); // drop cache references (docs held elsewhere survive)

    // IoGate: cap concurrent background disk reads (default 4). Fewer permits
    // than pool threads keeps disk-bound jobs from thrashing; the surplus
    // threads run CPU-only work (scene build, decode) meanwhile.
    void SetIoPermits(int permits);

    // RAII disk permit for background reads OTHER than GetOrParseNif (e.g. a
    // worker decoding a texture): acquire around the read so all disk-touching
    // pool work shares one bound. Blocks on construction until a permit is
    // granted (priority-ordered), releases on destruction.
    class IoPermit
    {
    public:
        IoPermit(ResourceManager& mgr, Priority prio) : m_mgr(mgr) { m_mgr.IoAcquire(prio); }
        ~IoPermit() { m_mgr.IoRelease(); }
        IoPermit(const IoPermit&) = delete;
        IoPermit& operator=(const IoPermit&) = delete;
    private:
        ResourceManager& m_mgr;
    };

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

    // --- NIF cache internals -------------------------------------------------
    using NifPtr = std::shared_ptr<const NifDocument>;
    static std::wstring NormalizeNifKey(const std::wstring& path);
    void EvictNif(); // caller holds m_nifMutex; trims the LRU (pinned docs kept)

    // Priority-aware disk-permit gate. A freed permit is granted to the
    // highest-priority (lowest enum) waiter, so a pane load out-prioritizes a
    // thumbnail once both compete for the disk.
    int IoAcquire(Priority prio); // blocks until granted; returns held-permit count
    void IoRelease();

    struct NifEntry
    {
        NifPtr doc;
        std::filesystem::file_time_type mtime {};
        std::list<std::wstring>::iterator lru; // position in m_nifLru
    };

    static constexpr std::size_t kNifCacheCap = 64; // LRU size (pinned docs exempt)

    mutable std::mutex m_nifMutex;
    std::unordered_map<std::wstring, NifEntry> m_nifCache; // key = NormalizeNifKey
    std::list<std::wstring> m_nifLru;                      // MRU at front
    // Files currently being parsed: joiners wait on the shared_future instead
    // of starting a duplicate parse.
    std::unordered_map<std::wstring, std::shared_future<NifPtr>> m_nifInFlight;

    // --- IoGate internals ----------------------------------------------------
    mutable std::mutex m_ioMutex;
    std::condition_variable m_ioCv;
    int m_ioPermits = 4;   // free permits
    int m_ioActive = 0;    // permits currently held (diagnostics)
    int m_ioPeak = 0;      // high-water mark of concurrent reads (diagnostics)
    std::array<int, static_cast<std::size_t>(Priority::Count)> m_ioWaiting {};
};

} // namespace nsk
