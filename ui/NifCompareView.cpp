#include "NifCompareView.h"
#include "../core/NifLog.h"
#include "../core/ResourceManager.h"

#include "ImageCore/ImageDecodeDispatcher.h" // which extensions route to an ImagePane

#include <VirtualPath.h> // Floar: is a path an archive to browse into?

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

    // Host the fixed-width control strip in a horizontal scroll view so a narrow
    // window scrolls (with a draggable bar) instead of clipping groups off-edge.
    m_controlsScroll = std::make_shared<FD2D::ScrollView>(name + L"_ControlsScroll");
    m_controlsScroll->SetContent(m_controls);
    // The strip reflows to fit the width (DynamicPanel wraps groups to rows), so
    // horizontal scroll is never needed; a vertical scrollbar only appears as a
    // last resort when the reflowed strip is capped shorter than its content.
    m_controlsScroll->SetHorizontalScrollEnabled(false);
    m_controlsScroll->SetVerticalScrollEnabled(true);
    m_controlsScroll->SetScrollBarsVisible(true);
    m_controlsScroll->SetPropagateMinSize(false);

    CreateInitialPanes();
    RebuildHostTree();

    SetSecondChild(m_controlsScroll);
    SetSplitRatio(0.85f);
    SetConstraintPropagation(FD2D::ConstraintPropagation::Minimum);
    RecalcControlStripExtent();

    m_controls->SetOnAddPane([this]() { AddPane(); });
    m_controls->SetOnResetCameras([this]()
    {
        ForEachViewport([](NifViewport& vp) { vp.ResetCamera(); });
    });
    m_controls->SetOnSyncViewsChanged([this](bool on) { m_syncViews = on; });
    m_controls->SetOnSyncLightingChanged([this](bool on) { m_syncLighting = on; });
    m_controls->SetOnSyncFilesChanged([this](bool on) { m_syncThumbnails = on; });
    m_controls->SetOnOrientationChanged([this](int idx) { ApplyOrientationPreset(idx); });

    // NAVIGATION group: global camera-feel tuning, broadcast to every pane and
    // remembered so new panes inherit it (see CreatePane).
    m_controls->SetOnMoveSensitivityChanged([this](float v)
    {
        m_navMoveSensitivity = v;
        ForEachViewport([v](NifViewport& vp) { vp.SetPanSensitivity(v); });
    });
    m_controls->SetOnZoomSensitivityChanged([this](float v)
    {
        m_navZoomSensitivity = v;
        ForEachViewport([v](NifViewport& vp) { vp.SetZoomSensitivity(v); });
    });
    m_controls->SetOnRotateSensitivityChanged([this](float v)
    {
        m_navRotateSensitivity = v;
        ForEachViewport([v](NifViewport& vp) { vp.SetOrbitSensitivity(v); });
    });
    m_controls->SetOnFovChanged([this](float deg)
    {
        m_fovRadians = deg * (NSK_PI / 180.0f);
        ForEachViewport([this](NifViewport& vp) { vp.SetFieldOfViewRadians(m_fovRadians); });
    });
    m_controls->SetOnOrthographicChanged([this](bool on) { ApplyOrthographic(on); });
    m_controls->SetOnOrbitSelectionChanged([this](bool on)
    {
        m_orbitAroundSelection = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetOrbitAroundSelection(on); });
    });
    m_controls->SetOnZoomToCursorChanged([this](bool on)
    {
        m_zoomToCursor = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetZoomToCursor(on); });
    });

    // ANIMATION group: playback of NIF-embedded transform animations. Actions
    // hit every pane while Sync Anim is on (compare two variants in phase),
    // else just the active pane.
    m_controls->SetOnAnimPlayClicked([this]() { ToggleAnimPlayback(); });
    m_controls->SetOnAnimSequenceChanged([this](int idx)
    {
        ForEachAnimTarget([idx](NifViewport& v) { v.SelectAnimSequence(idx); });
        RefreshAnimationControls();
    });
    m_controls->SetOnAnimTimeChanged([this](float t)
    {
        // Scrubbing pauses playback and poses at the dragged time (live).
        ForEachAnimTarget([t](NifViewport& v) { v.SetAnimPlaying(false); v.SetAnimTime(t); });
        m_controls->SetAnimPlayingDisplay(false);
    });
    m_controls->SetOnAnimLoopChanged([this](bool on)
    {
        ForEachAnimTarget([on](NifViewport& v) { v.SetAnimLoop(on); });
    });
    m_controls->SetOnAnimSpeedChanged([this](float v)
    {
        ForEachAnimTarget([v](NifViewport& vp) { vp.SetAnimSpeed(v); });
    });
    m_controls->SetOnSyncAnimationChanged([this](bool on) { m_syncAnimation = on; });

    m_controls->SetOnFrontalLightChanged([this](bool on)
    {
        ForEachViewport([on](NifViewport& vp) { vp.SetFrontalLight(on); });
    });
    m_controls->SetOnShowGridChanged([this](bool on)
    {
        ForEachViewport([on](NifViewport& vp) { vp.SetShowGrid(on); });
    });
    m_controls->SetOnShowAxesChanged([this](bool on)
    {
        ForEachViewport([on](NifViewport& vp) { vp.SetShowAxes(on); });
    });
    m_controls->SetOnWireframeChanged([this](bool on)
    {
        ForEachViewport([on](NifViewport& vp) { vp.SetWireframe(on); });
    });
    m_controls->SetOnShowHiddenChanged([this](bool on)
    {
        m_showHiddenNodes = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetShowHiddenNodes(on); });
    });
    m_controls->SetOnShowNormalsChanged([this](bool on)
    {
        m_showNormals = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetShowNormals(on); });
    });
    m_controls->SetOnShowTangentsChanged([this](bool on)
    {
        m_showTangents = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetShowTangents(on); });
    });
    m_controls->SetOnMsaaChanged([this](bool on)
    {
        m_msaaEnabled = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetMsaaEnabled(on); });
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
        if (m_syncLighting) { ForEachViewport([v](NifViewport& vp) { vp.SetBrightness(v); }); }
        else if (NifViewport* vp = FirstNifViewport()) { vp->SetBrightness(v); }
    });
    m_controls->SetOnAmbientChanged([this](float v)
    {
        if (m_syncLighting) { ForEachViewport([v](NifViewport& vp) { vp.SetAmbient(v); }); }
        else if (NifViewport* vp = FirstNifViewport()) { vp->SetAmbient(v); }
    });
    m_controls->SetOnDeclinationChanged([this](float v)
    {
        if (m_syncLighting) { ForEachViewport([v](NifViewport& vp) { vp.SetLightDeclinationDegrees(v); }); }
        else if (NifViewport* vp = FirstNifViewport()) { vp->SetLightDeclinationDegrees(v); }
    });
    m_controls->SetOnPlanarAngleChanged([this](float v)
    {
        if (m_syncLighting) { ForEachViewport([v](NifViewport& vp) { vp.SetLightPlanarAngleDegrees(v); }); }
        else if (NifViewport* vp = FirstNifViewport()) { vp->SetLightPlanarAngleDegrees(v); }
    });

    // Display setting rather than a light: always applies to every pane.
    m_controls->SetOnParallaxHeightChanged([this](float v)
    {
        m_parallaxHeightScale = v;
        ForEachViewport([v](NifViewport& vp) { vp.SetParallaxHeightScale(v); });
    });

    // Extended-material toggles - display settings, applied to every pane.
    m_controls->SetOnParallaxEnabledChanged([this](bool on)
    {
        m_enableParallax = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableParallax(on); });
    });
    m_controls->SetOnComplexMaterialEnabledChanged([this](bool on)
    {
        m_enableComplexMaterial = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableComplexMaterial(on); });
    });
    m_controls->SetOnPBREnabledChanged([this](bool on)
    {
        m_enablePBR = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnablePBR(on); });
    });

    m_controls->SetOnTexturesEnabledChanged([this](bool on)
    {
        m_enableTextures = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableTextures(on); });
    });
    m_controls->SetOnVertexColorsEnabledChanged([this](bool on)
    {
        m_enableVertexColors = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableVertexColors(on); });
    });
    m_controls->SetOnSpecularEnabledChanged([this](bool on)
    {
        m_enableSpecular = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableSpecular(on); });
    });
    m_controls->SetOnGlowEnabledChanged([this](bool on)
    {
        m_enableGlow = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableGlow(on); });
    });
    m_controls->SetOnLightingEnabledChanged([this](bool on)
    {
        m_enableLighting = on;
        ForEachViewport([on](NifViewport& vp) { vp.SetEnableLighting(on); });
    });
}

void NifCompareView::CreateInitialPanes()
{
    for (std::size_t i = 0; i < kDefaultInitialPanes; ++i)
    {
        m_panes.push_back(CreatePane());
    }
}

