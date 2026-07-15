# Unified Resource & Load Manager — Design

**Status:** Proposal (pre-implementation) · **Date:** 2026-07-16

## 1. Context & problem

Loading in NIFDiff is currently spread across three independent, uncoordinated
mechanisms:

| Subsystem | Threading | Notes |
|---|---|---|
| `ResourceResolver` (archive scan) | `std::async` (background scan) + `std::execution::par` (parallel BSA open/probe) | The only truly backgrounded step. |
| `TextureRepository::Prefetch` | `std::execution::par` (read + DDS decode + SRV create) | Runs **synchronously on the UI thread** — resolve is serial, decode/upload fan out but the UI waits for the whole burst. Lazy `GetOrLoad` is fully UI-thread. |
| `ThumbnailStrip` | Its own per-strip `std::thread` pool (≤3 each → up to ~15 for 8 panes) | Parses off-thread; renders on the UI thread. |

Consequences observed in practice:

- **UI stalls.** Texture loading runs on the UI thread; the archive scan can
  block the UI via `WaitForArchiveScan` (root cause of the "blank window for
  ~5.9 s" Debug startup — see `startup-profiling` memory).
- **Uncoordinated disk I/O.** The archive scan, texture prefetch, and thumbnail
  workers all read the disk at once with no global throttle; they compete
  (thumbnail `.nif` reads vs. BSA reads during startup).
- **Redundant work.** Textures are de-duplicated well (`TextureRepository`
  pools by resolved-bytes `sourceKey`, process-wide). **NIF parses are not:**
  each strip parses its folder independently, so N panes on the same folder
  re-parse the same files N times, and a pane's own file is parsed twice (once
  for the viewport, once for its thumbnail).
- **Ad-hoc workarounds** only: the startup pump-wait and the thumbnail
  first-render deferral — no general mechanism.

There is **no unified async thread/resource manager** that guarantees
"UI never blocks · disk I/O doesn't contend · nothing loads twice."

## 2. Goals / non-goals

**Goals**
- One shared, bounded worker pool; retire the three ad-hoc mechanisms.
- Keep the UI thread free of CPU-heavy and disk work; only immediate-context
  GPU work runs on it.
- Coordinate disk I/O so subsystems don't compete (priority-aware throttle).
- De-duplicate and reuse: never load/parse the same resource twice, including
  in-flight coalescing.
- First-class support for **bursts of open requests** (IPC / drag&drop):
  create up to 8 panes and set their UI immediately, then fill in models as
  loads complete.

**Non-goals**
- Making `ID3D11DeviceContext` (immediate context) thread-safe — it stays
  UI-only.
- Making the archive readers thread-safe — we serialize their access instead.
- A general job graph engine; a priority queue + explicit dependencies is
  enough.

## 3. Threading constraints (what runs where)

| Must run on the UI thread | Safe on worker threads |
|---|---|
| `RenderDevice::RenderScene` (immediate-context draws) | `NifDocument::loadFromFile` (parse) |
| Thumbnail render-to-target + `CopyResource` | `SceneBuilder::build` |
| Final scene swap into a live viewport (`SetDocument`) | Texture file read + DDS decode |
| | GPU **resource creation**: `CreateTexture2D` / `CreateShaderResourceView` / `CreateBuffer` (device creation is free-threaded) |
| | Archive **resolve** — worker-OK **only via a serialized lane** (readers are not thread-safe) |

**Guiding rule:** *CPU + GPU-resource-creation on workers; only
immediate-context drawing on the UI thread.* The thumbnail path already does
this split; the design generalizes it to pane loads and textures.

## 4. Architecture

A single `ResourceManager`, owned by the app and injected into subsystems
(like `RenderDevice` / `TextureRepository` today):

```
ResourceManager (app-owned, one instance)
├─ ThreadPool        fixed size ≈ hw_concurrency; priority queue; per-task cancel token
├─ IoGate            counting semaphore of K disk permits + a single serialized Archive lane
├─ Caches
│   ├─ TextureRepository   (existing) pool by ResourceBytes::sourceKey  + in-flight coalescing
│   └─ NifCache            LRU of shared_ptr<NifDocument> by canonical path (+ mtime)  + in-flight coalescing
└─ CompletionQueue   worker→UI results; wakes UI via Backplate::AsyncRedrawToken
```

