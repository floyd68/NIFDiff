// NifCompareView.h - FICture2-style top-level layout: a dynamic 2-4 pane
// 3D compare row plus a shared bottom control strip.
//
// Layout: outer top/bottom FD2D::SplitPanel (first = views host, second =
// NifCompareControlPanel), the same shape as FICture2's ImageBrowser
// rootSplit (ImageBrowser.cpp: ~0.85 ratio, second pane's extent clamped to
// its measured content height, ConstraintPropagation::Minimum) - not
// liteviewer's original fixed left/right SplitPanel + right-side sidebar.
// The "views host" is rebuilt on every pane add/remove via
// NifCompareSplitCoordinator::BuildEqualWidthHostTree, mirroring FICture2's
// own ImageBrowserSplitCoordinator (up to 4 equal-width panes).
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
    static constexpr std::size_t kMaxPanes = 4;
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
    std::vector<std::wstring> m_pendingCloseNames;
    std::shared_ptr<NifCompareControlPanel> m_controls;
    ResourceResolver* m_resolver = nullptr;

    std::function<void(NifComparePane&)> m_onPaneOpenRequested;

    bool m_syncViews = true;
    bool m_syncLighting = true;
    bool m_applyingSync = false; // re-entrancy guard while mirroring camera changes
};

} // namespace nsk