std::shared_ptr<ComparePane> NifCompareView::CreatePane()
{
    auto pane = std::make_shared<ComparePane>(NifCompareSplitCoordinator::NextPaneName());
    ComparePane* raw = pane.get();
    // Re-wire kind-specific callbacks whenever the content is (re)created.
    pane->SetOnContentCreated([this](PaneContent* c) { WireContent(c); });
    // Resources -> the frame forwards to the strip and the current content.
    if (m_resolver) pane->SetResourceResolver(m_resolver);
    if (m_textureRepository) pane->SetTextureRepository(m_textureRepository);
    if (m_renderDevice) pane->SetRenderDevice(m_renderDevice);
    if (m_resourceManager) pane->SetResourceManager(m_resourceManager);
    pane->SetThumbnailStripEnabled(m_thumbStripEnabled); // new panes inherit the global toggle
    pane->SetThumbnailStripSize(m_thumbStripExtent);     // ... and the current card size
    WireThumbnailCallbacks(raw);
    // The ctor created the initial NIF content before SetOnContentCreated was
    // set, so wire that content now (future swaps go through the callback).
    WireContent(pane->Content());
    return pane;
}

void NifCompareView::WireContent(PaneContent* c)
{
    if (auto* nif = dynamic_cast<NifComparePane*>(c))
    {
        NifViewport& vp = nif->Viewport();
        vp.SetParallaxHeightScale(m_parallaxHeightScale);
        vp.SetEnableParallax(m_enableParallax);
        vp.SetEnableComplexMaterial(m_enableComplexMaterial);
        vp.SetEnablePBR(m_enablePBR);
        vp.SetShowHiddenNodes(m_showHiddenNodes);
        vp.SetShowNormals(m_showNormals);
        vp.SetShowTangents(m_showTangents);
        vp.SetMsaaEnabled(m_msaaEnabled);
        vp.SetOrthographic(m_orthographic);
        vp.SetPanSensitivity(m_navMoveSensitivity);
        vp.SetZoomSensitivity(m_navZoomSensitivity);
        vp.SetOrbitSensitivity(m_navRotateSensitivity);
        vp.SetFieldOfViewRadians(m_fovRadians);
        vp.SetOrbitAroundSelection(m_orbitAroundSelection);
        vp.SetZoomToCursor(m_zoomToCursor);
        vp.SetEnableTextures(m_enableTextures);
        vp.SetEnableVertexColors(m_enableVertexColors);
        vp.SetEnableSpecular(m_enableSpecular);
        vp.SetEnableGlow(m_enableGlow);
        vp.SetEnableLighting(m_enableLighting);

        NifComparePane* raw = nif;
        vp.SetOnCameraChanged([this, raw](const Camera& cam)
        {
            if (!m_syncViews || m_applyingSync) return;
            m_applyingSync = true;
            for (auto& other : m_panes)
            {
                NifComparePane* n = AsNif(other);
                if (n && n != raw)
                    n->Viewport().SetCamera(cam);
            }
            m_applyingSync = false;
        });
        vp.SetOnCameraAnimateRequested([this]() { EnsureCameraAnimTimer(); });
        nif->SetOnDocumentChanged([this]()
        {
            RefreshExtendedMaterialControls();
            RefreshAnimationControls();
            UpdateIpcOpenSnapshot();
        });
        nif->SetOnFileOpened([this](const std::wstring& path)
        {
            if (m_onFileOpened) m_onFileOpened(path);
        });
    }
    else if (auto* img = dynamic_cast<ImagePane*>(c))
    {
        // Sync Views: mirror this image's zoom/pan/channel onto other image panes.
        ImagePane* raw = img;
        img->SetOnViewChanged([this, raw](const ImagePane::ImageViewState& state)
        {
            if (!m_syncViews || m_applyingSync) return;
            m_applyingSync = true;
            for (auto& p : m_panes)
            {
                ImagePane* other = AsImage(p.get());
                if (other && other != raw)
                    other->SetViewState(state);
            }
            m_applyingSync = false;
        });
    }
}

namespace
{
    // True when `path`'s extension is one ImageCore can decode (dds/png/tga/
    // jpg/...), i.e. it should open in an ImagePane rather than a NIF pane.
    bool IsImagePath(const std::wstring& path)
    {
        std::wstring ext = std::filesystem::path(path).extension().wstring();
        for (wchar_t& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        if (ext.empty())
            return false;
        for (const std::wstring& supported : ImageCore::ImageDecodeDispatcher::GetSupportedExtensions())
            if (ext == supported)
                return true;
        return false;
    }

    // True when `path` is a container to browse into (the strip descends into it)
    // rather than a file to view: a plain directory, or an archive file (.bsa/
    // .ba2/.zip/...). ComparePane::Load routes these into the thumbnail strip.
    bool IsBrowsableContainer(const std::wstring& path)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            return true;
        auto vp = Floar::VirtualPath::Parse(path);
        return vp && vp->IsArchiveFile();
    }
}

void NifCompareView::CreatePanesForPaths(const std::vector<std::wstring>& paths)
{
    m_panes.clear();
    m_activePane = nullptr;

    for (const std::wstring& path : paths)
    {
        if (m_panes.size() >= kMaxPanes)
            break;
        std::shared_ptr<ComparePane> pane = CreatePane();
        if (IsImagePath(path) || IsBrowsableContainer(path))
        {
            // Images decode now; folders/archives descend into the strip - both
            // go through Load (ComparePane routes by kind). Only a real NIF file
            // takes the deferred-parse placeholder path below.
            pane->Load(path);
        }
        else if (NifComparePane* nif = pane->NifContent())
        {
            nif->ShowPendingFile(path); // NIF placeholder; StartAllPendingLoads parses it
        }
        m_panes.push_back(std::move(pane));
    }
    if (m_panes.empty())
        m_panes.push_back(CreatePane());

    RebuildHostTree();
}

ComparePane* NifCompareView::OpenPathInPane(ComparePane* pane, const std::wstring& path)
{
    if (!pane || path.empty())
        return pane;
    // The pane's content swaps to the file's kind in place; the strip persists.
    std::string error;
    pane->Load(path, &error);
    return pane;
}

void NifCompareView::WireThumbnailCallbacks(ComparePane* raw)
{
    raw->SetOnThumbnailChosen([this, raw](const std::wstring& path)
    {
        // In-place: the pane's content swaps to the picked file's kind (image or
        // NIF) while its strip persists, so browsing continues seamlessly. Safe
        // to swap synchronously here - the strip is never torn down. Then mirror
        // the same file name into the other panes' folders.
        OpenPathInPane(raw, path);
        SyncThumbnailSelection(raw, path);
        Invalidate();
    });
    raw->SetOnThumbnailStripResize([this](float ext, bool committed)
    {
        // Keep every pane's strip the same size as the one being dragged;
        // persist only once the drag settles.
        SetThumbnailStripSize(ext);
        if (committed && m_onThumbStripSizeCommitted)
            m_onThumbStripSizeCommitted(ext);
    });
}

void NifCompareView::ApplyThumbnailPick(ComparePane* active, const std::wstring& path)
{
    if (!active || path.empty())
        return;
    std::string err;
    active->Load(path, &err);      // moves this pane's strip highlight (ShowForFile)
    SyncThumbnailSelection(active, path); // mirror into the other panes, like a click
}

void NifCompareView::StepActiveThumbnail(int delta)
{
    if (ComparePane* active = ActivePane())
    {
        active->FocusThumbnailStrip(); // keyboard browsing keeps the strip focused (enables type-to-select)
        ApplyThumbnailPick(active, active->StepThumbnailFile(delta));
    }
}

void NifCompareView::LoadEdgeThumbnail(bool last)
{
    if (ComparePane* active = ActivePane())
    {
        active->FocusThumbnailStrip();
        ApplyThumbnailPick(active, active->EdgeThumbnailFile(last));
    }
}

void NifCompareView::SyncThumbnailSelection(ComparePane* source, const std::wstring& path)
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
        NifComparePane* n = AsNif(p);
        if (!n)
            continue;
        NifViewport& vp = n->Viewport();
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

void NifCompareView::RequestOpenPane(ComparePane& pane)
{
    if (m_onPaneOpenRequested)
        m_onPaneOpenRequested(pane);
}

void NifCompareView::SetOnFileOpened(std::function<void(const std::wstring&)> handler)
{
    m_onFileOpened = std::move(handler);
}

void NifCompareView::RequestClosePane(ComparePane& pane)
{
    QueueClosePane(pane.Name());
}

ComparePane* NifCompareView::AddPane()
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