This replaces `std::async` (resolver), the implicit `std::execution::par`
pools, and the per-strip `std::thread` pools with one pool + one I/O gate.

## 5. Job model

Every load is a `Job` with:

- **Priority:** `P0` active pane's current file · `P1` other panes' initial
  loads · `P2` texture prefetch for loaded scenes · `P3` thumbnails.
- **Dependencies:** e.g. a texture-resolve job depends on "archive scan done."
  The scan becomes a job; texture jobs are its successors. This replaces the
  blocking `WaitForArchiveScan` with **ordering** — the UI never waits.
- **Cancellation token:** a `{requester, generation}` pair. Bumping a
  requester's generation (folder change, pane retarget, pane close) drops its
  queued/in-flight results (the thumbnail strip's generation counter,
  generalized).

The pool pulls the highest-priority runnable job whose dependencies are met and
whose generation is still current.

## 6. Resource pipelines (worker stage → UI stage)

| Resource | Worker (off-thread) | UI (immediate context) |
|---|---|---|
| **NIF (pane)** | parse + `SceneBuilder::build` (+ GPU buffer creation) | `SetDocument` — swap scene; draw next frame |
| **Texture** | resolve (Archive lane) → read + decode + SRV create | none — SRV lands in the pool, ready |
| **Thumbnail** | parse + build + framing | `RenderScene` + `CopyResource` |

Result: **pane loads stop blocking the UI** (today the whole load is a UI-thread
synchronous call — the ~1.3 s load freeze at startup). Only the final
`SetDocument` touches the UI.

## 7. I/O coordination — `IoGate`

The bottleneck is the **disk**, not the CPU (one BSA opens in ~2 s). So:

- **Bound concurrent disk jobs** with a counting semaphore of `K` permits
  (K ≈ 2–4). Many worker threads may exist, but at most K read the disk at
  once — the archive scan, texture reads, and NIF reads no longer thrash.
- **Priority-aware permits:** grant permits in priority order so a P0 pane load
  gets the disk before P3 thumbnails.
- **Single Archive lane:** archive-reader access is serialized on one lane
  (readers aren't thread-safe). This structurally removes the startup
  "thumbnail `.nif` reads vs. BSA reads" contention without threading the
  reader.

## 8. Caching / de-duplication

- **Textures:** keep `TextureRepository` (process-wide `sourceKey` pool) and add
  **in-flight coalescing** — a request for a source already loading joins the
  existing future instead of starting a second load.
- **NIF parses (new `NifCache`):** an LRU of `shared_ptr<NifDocument>` keyed by
  canonical path (+ mtime for invalidation). **Both** the pane viewport load and
  the thumbnail go through it → a file is parsed **once**. `SceneBuilder::build`
  output borrows the doc's geometry, so caching the doc is enough (build is
  ~2–5 ms vs. parse ~15–175 ms). In-flight coalescing applies here too.
- Net effect: opening the same mesh into 8 panes parses it **once**; a pane's
  thumbnail reuses the pane's own parsed doc.

## 9. UI delivery

Workers push results to `CompletionQueue` and call
`Backplate::AsyncRedrawToken::RequestAsyncRedraw()` (already used by the
thumbnail strip). Each frame the UI drains the queue and runs only the
immediate-context stage, applying a result **only if its generation is still
current**. This generalizes today's `ThumbnailStrip::m_ready` pattern to the
manager level.

## 10. Open-request flow & IPC-burst scenario

A burst of open requests (IPC forwards from other instances, multi-file
drag&drop, command line) can arrive within ms–hundreds of ms. The requirement:
**receive the requests, create up to 8 panes and set their file names / UI
immediately (no file I/O), then let each pane fill in its model as the
`ResourceManager` finishes loading.**

### 10.1 New component: `OpenRequestCoordinator` (UI thread)

All open sources — IPC (`IpcQueue` / `DrainIpcOpenQueue` / `ipcUiWindow`),
drag&drop, dialog, command line, session restore — normalize to
`OpenRequest{ path, intent, targetPaneHint, generation }` and funnel through it.

**Three phases:**

```
① Ingest & Coalesce (UI, µs)
     dedup identical paths · cap at kMaxPanes(8) · keep drop-zone intent
     (replace pane X / insert-after / append)
② Fast UI Alloc (UI, NO file I/O)
     create/reuse ≤8 panes · set path label + "Loading" state · ONE relayout per batch
③ Async Load (ResourceManager)
     per-pane NIF job → fills in via SetDocument as each completes
```

