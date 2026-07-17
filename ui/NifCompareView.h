// NifCompareView.h - FICture2-style top-level layout: a dynamic 1-8 pane
// 3D compare grid plus a shared bottom control strip.
//
// Layout: outer top/bottom FD2D::SplitPanel (first = views host, second =
// NifCompareControlPanel), the same shape as FICture2's ImageBrowser
// rootSplit (ImageBrowser.cpp: ~0.85 ratio, second pane's extent clamped to
// its measured content height, ConstraintPropagation::Minimum) - not
// liteviewer's original fixed left/right SplitPanel + right-side sidebar.
// The "views host" is rebuilt on every pane add/remove via
// NifCompareSplitCoordinator::BuildEqualWidthHostTree, mirroring FICture2's
// own ImageBrowserSplitCoordinator but extended to a two-row grid: 1-4
// panes in a single row, 5-6 as 3x2, 7-8 as 4x2.
#pragma once

#include "NifComparePane.h"
#include "NifCompareControlPanel.h"
#include "NifCompareSplitCoordinator.h"
#include "IpcOpenRequest.h"
#include "../core/ResourceResolver.h"

#include <SplitPanel.h>
#include <DockPanel.h>
#include <ScrollView.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nsk
{

class NifCompareView : public FD2D::SplitPanel
{
public:
    explicit NifCompareView(const std::wstring& name);

    static constexpr std::size_t kMinPanes = 1;
    static constexpr std::size_t kMaxPanes = 8;
    static constexpr std::size_t kDefaultInitialPanes = 2;

    std::size_t PaneCount() const { return m_panes.size(); }
    ComparePane& Pane(std::size_t index) { return *m_panes[index]; }

    // The ACTIVE pane (FICture2's focused-browser equivalent): set by any
    // click inside a pane, by the 1-8 number keys, and by a drop's target
    // pane; drawn with an accent border while several panes are open. It
    // is the target for pane-context hotkeys (Ctrl+O opens into it, F12
    // screenshots it). Never null while panes exist: a stale pointer
    // (pane closed since) falls back to the first pane.
    ComparePane* ActivePane() const;
    void SetActivePane(ComparePane* pane);

    // Called by the app shell when a pane's own "Open..." button (or a
    // restored/command-line file) needs a path picked/loaded. The app shell
    // owns the actual file dialog + NifComparePane::Load() call; this view
    // just tells it *which* pane requested it.
    void SetOnPaneOpenRequested(std::function<void(NifComparePane&)> handler);

    // Fires whenever any pane successfully loads a file (every open path
    // funnels through NifComparePane::Load) - the app shell uses it to keep
    // its recent-files (MRU) list current.
    void SetOnFileOpened(std::function<void(const std::wstring&)> handler);

    // Adds a pane (up to kMaxPanes) and rebuilds the equal-width host tree.
    // Returns the new pane, or nullptr if already at kMaxPanes.
    NifComparePane* AddPane();

    // Grows or shrinks to exactly `count` panes (clamped to
    // [kMinPanes, kMaxPanes]); shrinking drops panes from the end. Intended
    // for startup/session-restore sizing BEFORE any load - unlike the Close
    // button path this removes synchronously, so it must not be called from
    // inside a pane's own event handler (see QueueClosePane's comment).
    void SetPaneCount(std::size_t count);

    // Loads `path` into the first empty pane, or into a newly added pane if
    // every existing one is occupied (and the pane count allows growing).
    // Returns false when all kMaxPanes panes are occupied or the load
    // failed. Used by the IPC drain below; safe for any other caller on the
    // UI thread too.
    bool OpenIntoBestPane(const std::wstring& path);

    // Startup two-phase pane creation (so panes appear before the archive scan
    // finishes and before any file loads):
    //  - PlaceQueuedIpcPanesNamesOnly: during the scan wait, create a named
    //    placeholder pane for each queued IPC forward WITHOUT loading (the load
    //    would block on the scan). Poll it from the wait loop.
    //  - LoadAllPendingPanes: once the scan is ready, start the async load for
    //    every named-but-unloaded pane (initial session + IPC-placed alike).
    void PlaceQueuedIpcPanesNamesOnly();
    void LoadAllPendingPanes();
    // Phases of the startup load, so the mains can parse DURING the archive scan
    // and the textures/thumbnails follow once it lands:
    void StartAllPendingLoads();     // queue every named pane's parse job (scan-safe)
    void ShowAllThumbnailStrips();   // list folders + queue thumbnails (after scan)
    void RefreshTexturesAfterScan(); // re-resolve loaded panes' textures once ready

    // Rebuild the IPC same-name snapshot from the panes' current (loaded OR
    // pending) files. Call once after the initial panes are named, so forwards
    // arriving during startup match them.
    void RefreshIpcOpenSnapshot();

    // Single-instance IPC wiring (see ui/IpcOpenRequest.h for the gate
    // semantics and threading): the IPC worker threads decide + enqueue
    // against `queue`'s snapshot; this view keeps that snapshot current
    // (UpdateIpcOpenSnapshot on every document change) and consumes the
    // accepted paths on the UI thread (DrainIpcOpenQueue). Must be set
    // before the first document loads.
    void SetIpcOpenQueue(std::shared_ptr<IpcOpenQueue> queue);

    // Opens every queued path via OpenIntoBestPane. A path that no longer
    // fits (panes filled up since it was accepted) or fails to load is
    // handed to a freshly spawned NIFDiff instance instead - the sending
    // process was already told OpenedInPane and has exited, so the file
    // must not be dropped silently. Called from the CMD_NIFDIFF_IPC_OPEN
    // broadcast and once right after startup.
    void DrainIpcOpenQueue();

    // Handles the CMD_NIFDIFF_IPC_OPEN "drain now" broadcast posted by the
    // IPC server thread (see app/NIFDiffApp.cpp's server callback).
    bool OnCommandEvent(const FD2D::CommandEvent& event) override;

    // Catches right-clicks no child consumed (viewport right-*drags* pan the
    // camera and are consumed there; plain right-clicks bubble up to here -
    // see NifViewport::OnInputEvent's MouseUp case) and requests the
    // app-level context menu, passing the pane under the click (nullptr when
    // it landed outside every pane) so the menu can offer per-pane Open/
    // Close actions. The app shell owns the actual menu (About / file
    // association / Exit live at the app level, not in this view).
    //
    // Also handles the application-wide keyboard shortcuts (KeyDown events
    // reach this view through Backplate's focused-then-broadcast routing):
    //   F              Reset View (all)      G  Grid       X  Axes
    //   W              Wireframe             H  Hidden
    //   PgUp / PgDn    cycle camera preset
    // and the pane-context ones, acting on the ACTIVE pane:
    //   1..8           select the active pane
    //   Tab / Shift+Tab  cycle the active pane
    //   R              reset this pane's camera
    //   C              focus its selected sub-mesh (whole scene when none)
    //   I              toggle the material diff panel
    //   Ctrl+O         open a file into this pane
    //   Ctrl+W         close this pane        Del  clear its document
    //   Ctrl+E         open its containing folder in Explorer
    //   F12            save a screenshot of this pane
    // (I toggles the material diff panel, T the texture inspector - see
    // DrawMaterialDiffPanel / DrawTextureInspector.)
    bool OnInputEvent(const FD2D::InputEvent& event) override;
    // Reflows the control strip to the current width and sizes its (bottom) pane
    // to the resulting wrapped height before doing the split, so a narrow window
    // grows the strip into more rows instead of clipping controls off-edge.
    void Arrange(FD2D::Rect finalRect) override;

    // Collapse/expand the bottom control strip (the chevron tab on its top
    // edge toggles it; collapsed leaves just the tab so the 3D views get the
    // full height). notify=true fires the changed callback (persistence).
    void SetControlStripCollapsed(bool collapsed, bool notify = false);
    bool ControlStripCollapsed() const { return m_controlsCollapsed; }
    void SetOnControlStripCollapsedChanged(std::function<void(bool)> handler) { m_onControlsCollapsedChanged = std::move(handler); }
    // Drains the ResourceManager's completed loads once per frame (before the
    // pane/strip children render), then propagates the D3D pass to them.
    void OnRenderD3D(ID3D11DeviceContext* context) override;

    // Explorer drag&drop (Backplate's OLE drop target, enabled by the app
    // shell via EnsureDropTargetRegistered), FICture2's drag-controller
    // semantics: hovering the left 75% of a pane REPLACES that pane's
    // document (translucent red overlay over the whole pane), the right
    // 25% INSERTS a new pane right after it (translucent green overlay
    // over that strip; falls back to replace at kMaxPanes). A multi-file
    // drop places the first file per the drop point and the rest into
    // empty/new panes in order. Drags outside every pane are declined.
    bool OnFileDrag(const std::wstring& path, const POINT& clientPt, FD2D::FileDragVisual& outVisual) override;
    bool OnFileDropPaths(const std::vector<std::wstring>& paths, const POINT& clientPt) override;
    void OnFileDragLeave() override;
    // Draws the drag overlay above the pane content (second D2D pass).
    void OnRenderOverlay(ID2D1RenderTarget* target) override;

    // `clientPt` is in window client coordinates.
    void SetOnContextMenuRequested(std::function<void(POINT clientPt, NifComparePane* pane)> handler);

    // F12 shortcut target: the app shell owns the Save dialog + the actual
    // write (same flow as the context menu's "Save Pane Screenshot...").
    void SetOnScreenshotRequested(std::function<void(NifComparePane&)> handler);

    // Context-menu actions on a specific pane, invoked by the app shell's
    // menu handler. RequestOpenPane forwards to the SetOnPaneOpenRequested
    // handler (the app owns the file dialog); RequestClosePane goes through
    // the same deferred-removal path as the old per-pane Close button (see
    // QueueClosePane) so it is safe to call from a menu callback too.
    void RequestOpenPane(NifComparePane& pane);
    void RequestClosePane(NifComparePane& pane);

    // Splitter-ratio persistence for the SESSION only (FICture2's
    // Capture/ApplyHorizontalSplitRatios pattern, extended to this view's
    // two-row grid): the pre-order list of every SplitPanel ratio inside the
    // pane host tree. The app shell saves these on close and re-applies them
    // after restoring the session's files, so a dragged layout comes back on
    // relaunch. Adding/removing a pane deliberately does NOT carry ratios over
    // (see RebuildHostTree) - it resets to equal widths, which is what the
    // same-named-mesh compare workflow wants. Applying is positional: it lines
    // up exactly when the restored pane count matches what was saved.
    std::vector<float> CaptureSplitRatios() const;
    void ApplySplitRatios(const std::vector<float>& ratios);

    void SetResourceResolver(ResourceResolver* resolver);

    // Shared cross-pane texture pool; also cleared by InvalidateTextureCaches.
    // Must outlive this view.
    void SetTextureRepository(TextureRepository* repository);

    // The single app-wide render core (shaders/states/IBL), shared by every
    // pane's viewport. Must outlive this view.
    void SetRenderDevice(RenderDevice* device);

    // The shared async load pool (thumbnail parsing off the UI thread). Drained
    // once per frame in OnRenderD3D. Must outlive this view.
    void SetResourceManager(ResourceManager* manager);

    // Per-pane thumbnail strip master on/off (every pane owns its own strip;
    // this toggle applies to all of them at once), mirrored by the VIEW-group
    // "Thumbnails" checkbox. The owner persists the choice via
    // SetOnThumbnailStripEnabledChanged; the context-menu item flips the same
    // checkbox through ToggleThumbnailStrip.
    void SetOnThumbnailStripEnabledChanged(std::function<void(bool)> handler);
    void SetThumbnailStripEnabled(bool enabled, bool notify = false);
    bool IsThumbnailStripEnabled() const;
    void ToggleThumbnailStrip();

    // Thumbnail card size (strip thickness), applied to every pane's strip and
    // inherited by new panes. Use ThumbnailStrip::kSize{Small,Medium,Large}.
    void SetThumbnailStripSize(float extent);
    float ThumbnailStripSize() const { return m_thumbStripExtent; }
    // Fired when a drag-resize of any pane's strip settles, so the owner can
    // persist the final size (live drag frames don't fire this).
    void SetOnThumbnailStripSizeChanged(std::function<void(float)> handler);

    void InvalidateTextureCaches();

    // Resources panel callbacks (Game Data / overrides), forwarded to the
    // control strip.
    void SetOnBrowseGameData(std::function<void()> handler);
    void SetOnDetectGameData(std::function<void()> handler);
    void SetOnAddOverrideFolder(std::function<void()> handler);
    void SetOnClearOverrides(std::function<void()> handler);
    void SetGameDataLabel(const std::wstring& text);
    void SetOverrideCountLabel(std::size_t count);

private:
    // Reuse a free (empty) pane, else add one; null at kMaxPanes.
    ComparePane* AllocatePaneFor();
    void CreateInitialPanes();
    std::shared_ptr<NifComparePane> CreatePane();
    void WirePaneCallbacks(const std::shared_ptr<NifComparePane>& pane);
    void RebuildHostTree();
    // Rebuilds the views-area DockPanel's children (thumbnail strip docked
    // Bottom, host tree Fill) in the order DockPanel requires.
    void RebuildViewsArea();
    // Grays out the "Parallax Height" slider and the three extended-material
    // toggles unless some loaded pane carries material each control affects.
    void RefreshExtendedMaterialControls();
    // Sync the ANIMATION group with the active pane's AnimPlayer (sequence
    // list, time range/value, enabled state). Called on document changes and
    // active-pane switches, like RefreshExtendedMaterialControls.
    void RefreshAnimationControls();
    // Apply an ANIMATION-group action to the animation targets: every pane
    // when Sync Anim is on, else just the active pane.
    template <typename Fn> void ForEachAnimTarget(Fn&& fn);
    void ToggleAnimPlayback();
    // Application-wide shortcuts - see OnInputEvent's comment for the map.
    bool HandleShortcutKey(const FD2D::InputEvent& event);
    ComparePane* PaneAt(const POINT& clientPt) const;

    // Material data diff panel (a diff-oriented take on NifSkope's block
    // inspector): while a sub-mesh is selected in the active pane, an
    // overlay table shows its BSLightingShaderProperty values - shader
    // type/flags, specular/emissive/alpha/UV scalars, texture slots -
    // side by side with the same-named mesh of every other loaded pane
    // (index fallback, up to 4 columns), highlighting differing values.
    // Rebuilt from the live materials every frame it is visible, so no
    // refresh plumbing is needed. Toggled with the I key.
    void DrawMaterialDiffPanel(ID2D1RenderTarget* target);

    // Texture inspector (T key): for the active pane's selected sub-mesh,
    // a left-side overlay lists every bound texture slot with its
    // resolution, pixel format, mip count and resolved source (loose file
    // vs BSA - the mod-conflict diagnosis view), plus a 2D preview of the
    // clicked slot with R/G/B/A channel isolation (click the preview to
    // cycle) for reading _rmaos/_m channel content in place. Clicks over
    // the panel are consumed in OnInputEvent via the rects captured at
    // draw time.
    void DrawTextureInspector(ID2D1RenderTarget* target);
    // Per-pane badge distinguishing panes that show the SAME file name as
    // another pane (a synced compare group) from ones whose file is unique.
    void DrawSyncBadges(ID2D1RenderTarget* target);
    bool HandleTextureInspectorClick(const POINT& pt);
    bool EnsureTexturePreview(ID2D1RenderTarget* target, NifComparePane& pane, const std::string& relPath);

    // Drag&drop internals (see the OnFileDrag comment above). The overlay
    // pane pointer is validated against m_panes at draw time, so a pane
    // removed mid-drag cannot dangle into OnRenderOverlay.
    enum class DragOverlayKind { None, Replace, Insert };
    static constexpr float kInsertZoneRatio = 0.75f; // FICture2's insert threshold
    void SetDragOverlay(ComparePane* pane, DragOverlayKind kind);
    ComparePane* InsertPaneAfter(ComparePane* after); // nullptr at kMaxPanes
    // Publishes the loaded documents' file names/count into m_ipcQueue so
    // the IPC worker threads can gate incoming forwards without touching
    // the UI thread. Called wherever the document set changes.
    void UpdateIpcOpenSnapshot();
    void RecalcControlStripExtent();
    void ApplyOrientationPreset(int index);
    // Numpad 5: flip orthographic/perspective on every pane (kept uniform so a
    // side-by-side compare uses the same projection); new panes inherit it.
    void ToggleProjection();
    // Set orthographic on every pane and keep the NAVIGATION checkbox in sync;
    // the single path used by both the checkbox and the Numpad-5 toggle.
    void ApplyOrthographic(bool on);

    // Deferred pane removal: a pane's own Close button cannot safely destroy
    // its owning NifComparePane synchronously from inside its own click
    // handler (FD2D::Button::OnInputEvent touches `this` again right after
    // invoking the click callback - see Button.cpp's MouseUp case). Queue
    // the name and let a one-shot Win32 timer (fired from a fresh stack
    // frame via the normal DispatchMessage loop, after the click handler has
    // fully returned) perform the actual removal - the same "post, don't
    // mutate inline" shape as FICture2's ImageBrowserDeferredActions /
    // CMD_FIC2_DEFERRED_ACTION, minus the need for a custom Backplate
    // subclass (a SetTimer callback is dispatched by the stock Win32
    // message loop with no extra wiring).
    void QueueClosePane(const std::wstring& paneName);
    void ProcessPendingCloses();
    static void CALLBACK TimerThunk(HWND hwnd, UINT msg, UINT_PTR idEvent, DWORD dwTime);

    // ~60fps timer that steps every pane's camera tween until none are
    // animating (started on demand by a pane's animate-requested callback).
    void EnsureCameraAnimTimer();
    void TickCameraAnimations();
    static void CALLBACK CameraAnimThunk(HWND hwnd, UINT msg, UINT_PTR idEvent, DWORD dwTime);
    bool m_cameraAnimTimerRunning = false;

    // Heterogeneous: NIF panes and (from the texture-view port) image panes,
    // laid out together in one host tree. NIF-specific work iterates via
    // ForEachNifPane / AsNif; anything a pane exposes generically (Load,
    // CurrentPath, Clear, Kind) goes through the ComparePane base.
    std::vector<std::shared_ptr<ComparePane>> m_panes;
    ComparePane* m_activePane = nullptr; // read through ActivePane() - validates + falls back

    // NIF-pane helpers for the still-NIF-centric control/sync/material code.
    // AsNif returns nullptr for a non-NIF (e.g. image) pane; ForEachNifPane /
    // ForEachViewport skip those, so applying a NIF render setting to "all
    // panes" quietly ignores image panes.
    static NifComparePane* AsNif(const std::shared_ptr<ComparePane>& p)
    {
        return dynamic_cast<NifComparePane*>(p.get());
    }
    static NifComparePane* AsNif(ComparePane* p)
    {
        return dynamic_cast<NifComparePane*>(p);
    }
    template <class Fn> void ForEachNifPane(Fn&& fn)
    {
        for (auto& p : m_panes)
            if (auto* n = AsNif(p)) fn(*n);
    }
    template <class Fn> void ForEachViewport(Fn&& fn)
    {
        for (auto& p : m_panes)
            if (auto* n = AsNif(p)) fn(n->Viewport());
    }
    // First NIF pane's viewport (the "apply lighting to pane 1 only" target
    // when Sync Lighting is off), or null when no NIF pane is present.
    NifViewport* FirstNifViewport()
    {
        for (auto& p : m_panes)
            if (auto* n = AsNif(p)) return &n->Viewport();
        return nullptr;
    }
    ComparePane* m_dragOverlayPane = nullptr;
    DragOverlayKind m_dragOverlayKind = DragOverlayKind::None;
    std::wstring m_hostName; // current host tree inside m_viewsArea, swapped on rebuild (see RebuildHostTree)
    std::shared_ptr<FD2D::Wnd> m_hostRoot; // same tree, kept for the split-ratio walks
    // The SplitPanel's first child: a DockPanel holding the pane host tree
    // (Fill) and, docked at its bottom, the thumbnail strip - so the strip
    // sits below the panes and above the control strip.
    std::shared_ptr<FD2D::DockPanel> m_viewsArea;
    // Global on/off + size for every pane's thumbnail strip (new panes inherit
    // both). kSizeMedium is the default extent.
    bool m_thumbStripEnabled = true;
    float m_thumbStripExtent = ThumbnailStrip::kSizeMedium;
    std::function<void(bool)> m_onThumbStripEnabledChanged;
    std::function<void(float)> m_onThumbStripSizeCommitted;
    void ApplyThumbStripEnabled(bool on); // broadcast to all panes + relayout
    std::vector<std::wstring> m_pendingCloseNames;
    std::shared_ptr<NifCompareControlPanel> m_controls;
    // Horizontal scroll host for the control strip: when the window is too
    // narrow to show every group, this clips + adds a draggable scrollbar so no
    // control becomes unreachable (the strip is fixed-height, so only X scrolls).
    std::shared_ptr<FD2D::ScrollView> m_controlsScroll;
    std::shared_ptr<IpcOpenQueue> m_ipcQueue;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    RenderDevice* m_renderDevice = nullptr;
    ResourceManager* m_resourceManager = nullptr;

    std::function<void(NifComparePane&)> m_onPaneOpenRequested;
    std::function<void(const std::wstring&)> m_onFileOpened;
    std::function<void(POINT, NifComparePane*)> m_onContextMenuRequested;
    std::function<void(NifComparePane&)> m_onScreenshotRequested;

    bool m_showMaterialPanel = true; // 'I' toggles; shown only while something is selected
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_matPanelText; // lazy, device-independent
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_syncBadgeText; // lazy, centered badge label

    // A material-panel texture cell captured during the last draw: its client
    // rect plus the full relative path and resolved source, for hover-tooltip
    // and right-click-copy (see DrawMaterialTooltip / HandleMaterialPanelCopy).
    struct MatTexCell
    {
        D2D1_RECT_F rect {};
        std::wstring fullPath;   // untruncated relative texture path (copied on right-click)
        std::wstring resolved;   // loose absolute path / "archive -> entry", shown in the tooltip
    };
    bool m_matPanelLive = false;
    D2D1_RECT_F m_matPanelRect {};
    std::vector<MatTexCell> m_matTexCells;
    int m_matHoverCell = -1; // index into m_matTexCells under the cursor, -1 = none

    // Collapse + column-width resize state for the material panel.
    bool m_matPanelCollapsed = false;      // header-only when true (click header to toggle)
    float m_matColW = 208.0f;              // value-column width, adjustable via the grip
    int m_matColCount = 1;                 // columns drawn last frame (for grip drag scaling)
    D2D1_RECT_F m_matHeaderRect {};        // header row hit rect (collapse toggle)
    D2D1_RECT_F m_matResizeGrip {};        // bottom-left grip hit rect (width drag)
    bool m_matResizing = false;            // a grip drag is in progress
    LONG m_matResizeStartX = 0;            // cursor x at drag start
    float m_matColWStart = 208.0f;         // m_matColW at drag start

    void DrawMaterialTooltip(ID2D1RenderTarget* target, const MatTexCell& cell,
                             const D2D1_POINT_2F& cursor);
    bool HandleMaterialPanelCopy(const POINT& pt);
    bool HandleMaterialPanelMouseDown(const POINT& pt); // header toggle / grip drag / swallow
    void UpdateMaterialHover(const POINT& pt); // repaints when the hovered cell changes

    // Texture inspector state (see DrawTextureInspector).
    bool m_showTextureInspector = false; // 'T' toggles
    int m_texInspectorRow = 0;           // which listed slot is previewed
    int m_texChannelMode = 0;            // 0=RGBA 1=R 2=G 3=B 4=A
    std::wstring m_texPreviewKey;        // nifDir|relPath|channel of the cached bitmap
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_texPreviewBitmap;
    ID2D1RenderTarget* m_texPreviewOwner = nullptr; // bitmap is target-bound
    float m_texPreviewAspect = 1.0f;
    // Hit rects captured during the last draw (client coords).
    bool m_texPanelLive = false;
    D2D1_RECT_F m_texPanelRect {};
    D2D1_RECT_F m_texPreviewHitRect {};
    std::vector<D2D1_RECT_F> m_texRowRects;

    bool m_syncViews = true;
    bool m_syncLighting = true;
    bool m_syncThumbnails = true;

    // After a thumbnail pick in `source`, load the same file name from every
    // other pane's own folder (skips panes lacking that name).
    void SyncThumbnailSelection(NifComparePane* source, const std::wstring& path);
    // Thumbnail keyboard navigation on the active pane (each syncs to the
    // others). Step: prev/next sibling (delta -1/+1). Edge: first/last file.
    void ApplyThumbnailPick(NifComparePane* active, const std::wstring& path);
    void StepActiveThumbnail(int delta);
    void LoadEdgeThumbnail(bool last);
    bool m_applyingSync = false; // re-entrancy guard while mirroring camera changes
    float m_parallaxHeightScale = 2.0f; // current "Parallax Height" slider value, applied to new panes
    bool m_enableParallax = true;        // extended-material toggles, mirrored onto new panes
    bool m_enableComplexMaterial = true;
    bool m_enablePBR = true;
    bool m_showHiddenNodes = false;      // NifSkope "Show Hidden" equivalent, mirrored onto new panes
    bool m_showNormals = false;          // vertex normal/tangent line overlays, mirrored onto new panes
    bool m_showTangents = false;
    bool m_msaaEnabled = true;           // 4x MSAA toggle, mirrored onto new panes
    bool m_orthographic = false;         // ortho/persp projection, mirrored onto new panes
    // NAVIGATION group state, mirrored onto new panes (see WireNavigationControls).
    float m_navMoveSensitivity = 1.0f;
    float m_navZoomSensitivity = 1.0f;
    float m_navRotateSensitivity = 1.0f;
    float m_fovRadians = 0.9f;            // vertical FOV (~51.6 deg); NifViewport default
    bool m_orbitAroundSelection = true;
    bool m_zoomToCursor = true;
    bool m_syncAnimation = true; // ANIMATION group targets all panes (vs active only)
    // Control-strip collapse state + the clickable chevron tab's rect (client
    // coords, captured while drawing the overlay for the input hit-test).
    bool m_controlsCollapsed = false;
    D2D1_RECT_F m_collapseTabRect {};
    // Shader hot-reload: a 1s Win32 timer polls the loose override files
    // (render is on-demand, so a frame-callback poll would stall while idle).
    void EnsureShaderReloadTimer();
    static void CALLBACK ShaderReloadThunk(HWND hwnd, UINT msg, UINT_PTR idEvent, DWORD dwTime);
    void PollShaderHotReload();
    bool m_shaderReloadTimerRunning = false;
    std::function<void(bool)> m_onControlsCollapsedChanged;
    bool m_enableTextures = true;        // render-channel toggles, mirrored onto new panes
    bool m_enableVertexColors = true;
    bool m_enableSpecular = true;
    bool m_enableGlow = true;
    bool m_enableLighting = true;
};

} // namespace nsk
