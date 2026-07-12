#include "NifCompareView.h"

#include <Backplate.h>
#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{

NifCompareView::NifCompareView(const std::wstring& name)
    : FD2D::SplitPanel(name, FD2D::SplitterOrientation::Vertical) // outer split divides views (first pane) from the bottom control strip (second pane)
{
    m_controls = std::make_shared<NifCompareControlPanel>(name + L"_Controls");

    CreateInitialPanes();
    RebuildHostTree();

    SetSecondChild(m_controls);
    SetSplitRatio(0.85f);
    SetConstraintPropagation(FD2D::ConstraintPropagation::Minimum);
    RecalcControlStripExtent();

    m_controls->SetOnAddPane([this]() { AddPane(); });
    m_controls->SetOnResetCameras([this]()
    {
        for (auto& p : m_panes) p->Viewport().ResetCamera();
    });
    m_controls->SetOnSyncViewsChanged([this](bool on) { m_syncViews = on; });
    m_controls->SetOnSyncLightingChanged([this](bool on) { m_syncLighting = on; });
    m_controls->SetOnOrientationChanged([this](int idx) { ApplyOrientationPreset(idx); });

    m_controls->SetOnFrontalLightChanged([this](bool on)
    {
        for (auto& p : m_panes) p->Viewport().SetFrontalLight(on);
    });
    m_controls->SetOnShowGridChanged([this](bool on)
    {
        for (auto& p : m_panes) p->Viewport().SetShowGrid(on);
    });
    m_controls->SetOnShowAxesChanged([this](bool on)
    {
        for (auto& p : m_panes) p->Viewport().SetShowAxes(on);
    });
    m_controls->SetOnWireframeChanged([this](bool on)
    {
        for (auto& p : m_panes) p->Viewport().SetWireframe(on);
    });

    // Lighting sliders are shared UI (not per-pane manipulable like camera
    // drag), so "Sync Lighting" here means "apply to every pane" (on) vs.
    // "apply to the first pane only" (off, freezes the others' lighting for
    // an isolated before/after comparison) rather than a two-way mirror.
    m_controls->SetOnBrightnessChanged([this](float v)
    {
        if (m_panes.empty()) return;
        if (m_syncLighting) { for (auto& p : m_panes) p->Viewport().SetBrightness(v); }
        else { m_panes.front()->Viewport().SetBrightness(v); }
    });
    m_controls->SetOnAmbientChanged([this](float v)
    {
        if (m_panes.empty()) return;
        if (m_syncLighting) { for (auto& p : m_panes) p->Viewport().SetAmbient(v); }
        else { m_panes.front()->Viewport().SetAmbient(v); }
    });
    m_controls->SetOnDeclinationChanged([this](float v)
    {
        if (m_panes.empty()) return;
        if (m_syncLighting) { for (auto& p : m_panes) p->Viewport().SetLightDeclinationDegrees(v); }
        else { m_panes.front()->Viewport().SetLightDeclinationDegrees(v); }
    });
    m_controls->SetOnPlanarAngleChanged([this](float v)
    {
        if (m_panes.empty()) return;
        if (m_syncLighting) { for (auto& p : m_panes) p->Viewport().SetLightPlanarAngleDegrees(v); }
        else { m_panes.front()->Viewport().SetLightPlanarAngleDegrees(v); }
    });
}

void NifCompareView::CreateInitialPanes()
{
    for (std::size_t i = 0; i < kDefaultInitialPanes; ++i)
    {
        m_panes.push_back(CreatePane());
    }
}

std::shared_ptr<NifComparePane> NifCompareView::CreatePane()
{
    auto pane = std::make_shared<NifComparePane>(NifCompareSplitCoordinator::NextPaneName());
    if (m_resolver)
    {
        pane->SetResourceResolver(m_resolver);
    }
    WirePaneCallbacks(pane);
    return pane;
}

void NifCompareView::WirePaneCallbacks(const std::shared_ptr<NifComparePane>& pane)
{
    // Raw pointers only below: the pane outlives these callbacks (they are
    // destroyed together with it), and capturing the shared_ptr itself would
    // create an ownership cycle (pane -> viewport -> callback -> pane).
    NifComparePane* paneRaw = pane.get();

    pane->Viewport().SetOnCameraChanged([this, paneRaw](const Camera& cam)
    {
        if (!m_syncViews || m_applyingSync) return;
        m_applyingSync = true;
        for (auto& other : m_panes)
        {
            if (other.get() != paneRaw)
                other->Viewport().SetCamera(cam);
        }
        m_applyingSync = false;
    });

    pane->SetOnOpen([this, paneRaw]()
    {
        if (m_onPaneOpenRequested)
            m_onPaneOpenRequested(*paneRaw);
    });

    const std::wstring paneName = pane->Name();
    pane->SetOnClose([this, paneName]()
    {
        QueueClosePane(paneName);
    });
}

