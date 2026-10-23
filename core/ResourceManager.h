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

    // Called on the UI thread (through the completion queue) when a job's work
    // threw instead of finishing; `what` describes the exception.
    using FailureHandler = std::function<void(const std::string& what)>;

    // Enqueue worker-side work; skipped at pickup if the token is already stale.
    //
    // Failure contract: if `work` throws, the worker catches it, logs it, and
    // posts `onFailure(what)` as a completion for `token` - so it runs on the UI
    // thread, and only while the token is still current (a cancelled or
    // superseded request gets nothing, exactly like a normal completion). A
    // submitter whose requester shows a Loading state passes an onFailure that
    // delivers its regular "failed" result, so that state always ends. Without
    // an onFailure the failure is only logged. After an out-of-memory failure
    // the handling is best-effort: if posting the failure throws as well, that
    // is logged (if possible) and swallowed - the worker never terminates the
    // process.
    void Submit(Priority prio, Token token, std::function<void()> work,
                FailureHandler onFailure = {});

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
    //
    // If the parse throws, the I/O permit is returned, the in-flight entry is
    // removed, joiners receive nullptr (their promise is completed exactly
    // once), and the exception propagates to this caller - inside a pool job
    // that is the job's failure path (see Submit).
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

    // Non-RAII pair for a caller that must hand the acquire/release halves
    // to something else as independent callbacks (ResourceResolver::SetIoGate,
    // so the archive scan's own file opens share this same bound - see its
    // comment for why that's callback-based rather than a direct dependency
    // on this class). Just the counting semaphore's P/V ops: safe to call
    // from any thread, and release doesn't need to "match" a specific
    // acquire beyond being called once per acquire.
    void AcquireIoSlot(Priority prio) { IoAcquire(prio); }
    void ReleaseIoSlot() { IoRelease(); }

    // Test seam: replaces LoadNifDocument inside GetOrParseNif (e.g. a loader
    // that throws). Set before any parse is requested.
    using NifLoader = std::function<bool(NifDocument&, const std::wstring&, std::string*)>;
    void SetNifLoaderForTesting(NifLoader loader) { m_nifLoader = std::move(loader); }
    // Test seam: runs (under the cache lock) right before a parsed document is
    // added to the cache - e.g. a hook that throws bad_alloc.
    void SetNifCachePublishHookForTesting(std::function<void()> hook) { m_nifPublishHook = std::move(hook); }
    // Test seam: runs (outside the lock) when a request has joined another
    // thread's in-flight parse, right before it waits for the result.
    void SetNifJoinHookForTesting(std::function<void()> hook) { m_nifJoinHook = std::move(hook); }
    // Diagnostics: number of cached documents.
    std::size_t NifCacheSize() const;

    // Diagnostics: disk permits currently held / free.
    int IoPermitsInUse() const;
    int IoPermitsFree() const;

private:
    struct Job
    {
        Token token;
        std::function<void()> work;
        FailureHandler onFailure;
    };
    struct Completion
    {
        Token token;
        std::function<void()> cb;
    };

    void WorkerLoop();
    void RunJob(Job& job);          // runs work; routes an exception to onFailure
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
    // Caller holds m_nifMutex. Adds doc to the cache + LRU; on an allocation
    // failure leaves both unchanged and returns false (never throws).
    bool PublishNifToCache(const std::wstring& key, const NifPtr& doc,
                           std::filesystem::file_time_type stamp);

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
    NifLoader m_nifLoader; // empty = LoadNifDocument
    std::function<void()> m_nifPublishHook; // test seam, empty in the app
    std::function<void()> m_nifJoinHook;    // test seam, empty in the app

    // --- IoGate internals ----------------------------------------------------
    mutable std::mutex m_ioMutex;
    std::condition_variable m_ioCv;
    int m_ioPermits = 4;   // free permits
    int m_ioActive = 0;    // permits currently held (diagnostics)
    int m_ioPeak = 0;      // high-water mark of concurrent reads (diagnostics)
    std::array<int, static_cast<std::size_t>(Priority::Count)> m_ioWaiting {};
};

// Archive-aware NIF load. A `path` that points inside a BSA/BA2 archive (e.g.
// "...\foo.ba2\meshes\x.nif", as produced by the ThumbnailStrip's VFS listing)
// is read through Floar's virtual filesystem and parsed from memory; a plain
// filesystem path falls through to NifDocument::loadFromFile. Lives here (the
// app layer links Floar) rather than in NifDocument so the parser stays free of
// the archive dependency - NifValidate reuses NifDocument without linking Floar.
bool LoadNifDocument(NifDocument& doc, const std::wstring& path, std::string* error);

} // namespace nsk