ComparePane* NifCompareView::AllocatePaneFor()
{
    for (auto& pane : m_panes)
    {
        // A still-loading pane reads as occupied (CurrentPath() is its pending
        // path), so a burst of opens doesn't stack onto one not-yet-filled pane.
        if (pane && pane->CurrentPath().empty())
            return pane.get();
    }
    return AddPane(); // nullptr when already at kMaxPanes
}

void NifCompareView::PlaceQueuedIpcPanesNamesOnly()
{
    // Startup archive-scan window: create a named placeholder pane for each
    // queued IPC path WITHOUT loading (the load would block on the scan). Panes
    // appear immediately as the forwards arrive; LoadAllPendingPanes starts the
    // actual loads once the scan is ready. TryEnqueue already capped the queue
    // to the pane budget, so AllocatePaneFor is expected to succeed.
    if (!m_ipcQueue)
        return;
    bool placedAny = false;
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
        ComparePane* target = AllocatePaneFor();
        if (!target)
        {
            // At capacity (a race past the enqueue cap): put it back for the
            // post-scan normal drain, which has the spawn-a-new-instance path.
            std::lock_guard<std::mutex> lock(m_ipcQueue->mutex);
            m_ipcQueue->pending.push_front(std::move(path));
            break;
        }
        if (auto* nif = AsNif(target)) nif->ShowPendingFile(path);
        placedAny = true;
    }
    if (placedAny)
    {
        UpdateIpcOpenSnapshot(); // the new panes now "hold" their names for the gate
        Invalidate();
    }
}

void NifCompareView::LoadAllPendingPanes()
{
    StartAllPendingLoads();
    ShowAllThumbnailStrips();
}

void NifCompareView::StartAllPendingLoads()
{
    // Kick off the async load for every pane that is named but not yet loaded -
    // the initial session/command-line panes and any IPC panes placed names-only
    // during the scan wait. StartPendingLoad only queues the parse job (no
    // per-pane UI work, and it no-ops if already queued), so ALL main loads are
    // enqueued before any completion runs - keeping every pane's main NIF ahead
    // of the (lower-priority) folder thumbnails in the shared queue. Parsing
    // needs no archive scan, so this can (and does) run during the scan.
    ForEachNifPane([](NifComparePane& n) { n.StartPendingLoad(); });
}

void NifCompareView::ShowAllThumbnailStrips()
{
    // Show the thumbnail strips (folder listing + P3 thumbnails) so they take
    // their proper height right away; the mains still parse first (priority),
    // and each strip's thumbnails trail them. Called after the scan so a strip's
    // one-shot thumbnail render resolves archive textures.
    for (auto& p : m_panes) p->ShowThumbnailFolder(); // strip is on the base (all kinds)
}

void NifCompareView::RefreshTexturesAfterScan()
{
    // The archive scan landed: re-resolve every loaded pane's textures so the
    // BSA-backed ones (skipped as "not ready" while the scan ran) pop into the
    // models already on screen.
    ForEachNifPane([](NifComparePane& n) { n.RefreshTextures(); });
}

bool NifCompareView::OpenIntoBestPane(const std::wstring& path)
{
    if (path.empty())
        return false;

    ComparePane* target = AllocatePaneFor(); // content swaps to the file's kind on Load
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
    // Deliberately NOT snapshotting here: at this point no pane is named yet,
    // so it would wipe the SeedExpected seed to empty and make forwards that
    // arrive during startup match nothing. The snapshot is (re)built once panes
    // are named (RefreshIpcOpenSnapshot after CreateNamedPanes) and on every
    // document change.
}

void NifCompareView::RefreshIpcOpenSnapshot()
{
    UpdateIpcOpenSnapshot();
}

