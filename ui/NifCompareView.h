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
    NifComparePane& Pane(std::size_t index) { return *m_panes[index]; }

    // The ACTIVE pane (FICture2's focused-browser equivalent): set by any
    // click inside a pane, by the 1-8 number keys, and by a drop's target
    // pane; drawn with an accent border while several panes are open. It
    // is the target for pane-context hotkeys (Ctrl+O opens into it, F12
    // screenshots it). Never null while panes exist: a stale pointer
    // (pane closed since) falls back to the first pane.
    NifComparePane* ActivePane() const;
    void SetActivePane(NifComparePane* pane);

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

    // Per-pane thumbnail strip master on/off (every pane owns its own strip;
    // this toggle applies to all of them at once), mirrored by the VIEW-group
    // "Thumbnails" checkbox. The owner persists the choice via
    // SetOnThumbnailStripEnabledChanged; the context-menu item flips the same
    // checkbox through ToggleThumbnailStrip.
    void SetOnThumbnailStripEnabledChanged(std::function<void(bool)> handler);
    void SetThumbnailStripEnabled(bool enabled, bool notify = false);
    bool IsThumbnailStripEnabled() const;
    void ToggleThumbnailStrip();

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
    // Application-wide shortcuts - see OnInputEvent's comment for the map.
    bool HandleShortcutKey(const FD2D::InputEvent& event);
    NifComparePane* PaneAt(const POINT& clientPt) const;

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
    bool HandleTextureInspectorClick(const POINT& pt);
    bool EnsureTexturePreview(ID2D1RenderTarget* target, NifComparePane& pane, const std::string& relPath);

    // Drag&drop internals (see the OnFileDrag comment above). The overlay
    // pane pointer is validated against m_panes at draw time, so a pane
    // removed mid-drag cannot dangle into OnRenderOverlay.
    enum class DragOverlayKind { None, Replace, Insert };
    static constexpr float kInsertZoneRatio = 0.75f; // FICture2's insert threshold
    void SetDragOverlay(NifComparePane* pane, DragOverlayKind kind);
    NifComparePane* InsertPaneAfter(NifComparePane* after); // nullptr at kMaxPanes
    // Publishes the loaded documents' file names/count into m_ipcQueue so
    // the IPC worker threads can gate incoming forwards without touching
    // the UI thread. Called wherever the document set changes.
    void UpdateIpcOpenSnapshot();
    void RecalcControlStripExtent();
    void ApplyOrientationPreset(int index);

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

    std::vector<std::shared_ptr<NifComparePane>> m_panes;
    NifComparePane* m_activePane = nullptr; // read through ActivePane() - validates + falls back
    NifComparePane* m_dragOverlayPane = nullptr;
    DragOverlayKind m_dragOverlayKind = DragOverlayKind::None;
    std::wstring m_hostName; // current host tree inside m_viewsArea, swapped on rebuild (see RebuildHostTree)
    std::shared_ptr<FD2D::Wnd> m_hostRoot; // same tree, kept for the split-ratio walks
    // The SplitPanel's first child: a DockPanel holding the pane host tree
    // (Fill) and, docked at its bottom, the thumbnail strip - so the strip
    // sits below the panes and above the control strip.
    std::shared_ptr<FD2D::DockPanel> m_viewsArea;
    // Global on/off for every pane's thumbnail strip (new panes inherit it).
    bool m_thumbStripEnabled = true;
    std::function<void(bool)> m_onThumbStripEnabledChanged;
    void ApplyThumbStripEnabled(bool on); // broadcast to all panes + relayout
    std::vector<std::wstring> m_pendingCloseNames;
    std::shared_ptr<NifCompareControlPanel> m_controls;
    std::shared_ptr<IpcOpenQueue> m_ipcQueue;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    RenderDevice* m_renderDevice = nullptr;

    std::function<void(NifComparePane&)> m_onPaneOpenRequested;
    std::function<void(const std::wstring&)> m_onFileOpened;
    std::function<void(POINT, NifComparePane*)> m_onContextMenuRequested;
    std::function<void(NifComparePane&)> m_onScreenshotRequested;

    bool m_showMaterialPanel = true; // 'I' toggles; shown only while something is selected
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_matPanelText; // lazy, device-independent

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
    bool m_enableTextures = true;        // render-channel toggles, mirrored onto new panes
    bool m_enableVertexColors = true;
    bool m_enableSpecular = true;
    bool m_enableGlow = true;
    bool m_enableLighting = true;
};

} // namespace nsk
