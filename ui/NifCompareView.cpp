#include "NifCompareView.h"
#include "../core/NifLog.h"
#include "../core/ResourceManager.h"

#include <Backplate.h>
#include <Core.h>
#include <Util.h>
#include <DirectXTex.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <format>

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
    m_viewsArea = std::make_shared<FD2D::DockPanel>(name + L"_ViewsArea");

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
    m_controls->SetOnSyncFilesChanged([this](bool on) { m_syncThumbnails = on; });
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
    m_controls->SetOnShowNormalsChanged([this](bool on)
    {
        m_showNormals = on;
        for (auto& p : m_panes) p->Viewport().SetShowNormals(on);
    });
    m_controls->SetOnShowTangentsChanged([this](bool on)
    {
        m_showTangents = on;
        for (auto& p : m_panes) p->Viewport().SetShowTangents(on);
    });
    m_controls->SetOnMsaaChanged([this](bool on)
    {
        m_msaaEnabled = on;
        for (auto& p : m_panes) p->Viewport().SetMsaaEnabled(on);
    });
    // Global on/off for every pane's own thumbnail strip. The user click
    // applies it to all panes and reports it to the owner (for persistence).
    m_controls->SetOnThumbnailStripChanged([this](bool on)
    {
        ApplyThumbStripEnabled(on);
        if (m_onThumbStripEnabledChanged) m_onThumbStripEnabledChanged(on);
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

    m_controls->SetOnTexturesEnabledChanged([this](bool on)
    {
        m_enableTextures = on;
        for (auto& p : m_panes) p->Viewport().SetEnableTextures(on);
    });
    m_controls->SetOnVertexColorsEnabledChanged([this](bool on)
    {
        m_enableVertexColors = on;
        for (auto& p : m_panes) p->Viewport().SetEnableVertexColors(on);
    });
    m_controls->SetOnSpecularEnabledChanged([this](bool on)
    {
        m_enableSpecular = on;
        for (auto& p : m_panes) p->Viewport().SetEnableSpecular(on);
    });
    m_controls->SetOnGlowEnabledChanged([this](bool on)
    {
        m_enableGlow = on;
        for (auto& p : m_panes) p->Viewport().SetEnableGlow(on);
    });
    m_controls->SetOnLightingEnabledChanged([this](bool on)
    {
        m_enableLighting = on;
        for (auto& p : m_panes) p->Viewport().SetEnableLighting(on);
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
    if (m_renderDevice)
    {
        pane->SetRenderDevice(m_renderDevice);
    }
    if (m_resourceManager)
    {
        pane->SetResourceManager(m_resourceManager);
    }
    pane->Viewport().SetParallaxHeightScale(m_parallaxHeightScale);
    pane->Viewport().SetEnableParallax(m_enableParallax);
    pane->Viewport().SetEnableComplexMaterial(m_enableComplexMaterial);
    pane->Viewport().SetEnablePBR(m_enablePBR);
    pane->Viewport().SetShowHiddenNodes(m_showHiddenNodes);
    pane->Viewport().SetShowNormals(m_showNormals);
    pane->Viewport().SetShowTangents(m_showTangents);
    pane->Viewport().SetMsaaEnabled(m_msaaEnabled);
    pane->Viewport().SetEnableTextures(m_enableTextures);
    pane->Viewport().SetEnableVertexColors(m_enableVertexColors);
    pane->Viewport().SetEnableSpecular(m_enableSpecular);
    pane->Viewport().SetEnableGlow(m_enableGlow);
    pane->Viewport().SetEnableLighting(m_enableLighting);
    pane->SetThumbnailStripEnabled(m_thumbStripEnabled); // new panes inherit the global toggle
    pane->SetThumbnailStripSize(m_thumbStripExtent);     // ... and the current card size
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
    pane->SetOnFileOpened([this](const std::wstring& path)
    {
        if (m_onFileOpened)
            m_onFileOpened(path);
        // The pane refreshes its own thumbnail strip in Load().
    });
    pane->SetOnThumbnailChosen([this, paneRaw](const std::wstring& path)
    {
        SyncThumbnailSelection(paneRaw, path);
    });
    pane->SetOnThumbnailStripResize([this](float ext, bool committed)
    {
        // Keep every pane's strip the same size as the one being dragged;
        // persist only once the drag settles.
        SetThumbnailStripSize(ext);
        if (committed && m_onThumbStripSizeCommitted)
            m_onThumbStripSizeCommitted(ext);
    });
}

void NifCompareView::ApplyThumbnailPick(NifComparePane* active, const std::wstring& path)
{
    if (!active || path.empty())
        return;
    std::string err;
    active->Load(path, &err);      // moves this pane's strip highlight (ShowForFile)
    SyncThumbnailSelection(active, path); // mirror into the other panes, like a click
}

void NifCompareView::StepActiveThumbnail(int delta)
{
    if (NifComparePane* active = ActivePane())
        ApplyThumbnailPick(active, active->StepThumbnailFile(delta));
}

void NifCompareView::LoadEdgeThumbnail(bool last)
{
    if (NifComparePane* active = ActivePane())
        ApplyThumbnailPick(active, active->EdgeThumbnailFile(last));
}

void NifCompareView::SyncThumbnailSelection(NifComparePane* source, const std::wstring& path)
{
    if (!m_syncThumbnails)
        return;
    const std::wstring fileName = std::filesystem::path(path).filename().wstring();
    if (fileName.empty())
        return;
    for (auto& pane : m_panes)
    {
        if (pane.get() == source)
            continue;
        // Look for the same file name in this pane's own folder (the parent of
        // its currently-open .nif); load it if present. Panes whose folder
        // lacks that name are left as-is.
        const std::wstring current = pane->CurrentPath();
        if (current.empty())
            continue;
        std::error_code ec;
        const std::filesystem::path candidate =
            std::filesystem::path(current).parent_path() / fileName;
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            std::string err;
            pane->Load(candidate.wstring(), &err);
        }
    }
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

void NifCompareView::SetOnFileOpened(std::function<void(const std::wstring&)> handler)
{
    m_onFileOpened = std::move(handler);
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
        // A still-loading pane reads as occupied (CurrentPath() is its pending
        // path), so a burst of opens doesn't stack onto one not-yet-filled pane.
        if (pane && pane->CurrentPath().empty())
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

void NifCompareView::ApplyThumbStripEnabled(bool on)
{
    m_thumbStripEnabled = on;
    for (auto& p : m_panes)
        p->SetThumbnailStripEnabled(on);
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::SetThumbnailStripSize(float extent)
{
    m_thumbStripExtent = extent;
    for (auto& p : m_panes)
        p->SetThumbnailStripSize(extent);
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::SetOnThumbnailStripSizeChanged(std::function<void(float)> handler)
{
    m_onThumbStripSizeCommitted = std::move(handler);
}

bool NifCompareView::OnInputEvent(const FD2D::InputEvent& event)
{
    // Clicks over the texture inspector overlay belong to IT, not to the
    // viewport underneath (row select / channel cycle / plain swallow).
    if (event.type == FD2D::InputEventType::MouseDown && event.hasPoint &&
        event.button == FD2D::MouseButton::Left &&
        HandleTextureInspectorClick(event.point))
        return true;

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

namespace
{
    std::wstring Utf8ToWideStr(const std::string& s)
    {
        if (s.empty())
            return std::wstring();
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }

    // Texture paths are long; the interesting part is the tail (file name +
    // a bit of the folder).
    std::wstring PathTail(const std::string& path, std::size_t maxChars = 34)
    {
        std::wstring w = Utf8ToWideStr(path);
        if (w.size() <= maxChars)
            return w;
        return L"…" + w.substr(w.size() - maxChars);
    }

    std::wstring F2(float v)  { return std::format(L"{:.2f}", v); }
    std::wstring Vec2Str(const Vector2& v) { return std::format(L"{:.2f}, {:.2f}", v[0], v[1]); }
    std::wstring Col3Str(const Color3& c)  { return std::format(L"{:.2f} {:.2f} {:.2f}", c[0], c[1], c[2]); }
    std::wstring Hex32(std::uint32_t v)    { return std::format(L"{:08X}", v); }
}

void NifCompareView::DrawMaterialDiffPanel(ID2D1RenderTarget* target)
{
    if (!m_showMaterialPanel)
        return;
    NifComparePane* active = ActivePane();
    if (active == nullptr)
        return;
    const RenderMesh* sel = active->Viewport().SelectedMesh();
    if (sel == nullptr)
        return;

    // Columns: every loaded pane in order (up to 4), the active pane's
    // selection matched into the others by node name, then by index.
    struct Column
    {
        NifComparePane* pane = nullptr;
        const RenderMesh* mesh = nullptr;
        std::wstring header;
    };
    std::string selNameLower = sel->nodeName;
    for (char& c : selNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::vector<Column> cols;
    int paneNo = 0;
    for (const auto& p : m_panes)
    {
        ++paneNo;
        if (!p || p->Document() == nullptr)
            continue;
        Column col;
        col.pane = p.get();
        if (p.get() == active)
        {
            col.mesh = sel;
        }
        else
        {
            const std::vector<RenderMesh>& meshes = p->Viewport().Meshes();
            for (const RenderMesh& m : meshes)
            {
                std::string nameLower = m.nodeName;
                for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (nameLower == selNameLower)
                {
                    col.mesh = &m;
                    break;
                }
            }
            if (col.mesh == nullptr)
            {
                const int idx = active->Viewport().SelectedMeshIndex();
                if (idx >= 0 && static_cast<std::size_t>(idx) < meshes.size())
                    col.mesh = &meshes[static_cast<std::size_t>(idx)];
            }
        }
        col.header = L"Pane " + std::to_wstring(paneNo) + (p.get() == active ? L" ●" : L"");
        cols.push_back(col);
        if (cols.size() == 4)
            break;
    }
    if (cols.empty())
        return;

    // Rows: formatted values per column; a row "differs" when any value
    // deviates from the first column's.
    struct Row
    {
        std::wstring label;
        std::vector<std::wstring> vals;
        bool differs = false;
    };
    std::vector<Row> rows;
    const auto addRow = [&rows, &cols](const std::wstring& label, auto&& format, bool skipWhenAllEmpty = false)
    {
        Row r;
        r.label = label;
        bool anyContent = false;
        for (const Column& c : cols)
        {
            std::wstring v = c.mesh != nullptr ? format(*c.mesh, *c.pane) : std::wstring(L"-");
            anyContent = anyContent || (!v.empty() && v != L"-");
            r.vals.push_back(std::move(v));
        }
        if (skipWhenAllEmpty && !anyContent)
            return;
        for (const std::wstring& v : r.vals)
            r.differs = r.differs || v != r.vals.front();
        rows.push_back(std::move(r));
    };
    // Texture cells carry a resolve-source marker so two panes whose SAME
    // relative path lands on different sources (override vs vanilla, loose
    // vs archive) light up as a diff - the mod-conflict signal.
    const auto tex = [](const std::string& path, NifComparePane& pane) -> std::wstring
    {
        if (path.empty())
            return std::wstring();
        std::wstring v = PathTail(path, 26);
        if (TextureRepository::Entry* e = pane.Viewport().TextureEntry(path))
        {
            if (e->sourceKey.rfind("bsa:", 0) == 0)
                v += L"  (bsa)";
            else if (!e->sourceKey.empty())
                v += L"  (loose)";
        }
        return v;
    };

    addRow(L"Mesh",        [](const RenderMesh& m, NifComparePane&) { return Utf8ToWideStr(m.nodeName); });
    addRow(L"Triangles",   [](const RenderMesh& m, NifComparePane&) { return std::to_wstring(m.geometry ? m.geometry->triangles.size() : 0); });
    addRow(L"Shader",      [](const RenderMesh& m, NifComparePane& p) { return p.Viewport().ShaderKindFor(m); });
    addRow(L"Shader Type", [](const RenderMesh& m, NifComparePane&) { return std::to_wstring(m.material.shaderType); });
    addRow(L"SLSF1",       [](const RenderMesh& m, NifComparePane&) { return Hex32(m.material.shaderFlags1); });
    addRow(L"SLSF2",       [](const RenderMesh& m, NifComparePane&) { return Hex32(m.material.shaderFlags2); });
    addRow(L"Diffuse",     [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.diffuseTexture, p); }, true);
    addRow(L"Normal",      [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.normalTexture, p); }, true);
    addRow(L"Glow/Mask",   [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.glowTexture, p); }, true);
    addRow(L"Height",      [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.heightTexture, p); }, true);
    addRow(L"Cube Map",    [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.cubeTexture, p); }, true);
    addRow(L"Env Mask",    [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.envMaskTexture, p); }, true);
    addRow(L"Inner/Tint",  [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.innerTexture, p); }, true);
    addRow(L"Backlight",   [&](const RenderMesh& m, NifComparePane& p) { return tex(m.material.backlightTexture, p); }, true);
    addRow(L"Spec Color",  [](const RenderMesh& m, NifComparePane&) { return Col3Str(m.material.specularColor); });
    addRow(L"Spec Strength", [](const RenderMesh& m, NifComparePane&) { return F2(m.material.specularStrength); });
    addRow(L"Glossiness",  [](const RenderMesh& m, NifComparePane&) { return F2(m.material.glossiness); });
    addRow(L"Emissive",    [](const RenderMesh& m, NifComparePane&) { return Col3Str(m.material.emissiveColor); });
    addRow(L"Emissive Mult", [](const RenderMesh& m, NifComparePane&) { return F2(m.material.emissiveMultiple); });
    addRow(L"Alpha",       [](const RenderMesh& m, NifComparePane&) { return F2(m.material.alpha); });
    addRow(L"UV Scale",    [](const RenderMesh& m, NifComparePane&) { return Vec2Str(m.material.uvScale); });
    addRow(L"UV Offset",   [](const RenderMesh& m, NifComparePane&) { return Vec2Str(m.material.uvOffset); });
    addRow(L"EnvMap Scale", [](const RenderMesh& m, NifComparePane&) { return F2(m.material.environmentReflection); });
    addRow(L"Light Eff 1/2", [](const RenderMesh& m, NifComparePane&) { return F2(m.material.lightingEffect1) + L" / " + F2(m.material.lightingEffect2); });
    addRow(L"Alpha Blend", [](const RenderMesh& m, NifComparePane&)
    {
        return m.material.hasAlphaBlend
            ? L"On " + std::to_wstring(m.material.alphaSrcBlend) + L"/" + std::to_wstring(m.material.alphaDstBlend)
            : std::wstring(L"Off");
    });
    addRow(L"Alpha Test",  [](const RenderMesh& m, NifComparePane&)
    {
        return m.material.hasAlphaTest ? L"On " + F2(m.material.alphaTestThreshold) : std::wstring(L"Off");
    });
    addRow(L"Depth Write", [](const RenderMesh& m, NifComparePane&) { return std::wstring(m.material.depthWrite ? L"On" : L"Off"); });
    addRow(L"Double Sided", [](const RenderMesh& m, NifComparePane&) { return std::wstring(m.material.isDoubleSided ? L"Yes" : L"No"); });
    addRow(L"Decal",       [](const RenderMesh& m, NifComparePane&) { return std::wstring(m.material.isDecal ? L"Yes" : L"No"); });

    // --- layout & draw ----------------------------------------------------
    if (!m_matPanelText)
    {
        FD2D::Core::DWriteFactory()->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"", &m_matPanelText);
        if (m_matPanelText)
            m_matPanelText->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!m_matPanelText)
        return;

    constexpr float kRowH = 17.0f;
    constexpr float kLabelW = 108.0f;
    constexpr float kColW = 208.0f;
    constexpr float kPad = 8.0f;
    const float panelW = kLabelW + kColW * cols.size() + kPad * 2.0f;
    const float panelH = kRowH * (rows.size() + 1) + kPad * 2.0f;

    const D2D1_RECT_F view = LayoutRect();
    D2D1_RECT_F panel {
        view.right - panelW - 12.0f,
        view.top + 34.0f,
        view.right - 12.0f,
        view.top + 34.0f + panelH,
    };

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.06f, 0.90f), &brush)))
        return;
    target->FillRectangle(panel, brush.Get());
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
    target->DrawRectangle(panel, brush.Get(), 1.0f);

    const auto drawCell = [&](const std::wstring& text, float x, float y, float w, const D2D1_COLOR_F& color)
    {
        brush->SetColor(color);
        const D2D1_RECT_F rc { x, y, x + w - 6.0f, y + kRowH };
        target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), m_matPanelText.Get(), rc, brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const D2D1_COLOR_F kLabelCol  = D2D1::ColorF(0.60f, 0.63f, 0.67f);
    const D2D1_COLOR_F kValueCol  = D2D1::ColorF(0.88f, 0.88f, 0.90f);
    const D2D1_COLOR_F kDiffCol   = D2D1::ColorF(1.00f, 0.62f, 0.25f);
    const D2D1_COLOR_F kHeaderCol = D2D1::ColorF(0.52f, 0.56f, 0.61f);

    float y = panel.top + kPad;
    drawCell(L"MATERIAL DIFF (I)", panel.left + kPad, y, kLabelW, kHeaderCol);
    for (std::size_t c = 0; c < cols.size(); ++c)
        drawCell(cols[c].header, panel.left + kPad + kLabelW + kColW * c, y, kColW, kHeaderCol);
    y += kRowH;
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    target->DrawLine({ panel.left + kPad, y }, { panel.right - kPad, y }, brush.Get(), 1.0f);

    for (const Row& row : rows)
    {
        drawCell(row.label, panel.left + kPad, y, kLabelW, kLabelCol);
        for (std::size_t c = 0; c < row.vals.size(); ++c)
        {
            const bool highlight = row.differs && (c == 0 || row.vals[c] != row.vals.front());
            drawCell(row.vals[c], panel.left + kPad + kLabelW + kColW * c, y, kColW,
                     highlight ? kDiffCol : kValueCol);
        }
        y += kRowH;
    }
}