void NifCompareView::UpdateIpcOpenSnapshot()
{
    if (!m_ipcQueue)
        return;

    // Use each pane's CURRENT path (the loaded document's, or the pending path
    // while it is still loading), not just loaded documents: a pane that is
    // async-loading file X already "holds" X's name for the same-name IPC gate,
    // so a sibling forward of X lands in a new pane instead of being declined.
    std::vector<std::wstring> names;
    for (const auto& pane : m_panes)
    {
        if (!pane)
            continue;
        const std::wstring path = pane->CurrentPath();
        if (path.empty())
            continue;
        std::wstring name = IpcOpenQueue::FileNameLower(path);
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

ComparePane* NifCompareView::ActivePane() const
{
    for (const auto& p : m_panes)
    {
        if (p.get() == m_activePane)
            return m_activePane;
    }
    return m_panes.empty() ? nullptr : m_panes.front().get();
}

void NifCompareView::SetActivePane(ComparePane* pane)
{
    if (m_activePane == pane)
        return;
    m_activePane = pane;
    RefreshAnimationControls(); // ANIMATION group mirrors the active pane's player
    Invalidate();
}

void NifCompareView::ApplyThumbStripEnabled(bool on)
{
    m_thumbStripEnabled = on;
    for (auto& p : m_panes) p->SetThumbnailStripEnabled(on);
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::SetThumbnailStripSize(float extent)
{
    m_thumbStripExtent = extent;
    for (auto& p : m_panes) p->SetThumbnailStripSize(extent);
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

    // Control-strip collapse tab: toggle on press, and also swallow the release
    // over the tab so it never falls through to a pane (the viewport picks a
    // sub-mesh on MouseUp - same lesson as the material panel below).
    if (event.hasPoint && event.button == FD2D::MouseButton::Left &&
        FD2D::Util::RectContainsPoint(m_collapseTabRect, event.point))
    {
        if (event.type == FD2D::InputEventType::MouseDown)
        {
            SetControlStripCollapsed(!m_controlsCollapsed, /*notify=*/true);
            return true;
        }
        if (event.type == FD2D::InputEventType::MouseUp)
            return true;
    }

    // A material-panel column-width drag in progress owns every mouse event
    // until release.
    if (m_matResizing)
    {
        if (event.type == FD2D::InputEventType::MouseMove && event.hasPoint)
        {
            const int cols = (std::max)(m_matColCount, 1);
            const float delta = static_cast<float>(m_matResizeStartX - event.point.x) / cols;
            m_matColW = (std::clamp)(m_matColWStart + delta, 130.0f, 380.0f);
            Invalidate();
            return true;
        }
        if (event.type == FD2D::InputEventType::MouseUp)
        {
            m_matResizing = false;
            ReleaseCapture();
            return true;
        }
        return true; // swallow anything else mid-drag
    }

    // Track the hovered material-panel texture cell so the tooltip repaints as
    // the cursor moves across cells (mouse-move alone triggers no redraw).
    if (event.type == FD2D::InputEventType::MouseMove && event.hasPoint)
        UpdateMaterialHover(event.point);

    // Right-click on a material-panel texture cell copies its full relative
    // path (and swallows the event so no pane context menu appears).
    if (event.type == FD2D::InputEventType::MouseUp &&
        event.button == FD2D::MouseButton::Right &&
        event.hasPoint &&
        HandleMaterialPanelCopy(event.point))
        return true;

    // A press over the material panel belongs to IT: header toggles collapse, a
    // corner grip starts a width drag, and everything else is swallowed so the
    // pane behind the overlay doesn't become active or start an orbit drag.
    if (event.type == FD2D::InputEventType::MouseDown && event.hasPoint &&
        HandleMaterialPanelMouseDown(event.point))
        return true;

    // The overlay is drawn, not a child window, so a release over it would
    // otherwise fall through to the pane behind - and the viewport picks a
    // sub-mesh on MouseUp. Swallow every release over the live panel so a
    // header collapse/expand click never selects the mesh underneath.
    if (event.type == FD2D::InputEventType::MouseUp && event.hasPoint &&
        m_matPanelLive && FD2D::Util::RectContainsPoint(m_matPanelRect, event.point))
        return true;

    // Any click inside a pane makes it the active one (FICture2's focused
    // browser), BEFORE the children consume the event - a viewport orbit
    // drag or a path-label click both count as "working in this pane".
    if (event.type == FD2D::InputEventType::MouseDown && event.hasPoint)
    {
        if (ComparePane* hit = PaneAt(event.point))
            SetActivePane(hit);
    }

    if (FD2D::SplitPanel::OnInputEvent(event))
        return true;

    if (event.type == FD2D::InputEventType::MouseUp &&
        event.button == FD2D::MouseButton::Right &&
        event.hasPoint &&
        m_onContextMenuRequested)
    {
        // Per-pane menu items act on the pane under the cursor (any kind); the
        // app grays the NIF-only items for an image pane.
        m_onContextMenuRequested(event.point, PaneAt(event.point));
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

    // Translate a KeyDown into the printable character it would type on the
    // current keyboard layout (for thumbnail type-to-select). Returns false for
    // navigation/control keys (arrows, Enter, F-keys, ...) which ToUnicode maps
    // to nothing or a control char. Clears any pending dead-key state so it
    // never leaves a diacritic half-composed.
    bool TryGetPrintableChar(const FD2D::InputEvent& event, wchar_t& out)
    {
        BYTE keyState[256] {};
        if (!GetKeyboardState(keyState))
            return false;
        wchar_t buf[8] {};
        const int n = ToUnicode(event.keyCode, event.scanCode, keyState, buf, 8, 0);
        if (n < 0) // dead key: run once more to flush its state, then decline
        {
            wchar_t flush[8] {};
            (void)ToUnicode(event.keyCode, event.scanCode, keyState, flush, 8, 0);
            return false;
        }
        if (n == 0)
            return false;
        const wchar_t ch = buf[0];
        if (!std::iswprint(ch) || std::iswspace(ch))
            return false; // letters/digits/punctuation only
        out = ch;
        return true;
    }
}

ComparePane* NifCompareView::PaneAt(const POINT& clientPt) const
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
    // Accept anything we can open in a pane - a NIF, an image, or a container to
    // browse (a folder / archive). The target pane is converted to the matching
    // kind (or set browsing) on drop (OpenPathInPane -> ComparePane::Load).
    ComparePane* pane = (IsNifPath(path) || IsImagePath(path) || IsBrowsableContainer(path))
                            ? PaneAt(clientPt) : nullptr;
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

    std::vector<std::wstring> files;
    for (const std::wstring& p : paths)
    {
        if (IsNifPath(p) || IsImagePath(p) || IsBrowsableContainer(p))
            files.push_back(p);
    }
    if (files.empty())
        return false;

    ComparePane* hit = PaneAt(clientPt);
    if (hit == nullptr)
        return false; // matches the drag-over decline: no target, no drop

    // Same zone split the drag-over visual promised: right quarter inserts
    // a NEW pane right after the hovered one, the rest replaces it. Either way
    // OpenPathInPane converts the slot to the dropped file's kind and loads it.
    ComparePane* slot = hit;
    if (RelativeX(hit->LayoutRect(), clientPt) >= kInsertZoneRatio && m_panes.size() < kMaxPanes)
    {
        if (ComparePane* inserted = InsertPaneAfter(hit))
            slot = inserted;
    }

    ComparePane* target = OpenPathInPane(slot, files.front());
    SetActivePane(target); // dropping into a pane means "work here now"
    for (std::size_t i = 1; i < files.size(); ++i)
    {
        if (!OpenIntoBestPane(files[i]))
        {
            NIFLOG_WARN("Drop: no pane left for {} more dropped file(s).", files.size() - i);
            break;
        }
    }
    Invalidate();
    return true;
}

void NifCompareView::SetDragOverlay(ComparePane* pane, DragOverlayKind kind)
{
    if (m_dragOverlayPane == pane && m_dragOverlayKind == kind)
        return;
    m_dragOverlayPane = pane;
    m_dragOverlayKind = kind;
    Invalidate();
}

ComparePane* NifCompareView::InsertPaneAfter(ComparePane* after)
{
    if (m_panes.size() >= kMaxPanes)
        return nullptr;

    std::shared_ptr<ComparePane> pane = CreatePane();
    auto insertAt = m_panes.end();
    for (auto it = m_panes.begin(); it != m_panes.end(); ++it)
    {
        if (it->get() == after)
        {
            insertAt = it + 1;
            break;
        }
    }
    ComparePane* raw = pane.get();
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
    m_matPanelLive = false;   // recomputed below; stale hit rects must not linger
    m_matTexCells.clear();
    if (!m_showMaterialPanel)
        return;
    NifComparePane* active = AsNif(ActivePane());
    if (active == nullptr)
        return;
    const RenderMesh* sel = active->Viewport().SelectedMesh();
    if (sel == nullptr)
        return;

    // Columns: every loaded pane in order (up to kMaxPanes), the active pane's
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
        NifComparePane* n = AsNif(p);
        if (!n || n->Document() == nullptr)
            continue;
        Column col;
        col.pane = n;
        if (n == active)
        {
            col.mesh = sel;
        }
        else
        {
            const std::vector<RenderMesh>& meshes = n->Viewport().Meshes();
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
        col.header = L"Pane " + std::to_wstring(paneNo) + (n == active ? L" ●" : L"");
        cols.push_back(col);
        if (cols.size() == kMaxPanes)
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
        bool isTexture = false;                   // texture-slot rows get hover/copy
        std::vector<std::wstring> fullPaths;      // untruncated relative path per column
        std::vector<std::wstring> resolvedPaths;  // resolved source per column (loose abs / bsa)
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
    // Human-readable resolved source for a texture, mirrored into the tooltip:
    // "file:<abs>" -> the loose absolute path; "bsa:<archive>|<entry>" -> the
    // archive and entry it was extracted from.
    const auto texResolved = [](const std::string& sourceKey) -> std::wstring
    {
        if (sourceKey.rfind("file:", 0) == 0)
            return Utf8ToWideStr(sourceKey.substr(5));
        if (sourceKey.rfind("bsa:", 0) == 0)
        {
            const std::string body = sourceKey.substr(4);
            const std::size_t bar = body.find('|');
            if (bar != std::string::npos)
                return Utf8ToWideStr(body.substr(0, bar)) + L"  →  " + Utf8ToWideStr(body.substr(bar + 1));
            return Utf8ToWideStr(body);
        }
        return std::wstring();
    };
    // Texture rows stash the untruncated relative path and resolved source per
    // column so the panel can offer a hover tooltip and right-click-to-copy on
    // each texture cell (see m_matTexCells / HandleMaterialPanelCopy).
    const auto addTexRow = [&](const std::wstring& label, auto&& field)
    {
        Row r;
        r.label = label;
        r.isTexture = true;
        bool anyContent = false;
        for (const Column& c : cols)
        {
            std::string rel = c.mesh != nullptr ? field(*c.mesh) : std::string();
            std::wstring disp = c.mesh != nullptr ? tex(rel, *c.pane) : std::wstring(L"-");
            std::wstring resolved;
            if (!rel.empty())
                if (TextureRepository::Entry* e = c.pane->Viewport().TextureEntry(rel))
                    resolved = texResolved(e->sourceKey);
            anyContent = anyContent || !rel.empty();
            r.vals.push_back(std::move(disp));
            r.fullPaths.push_back(Utf8ToWideStr(rel));
            r.resolvedPaths.push_back(std::move(resolved));
        }
        if (!anyContent)
            return;
        for (const std::wstring& v : r.vals)
            r.differs = r.differs || v != r.vals.front();
        rows.push_back(std::move(r));
    };

    addRow(L"Mesh",        [](const RenderMesh& m, NifComparePane&) { return Utf8ToWideStr(m.nodeName); });
    addRow(L"Triangles",   [](const RenderMesh& m, NifComparePane&) { return std::to_wstring(m.geometry ? m.geometry->triangles.size() : 0); });
    addRow(L"Shader",      [](const RenderMesh& m, NifComparePane& p) { return p.Viewport().ShaderKindFor(m); });
    addRow(L"Shader Type", [](const RenderMesh& m, NifComparePane&) { return std::to_wstring(m.material.shaderType); });
    addRow(L"SLSF1",       [](const RenderMesh& m, NifComparePane&) { return Hex32(m.material.shaderFlags1); });
    addRow(L"SLSF2",       [](const RenderMesh& m, NifComparePane&) { return Hex32(m.material.shaderFlags2); });
    addTexRow(L"Diffuse",     [](const RenderMesh& m) -> const std::string& { return m.material.diffuseTexture; });
    addTexRow(L"Normal",      [](const RenderMesh& m) -> const std::string& { return m.material.normalTexture; });
    addTexRow(L"Glow/Mask",   [](const RenderMesh& m) -> const std::string& { return m.material.glowTexture; });
    addTexRow(L"Height",      [](const RenderMesh& m) -> const std::string& { return m.material.heightTexture; });
    addTexRow(L"Cube Map",    [](const RenderMesh& m) -> const std::string& { return m.material.cubeTexture; });
    addTexRow(L"Env Mask",    [](const RenderMesh& m) -> const std::string& { return m.material.envMaskTexture; });
    addTexRow(L"Inner/Tint",  [](const RenderMesh& m) -> const std::string& { return m.material.innerTexture; });
    addTexRow(L"Backlight",   [](const RenderMesh& m) -> const std::string& { return m.material.backlightTexture; });
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
    constexpr float kPad = 8.0f;
    const D2D1_RECT_F view = LayoutRect();
    m_matColCount = static_cast<int>(cols.size());
    // The desired column width (grip-adjustable) is capped so the whole table -
    // up to kMaxPanes columns - still fits between the view's side margins; with
    // many panes the columns auto-shrink rather than run off the left edge.
    const float availW = (view.right - view.left) - 24.0f - kLabelW - kPad * 2.0f;
    const float fitColW = availW > 0.0f ? availW / static_cast<float>(cols.size()) : m_matColW;
    const float colW = (std::max)(70.0f, (std::min)(m_matColW, fitColW));
    const float panelW = kLabelW + colW * cols.size() + kPad * 2.0f;
    const std::size_t bodyRows = m_matPanelCollapsed ? 0 : rows.size();
    const float panelH = kRowH * (bodyRows + 1) + kPad * 2.0f; // +1 = header row

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

    // Header row: title (+ per-pane headers when expanded) and a collapse
    // chevron on the right; clicking anywhere on the row toggles collapse.
    float y = panel.top + kPad;
    drawCell(L"MATERIAL DIFF (I)", panel.left + kPad, y, kLabelW, kHeaderCol);
    if (!m_matPanelCollapsed)
        for (std::size_t c = 0; c < cols.size(); ++c)
            drawCell(cols[c].header, panel.left + kPad + kLabelW + colW * c, y, colW, kHeaderCol);
    {
        const D2D1_RECT_F cr { panel.right - kPad - 16.0f, y, panel.right - kPad, y + kRowH };
        brush->SetColor(kHeaderCol);
        target->DrawTextW(m_matPanelCollapsed ? L"▸" : L"▾", 1, m_matPanelText.Get(), cr, brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    m_matHeaderRect = { panel.left, panel.top, panel.right, y + kRowH };
    y += kRowH;

    m_matPanelRect = panel;
    m_matPanelLive = true;
    m_matResizeGrip = {};
    if (m_matPanelCollapsed)
        return; // header only - body, grip, and tooltip are all suppressed

    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
    target->DrawLine({ panel.left + kPad, y }, { panel.right - kPad, y }, brush.Get(), 1.0f);

    for (const Row& row : rows)
    {
        drawCell(row.label, panel.left + kPad, y, kLabelW, kLabelCol);
        for (std::size_t c = 0; c < row.vals.size(); ++c)
        {
            const float cx = panel.left + kPad + kLabelW + colW * c;
            const bool highlight = row.differs && (c == 0 || row.vals[c] != row.vals.front());
            drawCell(row.vals[c], cx, y, colW, highlight ? kDiffCol : kValueCol);
            // Remember non-empty texture cells for hover/copy hit-testing.
            if (row.isTexture && c < row.fullPaths.size() && !row.fullPaths[c].empty())
                m_matTexCells.push_back({ { cx, y, cx + colW - 6.0f, y + kRowH },
                                          row.fullPaths[c], row.resolvedPaths[c] });
        }
        y += kRowH;
    }

    // Resize grip in the bottom-left corner: dragging it left/right widens or
    // narrows the value columns (the right edge stays pinned).
    const D2D1_RECT_F grip { panel.left, panel.bottom - 14.0f, panel.left + 14.0f, panel.bottom };
    m_matResizeGrip = grip;
    brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f));
    for (int i = 0; i < 3; ++i)
    {
        const float o = 4.0f + i * 4.0f;
        target->DrawLine({ grip.left + 2.0f, grip.bottom - o }, { grip.left + o, grip.bottom - 2.0f },
                         brush.Get(), 1.0f);
    }

    // Hover tooltip: when the cursor rests on a texture cell, expand the
    // truncated path to its full relative path plus resolved source.
    FD2D::Backplate* bp = BackplateRef();
    POINT cur {};
    if (bp && bp->Window() && GetCursorPos(&cur) && ScreenToClient(bp->Window(), &cur))
    {
        const D2D1_POINT_2F cp { static_cast<float>(cur.x), static_cast<float>(cur.y) };
        for (const MatTexCell& cell : m_matTexCells)
        {
            if (cp.x < cell.rect.left || cp.x > cell.rect.right ||
                cp.y < cell.rect.top  || cp.y > cell.rect.bottom)
                continue;
            DrawMaterialTooltip(target, cell, cp);
            break;
        }
    }
}

// Small floating tooltip anchored just below the hovered texture cell,
// showing the full relative path and (if resolved) the loose/bsa source.
void NifCompareView::DrawMaterialTooltip(ID2D1RenderTarget* target,
                                         const MatTexCell& cell,
                                         const D2D1_POINT_2F& cursor)
{
    if (!m_matPanelText)
        return;

    std::vector<std::wstring> lines;
    lines.push_back(cell.fullPath);
    if (!cell.resolved.empty())
        lines.push_back(L"→  " + cell.resolved);
    lines.push_back(L"Right-click: copy path");

    // Measure the widest line to size the box.
    float textW = 0.0f;
    for (const std::wstring& s : lines)
    {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(FD2D::Core::DWriteFactory()->CreateTextLayout(
                s.c_str(), static_cast<UINT32>(s.size()), m_matPanelText.Get(),
                4000.0f, 40.0f, &layout)))
        {
            DWRITE_TEXT_METRICS tm {};
            layout->GetMetrics(&tm);
            textW = (std::max)(textW, tm.width);
        }
    }

    constexpr float kPad = 7.0f;
    constexpr float kLineH = 17.0f;
    const float boxW = textW + kPad * 2.0f;
    const float boxH = kLineH * lines.size() + kPad * 2.0f;

    const D2D1_RECT_F view = LayoutRect();
    float bx = cursor.x + 14.0f;
    float by = cell.rect.bottom + 4.0f;
    if (bx + boxW > view.right - 6.0f) bx = view.right - 6.0f - boxW;
    if (by + boxH > view.bottom - 6.0f) by = cell.rect.top - 4.0f - boxH;
    if (bx < view.left + 6.0f) bx = view.left + 6.0f;
    if (by < view.top + 6.0f)  by = view.top + 6.0f;
    const D2D1_RECT_F box { bx, by, bx + boxW, by + boxH };

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.11f, 0.13f, 0.97f), &brush)))
        return;
    const D2D1_ROUNDED_RECT rr { box, 4.0f, 4.0f };
    target->FillRoundedRectangle(rr, brush.Get());
    brush->SetColor(D2D1::ColorF(1.0f, 0.62f, 0.25f, 0.55f));
    target->DrawRoundedRectangle(rr, brush.Get(), 1.0f);

    float ty = box.top + kPad;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const bool hint = (i + 1 == lines.size());
        const bool resolved = (i == 1 && !cell.resolved.empty());
        brush->SetColor(hint ? D2D1::ColorF(0.55f, 0.58f, 0.62f)
                      : resolved ? D2D1::ColorF(0.62f, 0.80f, 0.98f)
                                 : D2D1::ColorF(0.92f, 0.92f, 0.94f));
        const D2D1_RECT_F rc { box.left + kPad, ty, box.right - kPad, ty + kLineH };
        target->DrawTextW(lines[i].c_str(), static_cast<UINT32>(lines[i].size()),
                          m_matPanelText.Get(), rc, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ty += kLineH;
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
    NifComparePane* active = AsNif(ActivePane());
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

bool NifCompareView::HandleMaterialPanelMouseDown(const POINT& pt)
{
    if (!m_matPanelLive)
        return false;

    // Bottom-left grip: begin a column-width drag (captured until mouse-up).
    if (!m_matPanelCollapsed && FD2D::Util::RectContainsPoint(m_matResizeGrip, pt))
    {
        m_matResizing = true;
        m_matResizeStartX = pt.x;
        m_matColWStart = m_matColW;
        if (FD2D::Backplate* bp = BackplateRef())
            if (bp->Window())
                SetCapture(bp->Window());
        return true;
    }
    // Header row: toggle collapse/expand.
    if (FD2D::Util::RectContainsPoint(m_matHeaderRect, pt))
    {
        m_matPanelCollapsed = !m_matPanelCollapsed;
        Invalidate();
        return true;
    }
    // Any other press on the panel is swallowed so the pane behind it doesn't
    // become active (which, with no selection, would hide the panel) or orbit.
    return FD2D::Util::RectContainsPoint(m_matPanelRect, pt);
}

void NifCompareView::UpdateMaterialHover(const POINT& pt)
{
    int hover = -1;
    if (m_matPanelLive)
    {
        for (std::size_t i = 0; i < m_matTexCells.size(); ++i)
            if (FD2D::Util::RectContainsPoint(m_matTexCells[i].rect, pt))
            {
                hover = static_cast<int>(i);
                break;
            }
    }
    if (hover != m_matHoverCell)
    {
        m_matHoverCell = hover;
        Invalidate(); // the overlay reads the cursor at draw time to place the tooltip
    }
}

bool NifCompareView::HandleMaterialPanelCopy(const POINT& pt)
{
    if (!m_matPanelLive)
        return false;
    for (const MatTexCell& cell : m_matTexCells)
    {
        if (!FD2D::Util::RectContainsPoint(cell.rect, pt))
            continue;
        if (FD2D::Backplate* bp = BackplateRef())
            if (bp->CopyTextToClipboard(cell.fullPath))
                bp->ShowToast(L"Texture path copied to clipboard");
        return true; // consumed even if the copy failed
    }
    return false;
}

namespace
{
    // Lowercased file name of a path (empty when the path is empty), for the
    // same-name "synced" comparison across panes.
    std::wstring LowerBaseName(const std::wstring& path)
    {
        if (path.empty())
            return {};
        std::wstring name = std::filesystem::path(path).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return name;
    }

    // An inner-glow frame: `color` (at `edgeAlpha`) hugging the rect's edge and
    // fading to transparent `thickness` px inward, on all four sides. Reads far
    // better than a 1px line without covering the content.
    void DrawInnerGlowBorder(ID2D1RenderTarget* target, const D2D1_RECT_F& r,
                             D2D1_COLOR_F color, float edgeAlpha, float thickness)
    {
        const float t = (std::min)(thickness,
                                   (std::min)((r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f));
        if (t <= 0.0f)
            return;
        color.a = edgeAlpha;
        D2D1_COLOR_F clear = color;
        clear.a = 0.0f;

        auto strip = [&](const D2D1_RECT_F& rect, D2D1_POINT_2F edge, D2D1_POINT_2F in)
        {
            const D2D1_GRADIENT_STOP stops[2] = { { 0.0f, color }, { 1.0f, clear } };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target->CreateGradientStopCollection(stops, 2, &coll)))
                return;
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
            if (FAILED(target->CreateLinearGradientBrush({ edge, in }, coll.Get(), &brush)))
                return;
            target->FillRectangle(rect, brush.Get());
        };

        strip({ r.left, r.top, r.right, r.top + t }, { 0, r.top }, { 0, r.top + t });          // top
        strip({ r.left, r.bottom - t, r.right, r.bottom }, { 0, r.bottom }, { 0, r.bottom - t }); // bottom
        strip({ r.left, r.top, r.left + t, r.bottom }, { r.left, 0 }, { r.left + t, 0 });        // left
        strip({ r.right - t, r.top, r.right, r.bottom }, { r.right, 0 }, { r.right - t, 0 });    // right
    }
}

