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
    m_controls->SetOnShowHiddenChanged([this](bool on)
    {
        m_showHiddenNodes = on;
        for (auto& p : m_panes) p->Viewport().SetShowHiddenNodes(on);
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
    pane->Viewport().SetShowHiddenNodes(m_showHiddenNodes);
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

NifComparePane* NifCompareView::ActivePane() const
{
    for (const auto& p : m_panes)
    {
        if (p.get() == m_activePane)
            return m_activePane;
    }
    return m_panes.empty() ? nullptr : m_panes.front().get();
}

void NifCompareView::SetActivePane(NifComparePane* pane)
{
    if (m_activePane == pane)
        return;
    m_activePane = pane;
    Invalidate();
}

bool NifCompareView::OnInputEvent(const FD2D::InputEvent& event)
{
    // Any click inside a pane makes it the active one (FICture2's focused
    // browser), BEFORE the children consume the event - a viewport orbit
    // drag or a path-label click both count as "working in this pane".
    if (event.type == FD2D::InputEventType::MouseDown && event.hasPoint)
    {
        if (NifComparePane* hit = PaneAt(event.point))
            SetActivePane(hit);
    }

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

    if (event.type == FD2D::InputEventType::KeyDown && !event.isSystemKey)
        return HandleShortcutKey(event);

    return false;
}

namespace
{
    bool IsNifPath(const std::wstring& path)
    {
        std::wstring ext = std::filesystem::path(path).extension().wstring();
        for (wchar_t& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        return ext == L".nif";
    }
}

NifComparePane* NifCompareView::PaneAt(const POINT& clientPt) const
{
    for (const auto& p : m_panes)
    {
        if (p && FD2D::Util::RectContainsPoint(p->LayoutRect(), clientPt))
            return p.get();
    }
    return nullptr;
}

namespace
{
    // Relative X of the point inside the rect, 0..1.
    float RelativeX(const D2D1_RECT_F& rc, const POINT& pt)
    {
        const float w = (std::max)(1.0f, rc.right - rc.left);
        return (static_cast<float>(pt.x) - rc.left) / w;
    }
}

bool NifCompareView::OnFileDrag(const std::wstring& path, const POINT& clientPt, FD2D::FileDragVisual& outVisual)
{
    NifComparePane* pane = IsNifPath(path) ? PaneAt(clientPt) : nullptr;
    if (pane == nullptr)
    {
        SetDragOverlay(nullptr, DragOverlayKind::None);
        return false;
    }

    const bool insert = RelativeX(pane->LayoutRect(), clientPt) >= kInsertZoneRatio
                        && m_panes.size() < kMaxPanes;
    SetDragOverlay(pane, insert ? DragOverlayKind::Insert : DragOverlayKind::Replace);
    outVisual = insert ? FD2D::FileDragVisual::Insert : FD2D::FileDragVisual::Replace;
    return true;
}

void NifCompareView::OnFileDragLeave()
{
    FD2D::SplitPanel::OnFileDragLeave();
    SetDragOverlay(nullptr, DragOverlayKind::None);
}

bool NifCompareView::OnFileDropPaths(const std::vector<std::wstring>& paths, const POINT& clientPt)
{
    SetDragOverlay(nullptr, DragOverlayKind::None);

    std::vector<std::wstring> nifs;
    for (const std::wstring& p : paths)
    {
        if (IsNifPath(p))
            nifs.push_back(p);
    }
    if (nifs.empty())
        return false;

    NifComparePane* hit = PaneAt(clientPt);
    if (hit == nullptr)
        return false; // matches the drag-over decline: no target, no drop

    // Same zone split the drag-over visual promised: right quarter inserts
    // a NEW pane right after the hovered one, the rest replaces it.
    NifComparePane* target = hit;
    if (RelativeX(hit->LayoutRect(), clientPt) >= kInsertZoneRatio && m_panes.size() < kMaxPanes)
    {
        if (NifComparePane* inserted = InsertPaneAfter(hit))
            target = inserted;
    }

    SetActivePane(target); // dropping into a pane means "work here now"
    std::string error;
    if (!target->Load(nifs.front(), &error))
        NIFLOG_WARN("Drop: failed to load into the target pane ({}).", error);
    for (std::size_t i = 1; i < nifs.size(); ++i)
    {
        if (!OpenIntoBestPane(nifs[i]))
        {
            NIFLOG_WARN("Drop: no pane left for {} more dropped file(s).", nifs.size() - i);
            break;
        }
    }
    Invalidate();
    return true;
}

void NifCompareView::SetDragOverlay(NifComparePane* pane, DragOverlayKind kind)
{
    if (m_dragOverlayPane == pane && m_dragOverlayKind == kind)
        return;
    m_dragOverlayPane = pane;
    m_dragOverlayKind = kind;
    Invalidate();
}

NifComparePane* NifCompareView::InsertPaneAfter(NifComparePane* after)
{
    if (m_panes.size() >= kMaxPanes)
        return nullptr;

    std::shared_ptr<NifComparePane> pane = CreatePane();
    auto insertAt = m_panes.end();
    for (auto it = m_panes.begin(); it != m_panes.end(); ++it)
    {
        if (it->get() == after)
        {
            insertAt = it + 1;
            break;
        }
    }
    NifComparePane* raw = pane.get();
    m_panes.insert(insertAt, std::move(pane));
    RebuildHostTree();
    return raw;
}

void NifCompareView::OnRenderOverlay(ID2D1RenderTarget* target)
{
    FD2D::SplitPanel::OnRenderOverlay(target);
    if (target == nullptr)
        return;

    // Active-pane accent border (FICture2's focused-browser highlight).
    // Only meaningful while several panes compete for the pane-context
    // hotkeys; a single pane is trivially the active one.
    if (m_panes.size() > 1)
    {
        if (NifComparePane* active = ActivePane())
        {
            D2D1_RECT_F rc = active->LayoutRect();
            rc.left += 1.0f; rc.top += 1.0f; rc.right -= 1.0f; rc.bottom -= 1.0f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent;
            if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.58f, 0.95f, 0.85f), &accent)))
                target->DrawRectangle(rc, accent.Get(), 2.0f);
        }
    }

    if (m_dragOverlayKind == DragOverlayKind::None || m_dragOverlayPane == nullptr)
        return;
    // A deferred close during the drag could have destroyed the pane;
    // only draw over one that is still ours.
    bool alive = false;
    for (const auto& p : m_panes)
        alive = alive || p.get() == m_dragOverlayPane;
    if (!alive)
        return;

    // FICture2's drag-controller overlay: translucent red = the drop
    // replaces this pane, translucent green over the insert zone (the
    // right quarter) = the drop adds a new pane there.
    D2D1_RECT_F rc = m_dragOverlayPane->LayoutRect();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (m_dragOverlayKind == DragOverlayKind::Replace)
    {
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.18f), &brush)))
            target->FillRectangle(rc, brush.Get());
    }
    else
    {
        rc.left = rc.left + (rc.right - rc.left) * kInsertZoneRatio;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 0.18f), &brush)))
            target->FillRectangle(rc, brush.Get());
    }
}

