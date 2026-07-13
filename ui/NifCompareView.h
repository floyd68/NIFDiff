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
    // Returns false when all kMaxPanes panes are occupied - the caller (the
    // IPC path, see OnCommandEvent) treats that as "start a new instance
    // instead". Used by the single-instance IPC handler; safe for any other
    // caller on the UI thread too.
    bool OpenIntoBestPane(const std::wstring& path);

    // Handles CMD_NIFDIFF_IPC_OPEN broadcast from the IPC server thread
    // (see app/IpcOpenRequest.h), mirroring FICture2's
    // ImageBrowser::HandleIpcCompareMessage: load via OpenIntoBestPane,
    // record the outcome on the request, signal its event.
    bool OnCommandEvent(const FD2D::CommandEvent& event) override;

    // Catches right-clicks no child consumed (viewport right-*drags* pan the
    // camera and are consumed there; plain right-clicks bubble up to here -
    // see NifViewport::OnInputEvent's MouseUp case) and requests the
    // app-level context menu. The app shell owns the actual menu (About /
    // file association / Exit live at the app level, not in this view).
    bool OnInputEvent(const FD2D::InputEvent& event) override;

    // `clientPt` is in window client coordinates.
    void SetOnContextMenuRequested(std::function<void(POINT clientPt)> handler);

    void SetResourceResolver(ResourceResolver* resolver);
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
    std::wstring m_hostName; // current first-child host tree, removed on rebuild (see RebuildHostTree)
    std::vector<std::wstring> m_pendingCloseNames;
    std::shared_ptr<NifCompareControlPanel> m_controls;
    ResourceResolver* m_resolver = nullptr;

    std::function<void(NifComparePane&)> m_onPaneOpenRequested;
    std::function<void(POINT)> m_onContextMenuRequested;

    bool m_syncViews = true;
    bool m_syncLighting = true;
    bool m_applyingSync = false; // re-entrancy guard while mirroring camera changes
};

} // namespace nsk