void NifCompareView::DrawSyncBadges(ID2D1RenderTarget* target)
{
    if (target == nullptr || m_panes.size() < 2)
        return; // the synced/unique distinction only means something across panes

    // Lowercased file name per pane (empty when the pane holds no file). Two
    // panes are "synced" when they show the same file NAME (the compare-across-
    // mods workflow - Sync Files loads a picked name into every pane's folder).
    std::vector<std::wstring> names;
    names.reserve(m_panes.size());
    for (const auto& p : m_panes)
        names.push_back(p ? LowerBaseName(p->CurrentPath()) : std::wstring());

    if (!m_syncBadgeText)
    {
        FD2D::Core::DWriteFactory()->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.0f, L"", &m_syncBadgeText);
        if (m_syncBadgeText)
        {
            m_syncBadgeText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_syncBadgeText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    if (!m_syncBadgeText)
        return;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    for (std::size_t i = 0; i < m_panes.size(); ++i)
    {
        if (!m_panes[i] || names[i].empty())
            continue;
        bool synced = false;
        for (std::size_t j = 0; j < names.size(); ++j)
            if (j != i && names[j] == names[i]) { synced = true; break; }

        const D2D1_RECT_F r = m_panes[i]->LayoutRect();
        constexpr float bw = 64.0f, bh = 18.0f, pad = 8.0f;
        const float top = r.top + 26.0f; // just below the path-label strip
        const D2D1_RECT_F badge { r.right - pad - bw, top, r.right - pad, top + bh };
        const D2D1_ROUNDED_RECT rr { badge, 4.0f, 4.0f };

        // Synced: muted teal (the expected, aligned state). Unique: bright amber
        // so the odd-one-out pane stands out.
        const D2D1_COLOR_F fill = synced ? D2D1::ColorF(0.11f, 0.34f, 0.31f, 0.88f)
                                         : D2D1::ColorF(0.44f, 0.30f, 0.07f, 0.90f);
        const D2D1_COLOR_F edge = synced ? D2D1::ColorF(0.35f, 0.82f, 0.72f, 0.95f)
                                         : D2D1::ColorF(0.98f, 0.72f, 0.28f, 0.98f);
        if (SUCCEEDED(target->CreateSolidColorBrush(fill, &brush)))
            target->FillRoundedRectangle(rr, brush.Get());
        if (SUCCEEDED(target->CreateSolidColorBrush(edge, &brush)))
            target->DrawRoundedRectangle(rr, brush.Get(), 1.0f);
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.97f, 1.0f), &brush)))
        {
            const wchar_t* label = synced ? L"SYNCED" : L"UNIQUE";
            target->DrawTextW(label, static_cast<UINT32>(std::wcslen(label)),
                              m_syncBadgeText.Get(), badge, brush.Get());
        }
    }
}

