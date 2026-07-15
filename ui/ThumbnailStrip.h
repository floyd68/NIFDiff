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
class ResourceManager;

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
    // The shared async load pool (parses/builds off the UI thread). Required
    // for thumbnails to generate; must outlive the strip.
    void SetResourceManager(ResourceManager* manager) { m_manager = manager; }

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

    // Strip thickness (card size) presets - the fixed dimension the strip
    // takes along its docked edge (height when horizontal). Bigger = larger
    // thumbnails but less room for the 3D view.
    static constexpr float kSizeSmall = 150.0f;
    static constexpr float kSizeMedium = 196.0f;
    static constexpr float kSizeLarge = 248.0f;
    void SetFixedExtent(float extent);
    float FixedExtent() const { return m_fixedExtent; }

    // Live resize by dragging the grip on the strip's inner edge. Fires while
    // dragging (committed=false, so the owner can mirror the size onto the
    // other panes live) and once on release (committed=true, to persist it).
    void SetOnResize(std::function<void(float extent, bool committed)> handler) { m_onResize = std::move(handler); }

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
        float aspect = 1.0f;    // rendered thumbnail w/h (drives the card width)
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
        std::shared_ptr<const NifDocument> doc; // shared with the pane/cache (parsed once)
        std::vector<RenderMesh> meshes;
        // Framing computed on the worker: a rolled view + a tight orthographic
        // projection around the non-hidden geometry (equal margins), plus the
        // resulting w/h aspect for the non-square render target and card.
        Matrix4 view;
        Matrix4 proj;
        Vector3 eyePos { 0.0f, 0.0f, 0.0f };
        float aspect = 1.0f;
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

    // Submits every not-yet-rendered .nif entry to the shared ResourceManager
    // pool (no-op when disabled). Called after a re-list and on re-enable.
    void EnqueuePending();
    // Pool thread (STATIC - must not touch a possibly-dead strip): parse +
    // build + framing for one file, filling `out`. The parse goes through the
    // manager's shared NifCache so the doc is reused (pane + siblings parse once).
    static void BuildParsedThumb(ResourceManager* manager, const std::wstring& path,
                                 ParsedThumb& out);
    // Pool thread: pick the thumbnail camera (slight yaw) and a tight
    // orthographic frustum around the non-hidden geometry with equal margins,
    // filling out.view/proj/eyePos/aspect. `minB`/`maxB` are the world bounds.
    static void ComputeThumbFraming(const std::vector<RenderMesh>& meshes,
                                    const Vector3& minB, const Vector3& maxB,
                                    ParsedThumb& out);
    // UI thread (completion): queue a parsed scene for OnRenderD3D to draw.
    void AcceptParsed(std::shared_ptr<ParsedThumb> parsed);
    // UI thread: render one parsed scene into m_thumbTarget and copy it into
    // the entry's persistent texture (immediate context).
    void RenderParsedThumb(Entry& entry, ParsedThumb& parsed);
    // Builds the D2D bitmap for an entry whose texture exists but bitmap doesn't.
    void EnsureBitmap(Entry& entry);
    void EnsureTextFormat();
    int CardAtPoint(const POINT& pt) const; // -1 when none
    bool InResizeGrip(const POINT& pt) const; // over the drag-to-resize edge
    // Geometry helpers, orientation-aware. ThumbSide = the thumbnail's fixed
    // dimension (height when horizontal); CardExtent = one card's size along
    // the scroll axis (VARIES per card now: File cards are ThumbSide*aspect
    // wide, folder/Up tiles are square); leadGutter = space before the first
    // card; CardOffset = accumulated offset of card i along the scroll axis.
    float ThumbSide() const;
    float CardExtent(std::size_t index) const;
    float CardOffset(std::size_t index) const;
    float LeadGutter() const;
    float ContentExtent() const; // total size along the scroll axis
    void ClampScroll();

    RenderDevice* m_renderDevice = nullptr;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    ResourceManager* m_manager = nullptr; // shared async load pool
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    RenderTarget m_thumbTarget;   // reused for every thumbnail render
    RenderMeshCache m_thumbCache; // cleared per model

    std::wstring m_folder;       // directory currently listed
    std::wstring m_currentFile;  // active pane's .nif, highlighted when present
    std::vector<Entry> m_entries;
    bool m_enabled = true;          // master on/off (see SetEnabled)
    float m_fixedExtent = kSizeMedium; // strip thickness (see SetFixedExtent)
    bool m_horizontal = false;
    float m_scroll = 0.0f;          // offset along the scroll axis (Y or X)
    int m_hoverCard = -1;

    // Drag-to-resize state (grip on the strip's inner edge).
    bool m_resizing = false;
    bool m_gripHover = false;
    float m_dragStartMouse = 0.0f;  // cursor pos on the resize axis at drag start
    float m_dragStartExtent = 0.0f; // m_fixedExtent at drag start
    std::function<void(float, bool)> m_onResize;

    // Async parse via the shared ResourceManager pool. m_gen (the manager's
    // per-strip generation) is bumped on every NavigateTo / disable; results
    // tagged with an older generation are dropped by the manager. m_ready is
    // the UI-side queue of parsed scenes awaiting RenderParsedThumb - it is
    // only touched on the UI thread (completions + OnRenderD3D), so no lock.
    std::uint64_t m_gen = 0;
    std::deque<ParsedThumb> m_ready;

    std::function<void(const std::wstring&)> m_onActivated;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
};

} // namespace nsk
