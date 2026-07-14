#include "NifCompareView.h"
#include "../core/NifLog.h"

#include <Backplate.h>
#include <Util.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

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

    // Display setting rather than a light: always applies to every pane.
    m_controls->SetOnParallaxHeightChanged([this](float v)
    {
        m_parallaxHeightScale = v;
        for (auto& p : m_panes)
            p->Viewport().SetParallaxHeightScale(v);
    });

    // Extended-material toggles - display settings, applied to every pane.
    m_controls->SetOnParallaxEnabledChanged([this](bool on)
    {
        m_enableParallax = on;
        for (auto& p : m_panes)
            p->Viewport().SetEnableParallax(on);
    });
    m_controls->SetOnComplexMaterialEnabledChanged([this](bool on)
    {
        m_enableComplexMaterial = on;
        for (auto& p : m_panes)
            p->Viewport().SetEnableComplexMaterial(on);
    });
    m_controls->SetOnPBREnabledChanged([this](bool on)
    {
        m_enablePBR = on;
        for (auto& p : m_panes)
            p->Viewport().SetEnablePBR(on);
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
    if (m_textureRepository)
    {
        pane->SetTextureRepository(m_textureRepository);
    }
    pane->Viewport().SetParallaxHeightScale(m_parallaxHeightScale);
    pane->Viewport().SetEnableParallax(m_enableParallax);
    pane->Viewport().SetEnableComplexMaterial(m_enableComplexMaterial);
    pane->Viewport().SetEnablePBR(m_enablePBR);
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

    pane->SetOnDocumentChanged([this]()
    {
        RefreshExtendedMaterialControls();
        UpdateIpcOpenSnapshot();
    });
}

void NifCompareView::RefreshExtendedMaterialControls()
{
    // The height slider follows slider-driven parallax only
    // (HasActiveParallax); the three feature toggles follow their own
    // per-feature presence tests, so e.g. loading a lone PBR NIF enables
    // "True PBR" and "Parallax" (its _p displacement) but not "Complex Mat".
    bool anySliderParallax = false;
    bool anyParallax = false;
    bool anyComplexMaterial = false;
    bool anyPBR = false;
    for (auto& p : m_panes)
    {
        if (!p)
            continue;
        NifViewport& vp = p->Viewport();
        anySliderParallax = anySliderParallax || vp.HasActiveParallax();
        anyParallax = anyParallax || vp.HasParallaxMaterials();
        anyComplexMaterial = anyComplexMaterial || vp.HasComplexMaterials();
        anyPBR = anyPBR || vp.HasPBRMaterials();
    }
    m_controls->SetParallaxHeightEnabled(anySliderParallax);
    m_controls->SetParallaxToggleEnabled(anyParallax);
    m_controls->SetComplexMaterialToggleEnabled(anyComplexMaterial);
    m_controls->SetPBRToggleEnabled(anyPBR);
}

void NifCompareView::RequestOpenPane(NifComparePane& pane)
{
    if (m_onPaneOpenRequested)
        m_onPaneOpenRequested(pane);
}

void NifCompareView::RequestClosePane(NifComparePane& pane)
{
    QueueClosePane(pane.Name());
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

void NifCompareView::SetPaneCount(std::size_t count)
{
    count = (std::max)(kMinPanes, (std::min)(count, kMaxPanes));
    if (count == m_panes.size())
        return;

    while (m_panes.size() < count)
        m_panes.push_back(CreatePane());
    while (m_panes.size() > count)
        m_panes.pop_back();

    RebuildHostTree();
}

bool NifCompareView::OpenIntoBestPane(const std::wstring& path)
{
    if (path.empty())
        return false;

    NifComparePane* target = nullptr;
    for (auto& pane : m_panes)
    {
        if (pane && pane->Document() == nullptr)
        {
            target = pane.get();
            break;
        }
    }
    if (!target)
        target = AddPane(); // nullptr when already at kMaxPanes

    if (!target)
        return false;

    std::string error;
    if (!target->Load(path, &error))
        return false;

    Invalidate();
    return true;
}

void NifCompareView::SetIpcOpenQueue(std::shared_ptr<IpcOpenQueue> queue)
{
    m_ipcQueue = std::move(queue);
    UpdateIpcOpenSnapshot();
}

void NifCompareView::UpdateIpcOpenSnapshot()
{
    if (!m_ipcQueue)
        return;

    std::vector<std::wstring> names;
    for (const auto& pane : m_panes)
    {
        const NifDocument* doc = pane ? pane->Document() : nullptr;
        if (doc == nullptr)
            continue;
        std::wstring name = IpcOpenQueue::FileNameLower(doc->filePath());
        if (!name.empty())
            names.push_back(std::move(name));
    }

    std::lock_guard<std::mutex> lock(m_ipcQueue->mutex);
    m_ipcQueue->loadedCount = names.size();
    m_ipcQueue->openNamesLower = std::move(names);
}

void NifCompareView::DrainIpcOpenQueue()
{
    if (!m_ipcQueue)
        return;

    for (;;)
    {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(m_ipcQueue->mutex);
            if (m_ipcQueue->pending.empty())
                break;
            path = std::move(m_ipcQueue->pending.front());
            m_ipcQueue->pending.pop_front();
        }

        if (OpenIntoBestPane(path))
            continue;

        // The sending process was told OpenedInPane and has exited, so the
        // file must land somewhere: hand it to a fresh instance. (Rare -
        // the gate's capacity check keeps this to genuine races or files
        // that fail to load.)
        NIFLOG_WARN("IPC drain: queued path no longer fits/loads here - spawning a new instance.");
        wchar_t exePath[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            const std::wstring args = L"\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", exePath, args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
}

bool NifCompareView::OnCommandEvent(const FD2D::CommandEvent& event)
{
    if (event.id == CMD_NIFDIFF_IPC_OPEN)
    {
        DrainIpcOpenQueue();
        return true;
    }
    return FD2D::SplitPanel::OnCommandEvent(event);
}

bool NifCompareView::OnInputEvent(const FD2D::InputEvent& event)
{
    if (FD2D::SplitPanel::OnInputEvent(event))
        return true;

    if (event.type == FD2D::InputEventType::MouseUp &&
        event.button == FD2D::MouseButton::Right &&
        event.hasPoint &&
        m_onContextMenuRequested)
    {
        // Per-pane menu items act on the pane under the cursor.
        NifComparePane* hitPane = nullptr;
        for (auto& p : m_panes)
        {
            if (p && FD2D::Util::RectContainsPoint(p->LayoutRect(), event.point))
            {
                hitPane = p.get();
                break;
            }
        }
        m_onContextMenuRequested(event.point, hitPane);
        return true;
    }
    return false;
}

void NifCompareView::SetOnContextMenuRequested(std::function<void(POINT, NifComparePane*)> handler)
{
    m_onContextMenuRequested = std::move(handler);
}

void NifCompareView::RebuildHostTree()
{
    std::vector<std::shared_ptr<FD2D::Wnd>> wnds;
    wnds.reserve(m_panes.size());
    for (auto& p : m_panes)
        wnds.push_back(p);

    std::shared_ptr<FD2D::Wnd> host = NifCompareSplitCoordinator::BuildEqualWidthHostTree(wnds);

    // SplitPanel::SetFirstChild only ADDS the new child - it does not remove
    // the previous one from the Children() collection, and every child in
    // that collection keeps rendering (Wnd::OnRender walks all children).
    // Without this removal the superseded host tree lingers with its stale
    // layout rects, drawing ghost panes (visible as orphaned Open/Close
    // strips after shrinking the pane count). FICture2 sidesteps the same
    // FD2D behavior with ClearChildren(), which we can't use here - it would
    // also drop the control strip and the splitter.
    if (host && !m_hostName.empty() && host->Name() != m_hostName)
        RemoveChild(m_hostName);
    if (host)
        m_hostName = host->Name();

    SetFirstChild(host);

    // A new/removed child changes this panel's Measure/Arrange results, not
    // just its pixel content - Invalidate() alone only schedules a repaint
    // of the *existing* (now-stale) arranged rects (see Wnd::Invalidate's
    // comment), so without RequestLayout() a newly added pane stays
    // unarranged (zero size) until some unrelated resize forces a relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();

    // The pane set changed: the removed/added panes may change whether any
    // extended-material document is still open, and the IPC gate's
    // loaded-documents snapshot.
    RefreshExtendedMaterialControls();
    UpdateIpcOpenSnapshot();
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

void NifCompareView::SetTextureRepository(TextureRepository* repository)
{
    m_textureRepository = repository;
    for (auto& p : m_panes)
        p->SetTextureRepository(repository);
}

void NifCompareView::InvalidateTextureCaches()
{
    // Resolution inputs changed (game data / override folders): the pooled
    // SRVs may now be stale, so drop the shared pool together with every
    // pane's resolution memo (memoized Entry pointers die with the pool).
    if (m_textureRepository != nullptr)
        m_textureRepository->Clear();
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