void NifCompareView::OnRenderOverlay(ID2D1RenderTarget* target)
{
    FD2D::SplitPanel::OnRenderOverlay(target);
    if (target == nullptr)
        return;

    DrawMaterialDiffPanel(target);
    DrawTextureInspector(target);
    DrawSyncBadges(target);

    // Collapse/expand tab for the bottom control strip: a small centered
    // "drawer handle" sitting on the strip's top edge. Chevron points down
    // (collapse) while expanded, up (expand) while collapsed. The rect is
    // remembered for OnInputEvent's hit-test.
    if (m_controlsScroll)
    {
        const D2D1_RECT_F strip = m_controlsScroll->LayoutRect();
        constexpr float kTabW = 64.0f, kTabH = 15.0f;
        const float cx = (strip.left + strip.right) * 0.5f;
        m_collapseTabRect = D2D1::RectF(cx - kTabW * 0.5f, strip.top - kTabH,
                                        cx + kTabW * 0.5f, strip.top);

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.17f, 0.20f, 0.95f), &brush)))
        {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(m_collapseTabRect, 5.0f, 5.0f);
            target->FillRoundedRectangle(rr, brush.Get());
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
            target->DrawRoundedRectangle(rr, brush.Get(), 1.0f);

            // Chevron glyph (two strokes), pointing down when expanded.
            brush->SetColor(D2D1::ColorF(0.75f, 0.79f, 0.86f, 1.0f));
            const float cy = (m_collapseTabRect.top + m_collapseTabRect.bottom) * 0.5f;
            const float dir = m_controlsCollapsed ? -1.0f : 1.0f; // tip offset from center
            const D2D1_POINT_2F tip { cx, cy + 2.5f * dir };
            const D2D1_POINT_2F l { cx - 7.0f, cy - 2.5f * dir };
            const D2D1_POINT_2F r { cx + 7.0f, cy - 2.5f * dir };
            target->DrawLine(l, tip, brush.Get(), 2.0f);
            target->DrawLine(r, tip, brush.Get(), 2.0f);
        }
    }

    // Active-pane accent border (FICture2's focused-browser highlight) + its
    // sync group. Only meaningful while several panes compete for the
    // pane-context hotkeys; a single pane is trivially the active one.
    if (m_panes.size() > 1)
    {
        if (ComparePane* active = ActivePane())
        {
            // The panes showing the SAME file name as the active pane get a
            // green border, so the group being compared against the focused
            // pane stands out. Drawn on the chrome only - the 3D backgrounds
            // stay identical so the model comparison isn't biased.
            const std::wstring activeName = LowerBaseName(active->CurrentPath());
            if (!activeName.empty())
            {
                for (const auto& p : m_panes)
                {
                    if (!p || p.get() == active || LowerBaseName(p->CurrentPath()) != activeName)
                        continue;
                    DrawInnerGlowBorder(target, p->LayoutRect(),
                                        D2D1::ColorF(0.35f, 0.82f, 0.48f), 0.55f, 10.0f); // green group
                }
            }

            // The active pane's own inner glow, a touch stronger, on top.
            DrawInnerGlowBorder(target, active->LayoutRect(),
                                D2D1::ColorF(0.30f, 0.60f, 0.98f), 0.65f, 10.0f); // blue accent
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
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    // Type-to-select: while the active pane's thumbnail strip holds keyboard
    // focus (clicked into, or browsed with the arrow keys), a plain printable
    // key jumps the strip's selection to the next name-matching tile instead of
    // firing a single-letter display shortcut (G/X/W/...). Focus falls back to
    // the 3D view on the next viewport click, restoring the shortcuts.
    if (!ctrl && !alt)
    {
        if (ComparePane* active = ActivePane(); active && active->ThumbnailStripHasFocus())
        {
            wchar_t ch = 0;
            if (TryGetPrintableChar(event, ch))
            {
                ApplyThumbnailPick(active, active->TypeToSelectThumbnail(ch));
                return true; // consume: browsing the strip suspends letter shortcuts
            }
        }
    }

    // Image-pane shortcuts (active pane is an ImagePane): channel isolation
    // (R/G/B/A as grayscale, N back to RGBA), K alpha checkerboard, [ / ]
    // rotate, F / Home fit. Claimed before the NIF switch so the letter keys
    // mean channels here instead of NIF display toggles.
    if (!ctrl && !alt)
    {
        if (ImagePane* img = AsImage(ActivePane()))
        {
            switch (event.keyCode)
            {
            case 'R': img->SetChannelMode(1); return true;
            case 'G': img->SetChannelMode(2); return true;
            case 'B': img->SetChannelMode(3); return true;
            case 'A': img->SetChannelMode(4); return true;
            case 'N': img->SetChannelMode(0); return true;
            case 'K': img->ToggleAlphaCheckerboard(); return true;
            case VK_OEM_4: img->RotateCCW(); return true; // '['
            case VK_OEM_6: img->RotateCW(); return true;  // ']'
            case 'F': case VK_HOME: img->ResetView(); return true;
            default: break;
            }
        }
    }

    switch (event.keyCode)
    {
    case 'F': // Reset View, every pane (same as the PANES button)
        ForEachViewport([](NifViewport& vp) { vp.ResetCamera(); });
        return true;

    case 'R': // reset only the active pane's camera
        if (NifComparePane* active = AsNif(ActivePane()))
            active->Viewport().ResetCamera();
        return true;

    case 'C': // focus the active pane's selected sub-mesh (whole scene when none)
        if (NifComparePane* active = AsNif(ActivePane()))
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
        if (ComparePane* active = ActivePane())
            RequestClosePane(*active);
        return true;

    case 'E':
        if (!ctrl)
            return false;
        // Ctrl+E: show the active pane's file in Explorer (same behavior
        // as the context menu's "Open Containing Folder" - explorer's
        // /select verb needs no COM apartment).
        if (ComparePane* active = ActivePane())
        {
            const std::wstring path = active->CurrentPath();
            if (!path.empty())
            {
                const std::wstring args = L"/select,\"" + path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        return true;

    case VK_DELETE: // clear the active pane's document/image, keep the pane
        if (ComparePane* active = ActivePane())
            active->Clear();
        return true;

    case VK_TAB:
    {
        // Cycle the active pane (Shift+Tab goes backwards).
        if (m_panes.empty())
            return true;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::size_t index = 0;
        ComparePane* active = ActivePane();
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

    // Blender-style numpad view presets (NumLock on; the numeric VKs are
    // distinct from the nav-cluster ones, so they don't collide with the
    // thumbnail Home/End or PgUp/PgDn cycling). Ctrl picks the opposite face.
    case VK_NUMPAD1: m_controls->SetOrientation(ctrl ? 1 : 0); return true; // Front / Back
    case VK_NUMPAD3: m_controls->SetOrientation(ctrl ? 2 : 3); return true; // Right / Left
    case VK_NUMPAD7: m_controls->SetOrientation(ctrl ? 5 : 4); return true; // Top / Bottom
    case VK_NUMPAD5: ToggleProjection(); return true;                   // ortho <-> perspective
    case VK_DECIMAL: // Numpad . : frame the active pane's selection (whole scene when none)
        if (NifComparePane* active = AsNif(ActivePane()))
            active->Viewport().FocusOnSelection();
        return true;
    case VK_NUMPAD0: // frame the whole scene, keeping orientation (View All)
        if (NifComparePane* active = AsNif(ActivePane()))
            active->Viewport().FrameScene();
        return true;

    // Thumbnail navigation on the active pane's strip (FICture2's browser
    // keys); a file load syncs into the other panes via Sync Files. The
    // selection cursor spans files AND folder/".." tiles - stepping onto a file
    // loads it, stepping onto a folder only selects it, and Enter enters the
    // selected folder.
    //   Left / ',' : previous tile       Right / '.' : next tile
    //   Home : first tile                End : last tile
    //   Enter : enter selected folder    Backspace / Ctrl+Up : parent folder
    case VK_LEFT:       StepActiveThumbnail(-1); return true;
    case VK_RIGHT:      StepActiveThumbnail(+1); return true;
    case VK_OEM_COMMA:  StepActiveThumbnail(-1); return true; // ',' / '<'
    case VK_OEM_PERIOD: StepActiveThumbnail(+1); return true; // '.' / '>'
    case VK_HOME:       LoadEdgeThumbnail(false); return true;
    case VK_END:        LoadEdgeThumbnail(true);  return true;

    case VK_RETURN: // Enter: enter the selected folder / ".." tile (arrows load
                    // files immediately; folders are only selected until Enter)
    {
        if (ComparePane* active = ActivePane())
        {
            active->FocusThumbnailStrip();
            if (active->ActivateThumbnailSelection())
                return true;
        }
        return false;
    }

    case VK_BACK: // Backspace: browse the active pane's strip to the parent
        if (ComparePane* active = ActivePane())
        {
            active->FocusThumbnailStrip();
            active->NavigateThumbnailUp();
        }
        return true;

    case VK_UP: // Ctrl+Up: same as Backspace (browse to the parent folder)
        if (ctrl)
        {
            if (ComparePane* active = ActivePane())
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
            ComparePane* target = AddPane(); // nullptr at kMaxPanes
            if (target == nullptr)
                target = ActivePane();       // fall back: reuse the active pane
            if (target != nullptr)
            {
                SetActivePane(target);
                RequestOpenPane(*target);
            }
        }
        else if (ComparePane* target = ActivePane())
        {
            RequestOpenPane(*target);
        }
        return true;

    case VK_F4: // Ctrl+F4: close the active pane (FICture2's Close; == Ctrl+W)
        if (ctrl)
        {
            if (ComparePane* active = ActivePane())
                RequestClosePane(*active);
            return true;
        }
        return false;

    case VK_F12:
        if (NifComparePane* target = AsNif(ActivePane()))
        {
            if (m_onScreenshotRequested)
                m_onScreenshotRequested(*target);
        }
        return true;

    default:
        return false;
    }
}

void NifCompareView::SetOnContextMenuRequested(std::function<void(POINT, ComparePane*)> handler)
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
    // Initial guess only; Arrange() recomputes the strip height for the real
    // width each layout (the strip reflows, so its height depends on the width).
    const float contentHeight = m_controls->Measure({ 1600.0f, 10000.0f }).h;
    SetSecondPaneMinExtent((std::max)(80.0f, contentHeight));
    SetSecondPaneMaxExtent((std::max)(120.0f, contentHeight + 16.0f));
}

void NifCompareView::Arrange(FD2D::Rect finalRect)
{
    // Reflow the control strip to the current width and size its (bottom) pane to
    // the resulting wrapped height, so a narrow window grows the strip into more
    // rows rather than clipping. Capped so the 3D views keep the bulk of the
    // window; past the cap the strip's ScrollView shows a vertical scrollbar.
    if (m_controls)
    {
        if (m_controlsCollapsed)
        {
            // Collapsed: keep only a sliver for the chevron tab to sit on -
            // the 3D views get (almost) the full window height.
            constexpr float kCollapsedExtent = 6.0f;
            SetSecondPaneMinExtent(kCollapsedExtent);
            SetSecondPaneMaxExtent(kCollapsedExtent);
        }
        else
        {
            // Narrow window: collapse the multi-column groups to single columns
            // (decided from the overall width so it stays consistent across the
            // strip's Measure and Arrange), then size the strip to the result.
            m_controls->SetCompact(finalRect.w < 900.0f);
            const float availW = (std::max)(120.0f, finalRect.w - 24.0f);
            const float stripH = m_controls->Measure({ availW, 1.0e9f }).h;
            const float cap = (std::max)(140.0f, finalRect.h * 0.55f);
            const float ext = (std::min)(stripH + 10.0f, cap);
            SetSecondPaneMinExtent(ext);
            SetSecondPaneMaxExtent(ext);
        }
    }
    FD2D::SplitPanel::Arrange(finalRect);
}

void NifCompareView::SetControlStripCollapsed(bool collapsed, bool notify)
{
    if (m_controlsCollapsed == collapsed)
        return;
    m_controlsCollapsed = collapsed;
    if (notify && m_onControlsCollapsedChanged)
        m_onControlsCollapsedChanged(collapsed);
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void NifCompareView::ApplyOrientationPreset(int index)
{
    // Under Sync Views every pane shares one camera, so animating the active
    // pane and letting the sync mirror carry the tween keeps them in lockstep;
    // otherwise animate each pane so each keeps its own framing. (Unlike
    // liteviewer's Left-primary scheme, a preset has no natural primary once
    // there can be up to kMaxPanes - it targets every open pane.)
    if (m_syncViews)
    {
        if (NifComparePane* active = AsNif(ActivePane()))
            active->Viewport().AnimateToPreset(index);
    }
    else
    {
        ForEachViewport([index](NifViewport& vp) { vp.AnimateToPreset(index); });
    }
}

void NifCompareView::ToggleProjection()
{
    ApplyOrthographic(!m_orthographic);
}

void NifCompareView::ApplyOrthographic(bool on)
{
    m_orthographic = on;
    ForEachViewport([on](NifViewport& vp) { vp.SetOrthographic(on); });
    // Reflect the state in the NAVIGATION checkbox (notify=false: this IS the
    // handler's side, and Numpad 5 must not re-enter the checkbox callback).
    m_controls->SetOrthographicChecked(on, /*notify=*/false);
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

template <typename Fn>
void NifCompareView::ForEachAnimTarget(Fn&& fn)
{
    if (m_syncAnimation)
    {
        for (auto& p : m_panes)
            if (auto* n = AsNif(p))
                fn(n->Viewport());
    }
    else if (NifComparePane* active = AsNif(ActivePane()))
    {
        fn(active->Viewport());
    }
}

void NifCompareView::ToggleAnimPlayback()
{
    NifComparePane* active = AsNif(ActivePane());
    if (!active || !active->Viewport().HasAnimations())
        return;
    const bool play = !active->Viewport().AnimPlaying();
    ForEachAnimTarget([play](NifViewport& v) { v.SetAnimPlaying(play); });
    if (play)
        EnsureCameraAnimTimer(); // playback rides the shared ~60fps stepper
    m_controls->SetAnimPlayingDisplay(active->Viewport().AnimPlaying());
}

void NifCompareView::RefreshAnimationControls()
{
    NifComparePane* active = AsNif(ActivePane());
    NifViewport* v = active ? &active->Viewport() : nullptr;
    const bool has = (v != nullptr) && v->HasAnimations();
    m_controls->SetAnimEnabled(has);

    std::vector<std::wstring> names;
    int selected = 0;
    if (has)
    {
        for (std::size_t i = 0; i < v->AnimSequenceCount(); ++i)
            names.push_back(Utf8ToWideStr(v->AnimSequenceName(i)));
        if (names.empty())
            names.push_back(L"(always)"); // standalone controllers only
        selected = (std::max)(0, v->AnimSelectedSequence());
        m_controls->SetAnimTimeRange(v->AnimTimeMin(), v->AnimTimeMax());
        m_controls->SetAnimTimeValue(v->AnimTime());
        m_controls->SetAnimPlayingDisplay(v->AnimPlaying());
    }
    m_controls->SetAnimSequences(names, selected);
}

void NifCompareView::EnsureCameraAnimTimer()
{
    if (m_cameraAnimTimerRunning)
        return;
    FD2D::Backplate* bp = BackplateRef();
    if (!bp || !bp->Window())
        return;
    m_cameraAnimTimerRunning = true;
    // nIDEvent = this+1 keeps it distinct from the pending-close timer (this).
    ::SetTimer(bp->Window(), reinterpret_cast<UINT_PTR>(this) + 1, 16, &NifCompareView::CameraAnimThunk);
}

void NifCompareView::CameraAnimThunk(HWND /*hwnd*/, UINT /*msg*/, UINT_PTR idEvent, DWORD /*dwTime*/)
{
    reinterpret_cast<NifCompareView*>(idEvent - 1)->TickCameraAnimations();
}

void NifCompareView::TickCameraAnimations()
{
    // Shared ~60fps stepper: camera tweens AND mesh-animation playback ride the
    // same on-demand timer; it stops the moment neither has work left.
    const unsigned long long now = GetTickCount64();
    bool any = false;
    ForEachViewport([&any, now](NifViewport& vp)
    {
        if (vp.TickCameraAnimation(now)) any = true;
        if (vp.TickAnimation(now)) any = true;
    });
    // Follow the active pane's clock on the Time slider while playing (also
    // flips the Play button back when a one-shot clip reaches its end).
    if (NifComparePane* active = AsNif(ActivePane()); active && active->Viewport().HasAnimations())
    {
        m_controls->SetAnimTimeValue(active->Viewport().AnimTime());
        m_controls->SetAnimPlayingDisplay(active->Viewport().AnimPlaying());
    }
    if (any)
        return;
    // All tweens finished: stop the timer until the next animation is requested.
    if (FD2D::Backplate* bp = BackplateRef(); bp && bp->Window())
        ::KillTimer(bp->Window(), reinterpret_cast<UINT_PTR>(this) + 1);
    m_cameraAnimTimerRunning = false;
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
            [&](const std::shared_ptr<ComparePane>& p) { return p->Name() == paneName; });
    }
    m_pendingCloseNames.clear();

    RebuildHostTree();
}

void NifCompareView::SetOnPaneOpenRequested(std::function<void(ComparePane&)> handler)
{
    m_onPaneOpenRequested = std::move(handler);
}

void NifCompareView::SetResourceResolver(ResourceResolver* resolver)
{
    m_resolver = resolver;
    for (auto& p : m_panes) p->SetResourceResolver(resolver); // base wires the strip; NIF also the viewport
}

void NifCompareView::SetTextureRepository(TextureRepository* repository)
{
    m_textureRepository = repository;
    for (auto& p : m_panes) p->SetTextureRepository(repository);
}

void NifCompareView::SetRenderDevice(RenderDevice* device)
{
    m_renderDevice = device;
    for (auto& p : m_panes) p->SetRenderDevice(device);
}

void NifCompareView::SetResourceManager(ResourceManager* manager)
{
    m_resourceManager = manager;
    for (auto& p : m_panes) p->SetResourceManager(manager);
}

void NifCompareView::OnRenderD3D(ID3D11DeviceContext* context)
{
    // Apply completed async loads before the strips render this frame.
    if (m_resourceManager)
        m_resourceManager->DrainCompletions();

    // Start the shader hot-reload poll timer once a window exists (rendering
    // is on-demand, so the poll cannot live in this frame callback - an idle
    // window would stop noticing edits). Three file stats a second is noise.
    EnsureShaderReloadTimer();

    FD2D::SplitPanel::OnRenderD3D(context); // propagate to panes/strips
}

void NifCompareView::EnsureShaderReloadTimer()
{
    if (m_shaderReloadTimerRunning || m_renderDevice == nullptr)
        return;
    FD2D::Backplate* bp = BackplateRef();
    if (!bp || !bp->Window())
        return;
    m_shaderReloadTimerRunning = true;
    // nIDEvent = this+2 (this = pending-close, this+1 = camera/anim stepper).
    ::SetTimer(bp->Window(), reinterpret_cast<UINT_PTR>(this) + 2, 1000, &NifCompareView::ShaderReloadThunk);
}

void CALLBACK NifCompareView::ShaderReloadThunk(HWND, UINT, UINT_PTR idEvent, DWORD)
{
    reinterpret_cast<NifCompareView*>(idEvent - 2)->PollShaderHotReload();
}

void NifCompareView::PollShaderHotReload()
{
    // Saving an edit to shaders\*.hlsl shows up without restarting; a broken
    // edit falls back to the embedded shaders and surfaces a toast.
    if (m_renderDevice == nullptr || !m_renderDevice->ReloadShadersIfChanged())
        return;
    if (FD2D::Backplate* bp = BackplateRef())
        bp->ShowToast(m_renderDevice->ShaderOverrideStatus().empty()
            ? L"Shaders reloaded"
            : L"Shader compile error - embedded fallback active (see log)");
    Invalidate();
}

void NifCompareView::InvalidateTextureCaches()
{
    // Resolution inputs changed (game data / override folders): the pooled
    // SRVs may now be stale, so drop the shared pool together with every
    // pane's resolution memo (memoized Entry pointers die with the pool).
    if (m_textureRepository != nullptr)
        m_textureRepository->Clear();
    ForEachNifPane([](NifComparePane& n) { n.InvalidateTextureCache(); });
}

void NifCompareView::SetOnBrowseGameData(std::function<void()> handler) { m_controls->SetOnBrowseGameData(std::move(handler)); }
void NifCompareView::SetOnDetectGameData(std::function<void()> handler) { m_controls->SetOnDetectGameData(std::move(handler)); }
void NifCompareView::SetOnAddOverrideFolder(std::function<void()> handler) { m_controls->SetOnAddOverrideFolder(std::move(handler)); }
void NifCompareView::SetOnClearOverrides(std::function<void()> handler) { m_controls->SetOnClearOverrides(std::move(handler)); }
void NifCompareView::SetGameDataLabel(const std::wstring& text) { m_controls->SetGameDataLabel(text); }
void NifCompareView::SetOverrideCountLabel(std::size_t count) { m_controls->SetOverrideCountLabel(count); }

} // namespace nsk
