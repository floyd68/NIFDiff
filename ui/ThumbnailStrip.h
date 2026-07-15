// ThumbnailStrip.h - a folder browser that renders each .nif to a small 3D
// thumbnail and lists them in a scrollable side strip (FICture2's
// ThumbnailPane + AsyncThumbLoader equivalent, tracked as todo #12).
//
// Enabled by the render split (RenderDevice + RenderTarget + RenderMeshCache):
// the strip owns ONE small offscreen RenderTarget and reuses the app-wide
// shared RenderDevice to draw each model headlessly, then copies the result
// into a per-thumbnail texture wrapped as an ID2D1Bitmap1 for display. No
// extra device/shaders/IBL - those are shared.
//
// Threading: the heavy per-file work (NifDocument::loadFromFile + SceneBuilder
// ::build, both free of global state) runs on a background worker thread; only
// the D3D render-to-target + copy stays on the UI thread (immediate context).
// The worker pushes parsed scenes to a ready queue and wakes the UI via an
// AsyncRedrawToken; a generation counter cancels stale work when the folder
// changes. Clicking a thumbnail activates it (the owner loads it into a pane).
#pragma once

#include "../core/NifTypes.h"        // Vector3 (ParsedThumb::center)
#include "../core/ResourceResolver.h"
#include "../render/RenderDevice.h"
#include "../render/RenderTarget.h"
#include "../render/RenderMeshCache.h"
#include "../render/TextureRepository.h"

#include <Wnd.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace FD2D { class AsyncRedrawToken; }

namespace nsk
{

class NifDocument;

class ThumbnailStrip : public FD2D::Wnd
{
public:
    // Vertical = a fixed-width column of stacked cards (docks Left/Right);
    // Horizontal = a fixed-height row of cards (docks Top/Bottom). The scroll
    // axis, hit-test and Measure axis all follow this.
    enum class Orientation { Vertical, Horizontal };

    explicit ThumbnailStrip(const std::wstring& name);
    ~ThumbnailStrip() override; // stops + joins the background worker

    // Layout orientation; the owner must dock the strip on the matching edge.
    void SetOrientation(Orientation o);
    Orientation GetOrientation() const { return m_horizontal ? Orientation::Horizontal : Orientation::Vertical; }

    // Shared, app-owned; must outlive the strip. Set before attach/use.
    void SetRenderDevice(RenderDevice* device) { m_renderDevice = device; }
    void SetResourceResolver(ResourceResolver* resolver) { m_resolver = resolver; }
    void SetTextureRepository(TextureRepository* repository) { m_textureRepository = repository; }

    // Follow a pane's open .nif: list its folder (FICture2's ThumbnailPane
    // model) - sibling .nif files as thumbnails, subfolders and an "Up" tile
    // as navigable icons - with nifPath's card highlighted as the current
    // file. Empty path clears the strip (the active pane has nothing loaded).
    // Re-lists only when the folder actually changed; otherwise just moves the
    // highlight. Folder/Up tiles navigate the strip in place (no pane load).
    void ShowForFile(const std::wstring& nifPath);
    const std::wstring& Folder() const { return m_folder; }
    bool HasContent() const { return !m_entries.empty(); }

