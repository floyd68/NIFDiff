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

    // Called by the app shell when a pane's own "Open..." button (or a
    // restored/command-line file) needs a path picked/loaded. The app shell
    // owns the actual file dialog + NifComparePane::Load() call; this view
    // just tells it *which* pane requested it.
    void SetOnPaneOpenRequested(std::function<void(NifComparePane&)> handler);

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
    //   F              Reset View            G  Grid       X  Axes
    //   W              Wireframe             H  Hidden
    //   PgUp / PgDn    cycle camera preset   Ctrl+O  Open into a pane
    //   F12            Save Pane Screenshot (first pane with a document)
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

    void SetResourceResolver(ResourceResolver* resolver);

    // Shared cross-pane texture pool; also cleared by InvalidateTextureCaches.
    // Must outlive this view.
    void SetTextureRepository(TextureRepository* repository);

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
    // Grays out the "Parallax Height" slider and the three extended-material
    // toggles unless some loaded pane carries material each control affects.
    void RefreshExtendedMaterialControls();
    // Application-wide shortcuts - see OnInputEvent's comment for the map.
    bool HandleShortcutKey(const FD2D::InputEvent& event);
    NifComparePane* PaneAt(const POINT& clientPt) const;

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
    NifComparePane* m_dragOverlayPane = nullptr;
    DragOverlayKind m_dragOverlayKind = DragOverlayKind::None;
    std::wstring m_hostName; // current first-child host tree, removed on rebuild (see RebuildHostTree)
    std::vector<std::wstring> m_pendingCloseNames;
    std::shared_ptr<NifCompareControlPanel> m_controls;
    std::shared_ptr<IpcOpenQueue> m_ipcQueue;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;

    std::function<void(NifComparePane&)> m_onPaneOpenRequested;
    std::function<void(POINT, NifComparePane*)> m_onContextMenuRequested;
    std::function<void(NifComparePane&)> m_onScreenshotRequested;

    bool m_syncViews = true;
    bool m_syncLighting = true;
    bool m_applyingSync = false; // re-entrancy guard while mirroring camera changes
    float m_parallaxHeightScale = 2.0f; // current "Parallax Height" slider value, applied to new panes
    bool m_enableParallax = true;        // extended-material toggles, mirrored onto new panes
    bool m_enableComplexMaterial = true;
    bool m_enablePBR = true;
    bool m_showHiddenNodes = false;      // NifSkope "Show Hidden" equivalent, mirrored onto new panes
};

} // namespace nsk