namespace
{
    const wchar_t* FormatName(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return L"BC1";
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return L"BC2";
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return L"BC3";
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:      return L"BC4";
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:      return L"BC5";
        case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:      return L"BC6H";
        case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return L"BC7";
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return L"RGBA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return L"BGRA8";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return L"BGRX8";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"RGBA16F";
        case DXGI_FORMAT_R8_UNORM: return L"R8";
        case DXGI_FORMAT_UNKNOWN: return L"?";
        default: return L"other";
        }
    }

    // Human-readable resolved source: "loose …tail" / "bsa Archive.bsa".
    std::wstring SourceLabel(const std::string& sourceKey)
    {
        if (sourceKey.rfind("file:", 0) == 0)
            return L"loose " + PathTail(sourceKey.substr(5), 30);
        if (sourceKey.rfind("bsa:", 0) == 0)
        {
            const std::size_t bar = sourceKey.find('|');
            const std::string archive = sourceKey.substr(4, bar == std::string::npos ? std::string::npos : bar - 4);
            return L"bsa " + Utf8ToWideStr(std::filesystem::path(archive).filename().string());
        }
        return sourceKey.empty() ? L"(not loaded)" : Utf8ToWideStr(sourceKey);
    }

    struct TexSlotRef { const wchar_t* name; const std::string* path; };
    std::vector<TexSlotRef> GatherTexSlots(const NifMaterial& m)
    {
        const TexSlotRef all[] = {
            { L"Diffuse",   &m.diffuseTexture },
            { L"Normal",    &m.normalTexture },
            { L"Glow/Mask", &m.glowTexture },
            { L"Height",    &m.heightTexture },
            { L"Cube",      &m.cubeTexture },
            { L"Env Mask",  &m.envMaskTexture },
            { L"Inner",     &m.innerTexture },
            { L"Backlight", &m.backlightTexture },
            { L"Greyscale", &m.greyscaleTexture },
        };
        std::vector<TexSlotRef> out;
        for (const TexSlotRef& s : all)
        {
            if (!s.path->empty())
                out.push_back(s);
        }
        return out;
    }

    const wchar_t* kChannelNames[] = { L"RGB", L"R", L"G", L"B", L"A" };
}