    // Master on/off: when disabled the strip collapses (Measure 0, no paint)
    // and thumbnail generation stops (the per-frame loader idles), while the
    // folder + any thumbnails already rendered are kept for a later re-enable.
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Keyboard stepping helpers (act on the strip's current listing):
    // StepFile returns the .nif `delta` cards from the highlighted file (wraps;
    // first/last when nothing is highlighted); EdgeFile returns the first
    // (last=false) or last file. Both empty if the folder has no files - the
    // owner loads the returned path. NavigateUp browses to the parent folder.
    std::wstring StepFile(int delta) const;
    std::wstring EdgeFile(bool last) const;
    void NavigateUp();

    // Fires when a thumbnail is clicked, with its full .nif path.
    void SetOnActivated(std::function<void(const std::wstring&)> handler) { m_onActivated = std::move(handler); }

    // Full path of the thumbnail under the cursor, so Backplate's hover
    // tooltip reveals it (the card label only shows the file name, ellipsized).
    std::wstring TooltipText() const override;

    void OnAttached(FD2D::Backplate& backplate) override;
    FD2D::Size Measure(FD2D::Size available) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    // File = a .nif rendered to a 3D thumbnail; Folder = a subfolder icon that
    // navigates the strip into it; Up = the ".." tile to the parent folder.
    enum class EntryKind { File, Folder, Up };

    struct Entry
    {
        EntryKind kind = EntryKind::File;
        std::wstring path;      // File: .nif path; Folder/Up: target directory
        std::wstring name;      // label (file/folder name, or "..")
        bool rendered = false;  // 3D pass produced a texture (File only)
        bool failed = false;    // parse/build failed - show a placeholder
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;   // persistent copy of the render
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;   // D2D view of tex (built lazily in the D2D pass)
    };

    // A background-parsed scene ready for the UI thread to render to a
    // thumbnail. Holds the doc alive because the meshes borrow its geometry.
    struct ParsedThumb
    {
        std::uint64_t generation = 0;
        std::size_t index = 0;      // into m_entries (valid while generation matches)
        bool failed = false;
        std::shared_ptr<NifDocument> doc;
        std::vector<RenderMesh> meshes;
        Vector3 center { 0.0f, 0.0f, 0.0f };
        float radius = 1.0f;
    };
    // One queued parse job: file path (a copy - the worker never touches
    // m_entries) tagged with the generation it belongs to.
    struct ParseJob
    {
        std::uint64_t generation = 0;
        std::size_t index = 0;
        std::wstring path;
    };

    // Re-list a directory: Up tile (if any parent) + subfolders + *.nif files,
    // with selectPath's file card highlighted. Bumps the generation, drops
    // stale queued/ready work, and enqueues the new files for the worker.
    // Args are taken BY VALUE: the first thing this does is clear m_entries, so
    // a caller passing an entry's own path/name (a folder/Up tile) would be
    // handing us a reference into the vector we are about to free.
    void NavigateTo(std::wstring folder, std::wstring selectPath);
    // Draws a folder / up-arrow glyph inside rc for Folder/Up tiles.
    void DrawFolderIcon(ID2D1RenderTarget* target, const D2D1_RECT_F& rc, bool up) const;

    // Queues every not-yet-rendered .nif entry for the worker (no-op when the
    // strip is disabled). Called after a re-list and on re-enable.
    void EnqueuePending();
    // Background worker: parses + builds queued files, publishes to m_ready,
    // wakes the UI. EnsureWorker starts it lazily; the dtor stops + joins it.
    void EnsureWorker();
    void StopWorker();
    void WorkerLoop();
    // UI thread: render one background-parsed scene into m_thumbTarget and copy
    // it into the entry's persistent texture (immediate context).
    void RenderParsedThumb(Entry& entry, ParsedThumb& parsed);
    // Builds the D2D bitmap for an entry whose texture exists but bitmap doesn't.
    void EnsureBitmap(Entry& entry);
    void EnsureTextFormat();
    int CardAtPoint(const POINT& pt) const; // -1 when none
    // Geometry helpers, orientation-aware. thumb = square thumbnail side;
    // cardMain = per-card size along the scroll axis; leadGutter = space before
    // the first card (the header in vertical mode).
    float ThumbSide() const;
    float CardMain() const;
    float LeadGutter() const;
    float ContentExtent() const; // total size along the scroll axis
    void ClampScroll();

    RenderDevice* m_renderDevice = nullptr;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    RenderTarget m_thumbTarget;   // reused for every thumbnail render
    RenderMeshCache m_thumbCache; // cleared per model

    std::wstring m_folder;       // directory currently listed
    std::wstring m_currentFile;  // active pane's .nif, highlighted when present
    std::vector<Entry> m_entries;
    bool m_enabled = true;          // master on/off (see SetEnabled)
    bool m_horizontal = false;
    float m_scroll = 0.0f;          // offset along the scroll axis (Y or X)
    int m_hoverCard = -1;

    // Background parse worker. m_generation is bumped on every NavigateTo;
    // jobs/results tagged with an older generation are dropped. m_jobs is the
    // worker's input, m_ready its output; both are drained under their mutex.
    std::thread m_worker;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<ParseJob> m_jobs;
    std::mutex m_readyMutex;
    std::deque<ParsedThumb> m_ready;
    std::atomic<std::uint64_t> m_generation { 0 };
    std::atomic<bool> m_workerStop { false };
    std::shared_ptr<FD2D::AsyncRedrawToken> m_redrawToken; // UI wake from worker

    std::function<void(const std::wstring&)> m_onActivated;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
};

} // namespace nsk