bool NifCompareView::HandleShortcutKey(const FD2D::InputEvent& event)
{
    // Backplate fills InputModifiers for mouse messages only; query the
    // live key state for the Ctrl chord here.
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    switch (event.keyCode)
    {
    case 'F': // Reset View, every pane (same as the PANES button)
        for (auto& p : m_panes) p->Viewport().ResetCamera();
        return true;

    case 'R': // reset only the active pane's camera
        if (NifComparePane* active = ActivePane())
            active->Viewport().ResetCamera();
        return true;

    // Display toggles go through the control panel so the checkboxes stay
    // in sync (notify=true runs the same wired handlers a click would).
    case 'G': m_controls->ToggleShowGrid();   return true;
    case 'X': m_controls->ToggleShowAxes();   return true;
    case 'H': m_controls->ToggleShowHidden(); return true;

    case 'W':
        if (!ctrl)
        {
            m_controls->ToggleWireframe();
            return true;
        }
        // Ctrl+W: close the active pane (same deferred path as the
        // context menu item; a lone pane is never closed).
        if (NifComparePane* active = ActivePane())
            RequestClosePane(*active);
        return true;

    case 'E':
        if (!ctrl)
            return false;
        // Ctrl+E: show the active pane's file in Explorer (same behavior
        // as the context menu's "Open Containing Folder" - explorer's
        // /select verb needs no COM apartment).
        if (NifComparePane* active = ActivePane())
        {
            const NifDocument* doc = active->Document();
            if (doc != nullptr && !doc->filePath().empty())
            {
                const std::wstring args = L"/select,\"" + doc->filePath() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        return true;

    case VK_DELETE: // clear the active pane's document, keep the pane
        if (NifComparePane* active = ActivePane())
            active->Clear();
        return true;

    case VK_TAB:
    {
        // Cycle the active pane (Shift+Tab goes backwards).
        if (m_panes.empty())
            return true;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::size_t index = 0;
        NifComparePane* active = ActivePane();
        for (std::size_t i = 0; i < m_panes.size(); ++i)
        {
            if (m_panes[i].get() == active)
            {
                index = i;
                break;
            }
        }
        const std::size_t count = m_panes.size();
        index = shift ? (index + count - 1) % count : (index + 1) % count;
        SetActivePane(m_panes[index].get());
        return true;
    }

    case VK_PRIOR: m_controls->CycleOrientation(-1); return true; // PgUp
    case VK_NEXT:  m_controls->CycleOrientation(+1); return true; // PgDn

    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8':
    {
        const std::size_t index = static_cast<std::size_t>(event.keyCode - '1');
        if (index < m_panes.size())
            SetActivePane(m_panes[index].get());
        return true;
    }

    case 'O':
        if (!ctrl)
            return false;
        if (NifComparePane* target = ActivePane())
            RequestOpenPane(*target);
        return true;

    case VK_F12:
        if (NifComparePane* target = ActivePane())
        {
            if (m_onScreenshotRequested)
                m_onScreenshotRequested(*target);
        }
        return true;

    default:
        return false;
    }
}

void NifCompareView::SetOnContextMenuRequested(std::function<void(POINT, NifComparePane*)> handler)
{
    m_onContextMenuRequested = std::move(handler);
}

void NifCompareView::SetOnScreenshotRequested(std::function<void(NifComparePane&)> handler)
{
    m_onScreenshotRequested = std::move(handler);
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