bool NifCompareView::EnsureTexturePreview(ID2D1RenderTarget* target, NifComparePane& pane, const std::string& relPath)
{
    const std::wstring key = pane.Viewport().NifDirectory() + L"|" + Utf8ToWideStr(relPath)
                           + L"|" + std::to_wstring(m_texChannelMode);
    if (m_texPreviewBitmap && m_texPreviewOwner == target && m_texPreviewKey == key)
        return true;
    m_texPreviewBitmap.Reset();
    m_texPreviewOwner = nullptr;
    m_texPreviewKey.clear();

    if (m_resolver == nullptr)
        return false;
    ResourceBytes found = m_resolver->Find(relPath, pane.Viewport().NifDirectory());
    if (!found.ok())
        return false;

    DirectX::TexMetadata meta {};
    DirectX::ScratchImage img;
    HRESULT hr = !found.diskPath.empty()
        ? DirectX::LoadFromDDSFile(found.diskPath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, img)
        : DirectX::LoadFromDDSMemory(found.data.data(), found.data.size(), DirectX::DDS_FLAGS_NONE, &meta, img);
    if (FAILED(hr) || meta.mipLevels == 0)
        return false;

    // Pick the smallest mip that still fills the preview box, keeping the
    // CPU decode cheap for 4K sources.
    std::size_t mip = 0;
    while (mip + 1 < meta.mipLevels &&
           (std::max)(meta.width >> (mip + 1), meta.height >> (mip + 1)) >= 256)
        ++mip;

    const DirectX::Image* src = img.GetImage(mip, 0, 0);
    if (src == nullptr)
        return false;
    DirectX::ScratchImage decoded;
    const DirectX::Image* px = nullptr;
    if (DirectX::IsCompressed(meta.format))
    {
        if (FAILED(DirectX::Decompress(*src, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)))
            return false;
        px = decoded.GetImage(0, 0, 0);
    }
    else if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (FAILED(DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM,
                       DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, decoded)))
            return false;
        px = decoded.GetImage(0, 0, 0);
    }
    else
    {
        px = src;
    }
    if (px == nullptr || px->pixels == nullptr)
        return false;

    // RGBA8 -> BGRA8 with the channel transform baked in.
    std::vector<std::uint8_t> bgra(px->width * px->height * 4);
    for (std::size_t y = 0; y < px->height; ++y)
    {
        const std::uint8_t* srow = px->pixels + y * px->rowPitch;
        std::uint8_t* drow = bgra.data() + y * px->width * 4;
        for (std::size_t x = 0; x < px->width; ++x)
        {
            const std::uint8_t r = srow[x * 4 + 0], g = srow[x * 4 + 1];
            const std::uint8_t b = srow[x * 4 + 2], a = srow[x * 4 + 3];
            std::uint8_t db = b, dg = g, dr = r;
            switch (m_texChannelMode)
            {
            case 1: db = dg = dr = r; break;
            case 2: db = dg = dr = g; break;
            case 3: db = dg = dr = b; break;
            case 4: db = dg = dr = a; break;
            default: break;
            }
            drow[x * 4 + 0] = db;
            drow[x * 4 + 1] = dg;
            drow[x * 4 + 2] = dr;
            drow[x * 4 + 3] = 0xFF;
        }
    }

    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(target->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(px->width), static_cast<UINT32>(px->height)),
            bgra.data(), static_cast<UINT32>(px->width * 4), props, &m_texPreviewBitmap)))
        return false;

    m_texPreviewOwner = target;
    m_texPreviewKey = key;
    m_texPreviewAspect = px->height > 0
        ? static_cast<float>(px->width) / static_cast<float>(px->height) : 1.0f;
    return true;
}