- **Same-cycle merge:** `DrainIpcOpenQueue` drains the whole queue in one UI
  cycle, so a same-ms burst is one batch. A short **debounce (~32–64 ms)** timer
  merges requests spread across cycles, preventing repeated relayout.
- **8-cap policy:** overflow is ignored with a toast by default (FIFO-replace is
  a configurable alternative). Drop-zone intent is honored when placing panes.

### 10.2 Pane load-state machine

```
Empty → PathSet(Loading, gen=G) → Ready
                     └───────────→ Failed
```

- **PathSet(Loading):** path label shown, viewport renders a **placeholder**
  (file name / spinner / grid). No document yet.
- **Ready:** the completion callback for `{pane, gen}` arrives → `SetDocument`
  → the model draws next frame.
- Rendering branches on state, so **"pane exists with a name" is decoupled from
  "model ready."**

### 10.3 IPC response timing

- The IPC **ack is sent at "accept" (phase ② done: pane created + load
  queued)** — *not* when the model finishes loading. The forwarding client gets
  a fast ack and exits; combined with the existing 3-Waiting-ack cap, IPC stays
  responsive under bursts.
- Rejected (cap exceeded / shutting down) → immediate reject ack.

### 10.4 Ordering, retarget & cancellation

- **Display order = request / drop-zone order:** phase ② fixes each pane's slot
  and its `{pane, gen}` token, so out-of-order load completions still land in
  the correct pane.
- **Retarget cancel (burst-critical):** if a pane is re-pointed from file A to B
  mid-load, bump the pane's generation → A's job/result is dropped. Pane close
  invalidates the generation too.
- **Dedup:** the same file opened into several panes parses once (NifCache +
  coalescing); the panes share one `shared_ptr<NifDocument>` (only their
  per-viewport GPU meshes differ).

## 11. Migration plan (phased, low-risk → high-impact)

1. **ThreadPool + CompletionQueue.** Move the thumbnail workers onto it (they're
   already generation-cancellable); drop the per-strip threads. *Pure refactor.*
2. **NifCache + in-flight coalescing.** Route pane loads and thumbnails through
   it. *Kills double-parse and cross-pane re-parse — immediately noticeable.*
3. **IoGate** (semaphore + Archive lane). *Retires the startup pump-wait and the
   thumbnail-defer hacks.*
4. **Async texture Prefetch** — submit to the pool, deliver via the completion
   queue; keep the `sourceKey` pool + add in-flight dedup. *UI stops blocking on
   prefetch.*
5. **Pane-load parse on workers** — only `SetDocument` on the UI. *Removes the
   load freeze.*
6. **Replace `WaitForArchiveScan` blocking with dependency ordering.**
7. **`OpenRequestCoordinator` + pane state machine.** Validate first with
   multi-file drag&drop (already a source), then wire `DrainIpcOpenQueue` in.
8. **ack-on-accept** for IPC + 8-cap + retarget cancellation.
9. **Placeholder rendering** for the Loading state.

Each phase is independently shippable and testable.

## 12. Test scenarios

- **IPC burst of 8 opens:** all 8 panes appear with file names in <100 ms;
  models fill in independently; the IPC client acks immediately.
- **Same file × 8:** parsed **once** (cache + coalescing); 8 panes share it.
- **Retarget / close mid-load:** the superseded load's result is discarded (no
  wrong-pane model).
- **> 8 requests:** cap policy applied + IPC reject ack.
- **Debug startup, 5-folder session:** window visible ~t+1.3 s and responsive;
  no disk contention between the scan and thumbnails.

## 13. Risks & open questions

- **Pool sizing vs. IoGate `K`:** many threads but few I/O permits — pick `K`
  from measured disk behavior (SSD vs. HDD, Defender on Program Files).
- **GPU resource creation off-thread:** confirmed free-threaded, but validate
  the D3D debug layer stays quiet under concurrent creation.
- **NifCache invalidation:** mtime-based; decide eviction size and whether a
  pane holding a `shared_ptr` pins a cache entry (it should, until swapped).
- **Ordering of `SetDocument` vs. an in-flight retarget:** the generation check
  covers it, but needs a focused test.