NifComparePane* NifCompareView::AddPane()
{
    if (!NifCompareSplitCoordinator::CanAddPane(m_panes.size(), kMaxPanes))
        return nullptr;

    auto pane = CreatePane();
    m_panes.push_back(pane);
    RebuildHostTree();
    return pane.get();
}

void NifCompareView::RebuildHostTree()
{
    std::vector<std::shared_ptr<FD2D::Wnd>> wnds;
    wnds.reserve(m_panes.size());
    for (auto& p : m_panes)
        wnds.push_back(p);

    SetFirstChild(NifCompareSplitCoordinator::BuildEqualWidthHostTree(wnds));

    // A new/removed child changes this panel's Measure/Arrange results, not
    // just its pixel content - Invalidate() alone only schedules a repaint
    // of the *existing* (now-stale) arranged rects (see Wnd::Invalidate's
    // comment), so without RequestLayout() a newly added pane stays
    // unarranged (zero size) until some unrelated resize forces a relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::RecalcControlStripExtent()
{
    // Query the strip's own Measure() (already sums its rows' heights
    // correctly) instead of guessing a fixed pixel height, same trick
    // liteviewer's constructor used for its (much taller) sidebar variant.
    const float contentHeight = m_controls->Measure({ 1600.0f, 10000.0f }).h;
    SetSecondPaneMinExtent((std::max)(80.0f, contentHeight - 6.0f));
    SetSecondPaneMaxExtent((std::max)(120.0f, contentHeight + 16.0f));
}

void NifCompareView::ApplyOrientationPreset(int index)
{
    // Unlike liteviewer's Left-is-primary/Right-mirrors-if-synced scheme,
    // an orientation preset has no natural "primary" pane once there can be
    // up to 4 - it always applies to every currently open pane.
    for (auto& p : m_panes)
    {
        p->Viewport().GetCamera().setPreset(index);
        p->Viewport().Invalidate();
    }
}

void NifCompareView::QueueClosePane(const std::wstring& paneName)
{
    if (m_panes.size() <= kMinPanes)
        return; // never close the last remaining pane

    for (const std::wstring& n : m_pendingCloseNames)
    {
        if (n == paneName)
            return; // already queued
    }
    m_pendingCloseNames.push_back(paneName);

    FD2D::Backplate* bp = BackplateRef();
    if (!bp || !bp->Window())
        return;
    ::SetTimer(bp->Window(), reinterpret_cast<UINT_PTR>(this), USER_TIMER_MINIMUM, &NifCompareView::TimerThunk);
}

void NifCompareView::TimerThunk(HWND hwnd, UINT /*msg*/, UINT_PTR idEvent, DWORD /*dwTime*/)
{
    ::KillTimer(hwnd, idEvent);
    // idEvent is this view's own `this` pointer (see QueueClosePane) - safe
    // for the lifetime of a single top-level compare view/window, which is
    // how NIFDiff's app shell uses this class (one NifCompareView per
    // window, alive for the whole session).
    auto* self = reinterpret_cast<NifCompareView*>(idEvent);
    self->ProcessPendingCloses();
}

void NifCompareView::ProcessPendingCloses()
{
    if (m_pendingCloseNames.empty())
        return;

    for (const std::wstring& paneName : m_pendingCloseNames)
    {
        if (m_panes.size() <= kMinPanes)
            break;
        std::erase_if(m_panes,
            [&](const std::shared_ptr<NifComparePane>& p) { return p->Name() == paneName; });
    }
    m_pendingCloseNames.clear();

    RebuildHostTree();
}

void NifCompareView::SetOnPaneOpenRequested(std::function<void(NifComparePane&)> handler)
{
    m_onPaneOpenRequested = std::move(handler);
}

void NifCompareView::SetResourceResolver(ResourceResolver* resolver)
{
    m_resolver = resolver;
    for (auto& p : m_panes)
        p->SetResourceResolver(resolver);
}

void NifCompareView::InvalidateTextureCaches()
{
    for (auto& p : m_panes)
        p->InvalidateTextureCache();
}

void NifCompareView::SetOnBrowseGameData(std::function<void()> handler) { m_controls->SetOnBrowseGameData(std::move(handler)); }
void NifCompareView::SetOnDetectGameData(std::function<void()> handler) { m_controls->SetOnDetectGameData(std::move(handler)); }
void NifCompareView::SetOnAddOverrideFolder(std::function<void()> handler) { m_controls->SetOnAddOverrideFolder(std::move(handler)); }
void NifCompareView::SetOnClearOverrides(std::function<void()> handler) { m_controls->SetOnClearOverrides(std::move(handler)); }
void NifCompareView::SetGameDataLabel(const std::wstring& text) { m_controls->SetGameDataLabel(text); }
void NifCompareView::SetOverrideCountLabel(std::size_t count) { m_controls->SetOverrideCountLabel(count); }

} // namespace nsk