void NifCompareView::DrawTextureInspector(ID2D1RenderTarget* target)
{
    m_texPanelLive = false;
    m_texRowRects.clear();

    if (!m_showTextureInspector)
        return;
    NifComparePane* active = ActivePane();
    if (active == nullptr)
        return;
    const RenderMesh* sel = active->Viewport().SelectedMesh();
    if (sel == nullptr)
        return;
    const std::vector<TexSlotRef> slots = GatherTexSlots(sel->material);
    if (slots.empty())
        return;
    if (m_texInspectorRow >= static_cast<int>(slots.size()))
        m_texInspectorRow = 0;

    if (!m_matPanelText) // shared 12px format with the material panel
    {
        FD2D::Core::DWriteFactory()->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"", &m_matPanelText);
        if (m_matPanelText)
            m_matPanelText->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!m_matPanelText)
        return;

    constexpr float kRowH = 17.0f;
    constexpr float kNameW = 72.0f;
    constexpr float kMetaW = 128.0f;
    constexpr float kSourceW = 240.0f;
    constexpr float kPad = 8.0f;
    constexpr float kPreview = 256.0f;

    const float panelW = kNameW + kMetaW + kSourceW + kPad * 2.0f;
    const float panelH = kRowH * (slots.size() + 2) + kPreview + kPad * 3.0f;
    const D2D1_RECT_F view = LayoutRect();
    const D2D1_RECT_F panel { view.left + 12.0f, view.top + 34.0f,
                              view.left + 12.0f + panelW, view.top + 34.0f + panelH };

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.06f, 0.90f), &brush)))
        return;
    target->FillRectangle(panel, brush.Get());
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
    target->DrawRectangle(panel, brush.Get(), 1.0f);

    const auto drawCell = [&](const std::wstring& text, float x, float y, float w, const D2D1_COLOR_F& color)
    {
        brush->SetColor(color);
        const D2D1_RECT_F rc { x, y, x + w - 6.0f, y + kRowH };
        target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), m_matPanelText.Get(), rc, brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const D2D1_COLOR_F kLabelCol  = D2D1::ColorF(0.60f, 0.63f, 0.67f);
    const D2D1_COLOR_F kValueCol  = D2D1::ColorF(0.88f, 0.88f, 0.90f);
    const D2D1_COLOR_F kAccent    = D2D1::ColorF(0.45f, 0.70f, 1.00f);
    const D2D1_COLOR_F kHeaderCol = D2D1::ColorF(0.52f, 0.56f, 0.61f);

    float y = panel.top + kPad;
    drawCell(L"TEXTURES (T)  ·  click a row, click the preview to cycle " +
             std::wstring(kChannelNames[m_texChannelMode]),
             panel.left + kPad, y, panelW - kPad * 2.0f, kHeaderCol);
    y += kRowH;
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    target->DrawLine({ panel.left + kPad, y }, { panel.right - kPad, y }, brush.Get(), 1.0f);

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        const D2D1_RECT_F rowRect { panel.left + 2.0f, y, panel.right - 2.0f, y + kRowH };
        m_texRowRects.push_back(rowRect);
        const bool selRow = static_cast<int>(i) == m_texInspectorRow;
        if (selRow)
        {
            brush->SetColor(D2D1::ColorF(0.30f, 0.58f, 0.95f, 0.18f));
            target->FillRectangle(rowRect, brush.Get());
        }

        TextureRepository::Entry* e = active->Viewport().TextureEntry(*slots[i].path);
        std::wstring meta = e != nullptr && e->width > 0
            ? std::format(L"{}x{} {} {}m", e->width, e->height, FormatName(e->format), e->mipLevels)
            : std::wstring(L"(unresolved)");
        std::wstring source = e != nullptr ? SourceLabel(e->sourceKey) : std::wstring(L"-");

        drawCell(slots[i].name, panel.left + kPad, y, kNameW, selRow ? kAccent : kLabelCol);
        drawCell(meta, panel.left + kPad + kNameW, y, kMetaW, selRow ? kAccent : kValueCol);
        drawCell(source, panel.left + kPad + kNameW + kMetaW, y, kSourceW, selRow ? kAccent : kValueCol);
        y += kRowH;
    }

    // Preview of the selected slot.
    y += kPad * 0.5f;
    const std::string& previewPath = *slots[static_cast<std::size_t>(m_texInspectorRow)].path;
    drawCell(PathTail(previewPath, 52), panel.left + kPad, y, panelW - kPad * 2.0f, kLabelCol);
    y += kRowH;

    D2D1_RECT_F box { panel.left + kPad, y, panel.left + kPad + kPreview, y + kPreview };
    m_texPreviewHitRect = box;
    brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f));
    target->FillRectangle(box, brush.Get());
    if (EnsureTexturePreview(target, *active, previewPath) && m_texPreviewBitmap)
    {
        // Fit the bitmap into the box, preserving aspect.
        D2D1_RECT_F dst = box;
        if (m_texPreviewAspect >= 1.0f)
        {
            const float h = kPreview / m_texPreviewAspect;
            dst.top = box.top + (kPreview - h) * 0.5f;
            dst.bottom = dst.top + h;
        }
        else
        {
            const float w = kPreview * m_texPreviewAspect;
            dst.left = box.left + (kPreview - w) * 0.5f;
            dst.right = dst.left + w;
        }
        target->DrawBitmap(m_texPreviewBitmap.Get(), dst, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));
    target->DrawRectangle(box, brush.Get(), 1.0f);

    m_texPanelRect = panel;
    m_texPanelLive = true;
}