## 14. Current code touch-points

- `core/ResourceResolver` — expose the scan as a manager job/dependency; the
  Archive lane wraps its readers. (`IsArchiveScanReady()` already added.)
- `render/TextureRepository` — keep the pool; add in-flight coalescing; make
  `Prefetch` submit to the pool.
- `ui/ThumbnailStrip` — drop its own pool; submit parse jobs to the manager;
  keep the UI-side `RenderParsedThumb`.
- `ui/NifComparePane` / `NifCompareView` — pane load-state machine; loads go
  through the manager; `DrainIpcOpenQueue` → `OpenRequestCoordinator`.
- `app/NIFDiffApp` — construct the `ResourceManager`; retire the startup
  pump-wait once dependency ordering lands.

## Appendix A — API sketch

Illustrative, not final. All public entry points are called from the UI thread;
callbacks are delivered on the UI thread via the completion drain.

```cpp
// A cancellation handle. Bumping the generation invalidates prior submissions.
struct LoadToken { const void* requester; uint64_t generation; };

enum class LoadPriority { ActivePane, OtherPane, TexturePrefetch, Thumbnail };

class ResourceManager {
public:
    // --- lifecycle ---
    void Start(ID3D11Device* device, ResourceResolver* resolver); // sizes the pool
    void Shutdown();                                              // drains + joins

    // --- NIF: parse+build off-thread, cached + coalesced by canonical path ---
    // onReady runs on the UI thread; skipped if the token generation is stale.
    void RequestNif(std::wstring path, LoadPriority prio, LoadToken tok,
                    std::function<void(std::shared_ptr<NifDocument>)> onReady,
                    std::function<void()> onFailed = {});

    // --- Textures: resolve (archive lane) + decode + SRV, pooled by sourceKey ---
    void PrefetchTextures(std::vector<std::string> relPaths, std::wstring nifDir,
                          LoadPriority prio, LoadToken tok);
    // Synchronous pool hit (UI thread) once prefetched; else lazy load.
    TextureRepository::Entry* GetOrLoadTexture(const std::string& rel,
                                               const std::wstring& nifDir);

    // --- cancellation: drop everything queued/in-flight for this requester ---
    void Cancel(const void* requester);            // e.g. pane closed
    // (retarget = caller bumps generation, then re-requests with the new one)

    // --- UI pump: run each frame; applies completed results on the UI thread ---
    void DrainCompletions();                       // called from the render loop

    // --- dependencies: texture jobs wait on this; no UI blocking ---
    bool IsArchiveScanReady() const;               // moved from ResourceResolver
};
```

Notes:
- `RequestNif` consults `NifCache` first (hit → `onReady` next drain); on a
  miss it checks the in-flight map (join) before enqueuing a parse job.
- The pool worker acquires an `IoGate` permit around the disk read only, then
  releases it before the CPU-only build, so permits track *disk* concurrency,
  not thread count.
- `DrainCompletions` is the single UI-side apply point; each result carries its
  `LoadToken` and is dropped if `tok.generation != current(requester)`.

## Appendix B — IPC 8-open burst, across threads

```
 IPC thread        UI thread (OpenRequestCoordinator)          ResourceManager pool
 ─────────         ────────────────────────────────           ────────────────────
 accept f1..f8  →  enqueue OpenRequest x8 (IpcQueue)
   (ack each                │
    on ACCEPT)      DrainIpcOpenQueue (one cycle):
                    ① dedup + cap8 + intent
                    ② create/reuse ≤8 panes,
                       set path label + Loading,
                       ONE relayout
                    ③ for each pane:
                       RequestNif(prio, {pane,gen})   ──────►  parse+build (IoGate-gated read)
                                │                                        │  (P0 active first)
                    window shows 8 named panes                 complete f_k ─┐
                    (placeholders) ~immediately                              │
                                │                              RequestAsyncRedraw
                    DrainCompletions (per frame):  ◄───────────────────────┘
                       if gen current → SetDocument(pane_k)
                       (panes fill in independently, any order)
```

Retarget during the burst: if pane 3 is re-pointed f3→f9, the coordinator bumps
pane 3's generation and calls `RequestNif(f9, {pane3, gen+1})`; the in-flight
`f3` result arrives with the old generation and is discarded at
`DrainCompletions`.