bool NifCompareView::HandleTextureInspectorClick(const POINT& pt)
{
    if (!m_texPanelLive || !FD2D::Util::RectContainsPoint(m_texPanelRect, pt))
        return false;

    if (FD2D::Util::RectContainsPoint(m_texPreviewHitRect, pt))
    {
        m_texChannelMode = (m_texChannelMode + 1) % 5; // RGB -> R -> G -> B -> A
        Invalidate();
        return true;
    }
    for (std::size_t i = 0; i < m_texRowRects.size(); ++i)
    {
        if (FD2D::Util::RectContainsPoint(m_texRowRects[i], pt))
        {
            m_texInspectorRow = static_cast<int>(i);
            Invalidate();
            return true;
        }
    }
    return true; // swallow clicks anywhere else on the panel
}

void NifCompareView::OnRenderOverlay(ID2D1RenderTarget* target)
{
    FD2D::SplitPanel::OnRenderOverlay(target);
    if (target == nullptr)
        return;

    DrawMaterialDiffPanel(target);
    DrawTextureInspector(target);

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

    case 'C': // focus the active pane's selected sub-mesh (whole scene when none)
        if (NifComparePane* active = ActivePane())
            active->Viewport().FocusOnSelection();
        return true;

    case 'I': // material diff panel (shows while a sub-mesh is selected)
        m_showMaterialPanel = !m_showMaterialPanel;
        Invalidate();
        return true;

    case 'T': // texture inspector (shows while a sub-mesh is selected)
        m_showTextureInspector = !m_showTextureInspector;
        Invalidate();
        return true;

    // Display toggles go through the control panel so the checkboxes stay
    // in sync (notify=true runs the same wired handlers a click would).
    case 'G': m_controls->ToggleShowGrid();   return true;
    case 'X': m_controls->ToggleShowAxes();   return true;
    case 'H': m_controls->ToggleShowHidden(); return true;

    // N toggles the vertex normal overlay; Shift+N the tangent overlay.
    case 'N':
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
            m_controls->ToggleShowTangents();
        else
            m_controls->ToggleShowNormals();
        return true;

    case 'M': m_controls->ToggleMsaa(); return true; // 4x MSAA on/off

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

    // Thumbnail navigation on the active pane's strip (FICture2's browser
    // keys); each load syncs into the other panes via Sync Files.
    //   Left / ',' : previous sibling    Right / '.' : next sibling
    //   Home : first file                End : last file
    //   Backspace / Ctrl+Up : parent folder
    case VK_LEFT:       StepActiveThumbnail(-1); return true;
    case VK_RIGHT:      StepActiveThumbnail(+1); return true;
    case VK_OEM_COMMA:  StepActiveThumbnail(-1); return true; // ',' / '<'
    case VK_OEM_PERIOD: StepActiveThumbnail(+1); return true; // '.' / '>'
    case VK_HOME:       LoadEdgeThumbnail(false); return true;
    case VK_END:        LoadEdgeThumbnail(true);  return true;

    case VK_BACK: // Backspace: browse the active pane's strip to the parent
        if (NifComparePane* active = ActivePane())
            active->NavigateThumbnailUp();
        return true;

    case VK_UP: // Ctrl+Up: same as Backspace (browse to the parent folder)
        if (ctrl)
        {
            if (NifComparePane* active = ActivePane())
                active->NavigateThumbnailUp();
            return true;
        }
        return false;

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
        // Ctrl+O opens into the active pane (FICture2's OpenImage);
        // Ctrl+Shift+O opens into a fresh pane (its OpenImageSplitNew).
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        {
            NifComparePane* target = AddPane(); // nullptr at kMaxPanes
            if (target == nullptr)
                target = ActivePane();          // fall back: reuse the active pane
            if (target != nullptr)
            {
                SetActivePane(target);
                RequestOpenPane(*target);
            }
        }
        else if (NifComparePane* target = ActivePane())
        {
            RequestOpenPane(*target);
        }
        return true;

    case VK_F4: // Ctrl+F4: close the active pane (FICture2's Close; == Ctrl+W)
        if (ctrl)
        {
            if (NifComparePane* active = ActivePane())
                RequestClosePane(*active);
            return true;
        }
        return false;

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

namespace
{
    // Pre-order over the host tree, visiting every SplitPanel (both the
    // horizontal in-row splits and the vertical two-row split). The tree
    // shape is deterministic for a given pane count, so a captured list
    // re-applies exactly after a rebuild with the same count and lines up
    // as a best-effort prefix otherwise.
    void CaptureRatiosRecursive(const std::shared_ptr<FD2D::Wnd>& node, std::vector<float>& out)
    {
        if (!node)
            return;
        if (auto sp = std::dynamic_pointer_cast<FD2D::SplitPanel>(node))
            out.push_back(sp->SplitRatio());
        for (const auto& child : node->ChildrenInOrder())
            CaptureRatiosRecursive(child, out);
    }

    void ApplyRatiosRecursive(const std::shared_ptr<FD2D::Wnd>& node, const std::vector<float>& ratios, std::size_t& idx)
    {
        if (!node)
            return;
        if (auto sp = std::dynamic_pointer_cast<FD2D::SplitPanel>(node))
        {
            if (idx < ratios.size())
                sp->SetSplitRatio(ratios[idx]);
            ++idx;
        }
        for (const auto& child : node->ChildrenInOrder())
            ApplyRatiosRecursive(child, ratios, idx);
    }
}

std::vector<float> NifCompareView::CaptureSplitRatios() const
{
    std::vector<float> out;
    CaptureRatiosRecursive(m_hostRoot, out);
    return out;
}

void NifCompareView::ApplySplitRatios(const std::vector<float>& ratios)
{
    if (ratios.empty())
        return;
    std::size_t idx = 0;
    ApplyRatiosRecursive(m_hostRoot, ratios, idx);
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::RebuildViewsArea()
{
    if (!m_viewsArea)
        return;
    // The pane grid fills the whole views area (each pane hosts its own
    // thumbnail strip now). ClearDocks drops the previous host + its stale
    // dock-order entry (kept alive by m_hostRoot) so the new tree isn't
    // starved of space by a leftover Fill entry.
    m_viewsArea->ClearDocks();
    if (m_hostRoot)
    {
        m_viewsArea->AddChild(m_hostRoot);
        m_viewsArea->SetChildDock(m_hostRoot, FD2D::Dock::Fill);
    }
}

void NifCompareView::SetOnThumbnailStripEnabledChanged(std::function<void(bool)> handler)
{
    m_onThumbStripEnabledChanged = std::move(handler);
}

void NifCompareView::SetThumbnailStripEnabled(bool enabled, bool notify)
{
    // Reflect in the checkbox without re-firing its handler, then apply to
    // every pane; report to the owner only when asked (restore passes false).
    m_controls->SetThumbnailStripChecked(enabled, /*notify=*/false);
    ApplyThumbStripEnabled(enabled);
    if (notify && m_onThumbStripEnabledChanged)
        m_onThumbStripEnabledChanged(enabled);
}

bool NifCompareView::IsThumbnailStripEnabled() const
{
    return m_controls->ThumbnailStripChecked();
}

void NifCompareView::ToggleThumbnailStrip()
{
    // Flips the checkbox WITH notify -> the wired handler broadcasts + persists.
    m_controls->ToggleThumbnailStrip();
}

void NifCompareView::RebuildHostTree()
{
    // Every caller of this changes the pane COUNT (add / remove / initial
    // build), which changes the split-tree shape. Carrying the old splitter
    // ratios across a shape change only mis-maps them positionally (a 2-pane
    // 0.35 drag would land on the first splitter of a 3-pane tree and skew
    // it), so the rebuild instead keeps the coordinator's equal-width
    // defaults - every pane comes out the same width. This is what the
    // "open several same-named NIFs into new panes" compare workflow wants.
    // Dragged ratios still persist across a full app session (saved to the
    // INI, re-applied by LoadAndOpenInitialSession via ApplySplitRatios).
    std::vector<std::shared_ptr<FD2D::Wnd>> wnds;
    wnds.reserve(m_panes.size());
    for (auto& p : m_panes)
        wnds.push_back(p);

    std::shared_ptr<FD2D::Wnd> host = NifCompareSplitCoordinator::BuildEqualWidthHostTree(wnds);

    // The host tree lives inside the persistent m_viewsArea DockPanel (which
    // also holds the thumbnail strip). RebuildViewsArea ClearChildren()s that
    // DockPanel and re-adds in dock order, so the superseded host tree can't
    // linger as ghost panes (the old worry when SetFirstChild only ADDED).
    m_hostRoot = host;
    if (host)
        m_hostName = host->Name();
    RebuildViewsArea();
    SetFirstChild(m_viewsArea);

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

void NifCompareView::SetRenderDevice(RenderDevice* device)
{
    m_renderDevice = device;
    for (auto& p : m_panes)
        p->SetRenderDevice(device);
}

void NifCompareView::SetResourceManager(ResourceManager* manager)
{
    m_resourceManager = manager;
    for (auto& p : m_panes)
        p->SetResourceManager(manager);
}

void NifCompareView::OnRenderD3D(ID3D11DeviceContext* context)
{
    // Apply completed async loads before the strips render this frame.
    if (m_resourceManager)
        m_resourceManager->DrainCompletions();
    FD2D::SplitPanel::OnRenderD3D(context); // propagate to panes/strips
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
