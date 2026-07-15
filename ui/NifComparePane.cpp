#include "NifComparePane.h"

#include "../core/ResourceManager.h" // GetOrParseNif (shared NifCache)
#include "../core/SceneBuilder.h"    // build the scene on the load pool
#include "../core/StartupTrace.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{

namespace
{
    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty())
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
        return w;
    }

    std::wstring FormatCount(std::size_t n)
    {
        std::wstring s = std::to_wstring(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
            s.insert(static_cast<std::size_t>(i), L",");
        return s;
    }
}

NifComparePane::NifComparePane(const std::wstring& name)
    : FD2D::DockPanel(name)
{
    m_viewport = std::make_shared<NifViewport>(name + L"_Viewport");

    m_pathLabel = std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true); // long paths trim from the right within the pane width
    m_pathLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));
    // Reveal the full path on hover when the strip is too narrow to show it,
    // and copy just the path (not the selection suffix) on right-click.
    m_pathLabel->SetTooltipOnTruncation(true);
    m_pathLabel->SetCopyTextOnRightClick(true);

    // Bottom-right stats readout: total triangle count of the loaded scene,
    // plus the picked sub-mesh's count while one is selected. Full-width
    // bottom strip with trailing alignment = visually bottom-right.
    m_statsLabel = std::make_shared<FD2D::Text>(name + L"_Stats");
    m_statsLabel->SetFont(L"Segoe UI", 13.0f);
    m_statsLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));
    m_statsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    // This pane's own thumbnail strip (one per pane, not shared): follows THIS
    // pane's open .nif folder. Clicking a sibling loads it into this pane.
    m_thumbStrip = std::make_shared<ThumbnailStrip>(name + L"_Thumbs");
    m_thumbStrip->SetOrientation(ThumbnailStrip::Orientation::Horizontal);
    m_thumbStrip->SetOnActivated([this](const std::wstring& path)
    {
        std::string err;
        Load(path, &err);
        Invalidate();
        // Let the owner mirror this pick into the other panes' folders.
        if (m_onThumbnailChosen)
            m_onThumbnailChosen(path);
    });
    m_thumbStrip->SetOnResize([this](float ext, bool committed)
    {
        if (m_onThumbStripResize)
            m_onThumbStripResize(ext, committed);
    });

    // DockPanel's Fill dock stops any further docking, so the Top-docked path
    // strip and the Bottom-docked strips must be added before the Fill-docked
    // viewport. Bottom order = outermost first: the thumbnail strip sits at the
    // very bottom edge, the stats readout just above it (see DockPanel::Arrange
    // / NifViewport.h's comment on the same pattern).
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_thumbStrip);
    SetChildDock(m_thumbStrip, FD2D::Dock::Bottom);
    AddChild(m_statsLabel);
    SetChildDock(m_statsLabel, FD2D::Dock::Bottom);
    AddChild(m_viewport);
    SetChildDock(m_viewport, FD2D::Dock::Fill);

    // Click-to-select feedback: show the picked sub-mesh's name next to the
    // file path (NIFDiff has no block-tree sidebar for the selection to
    // land in, so the path strip doubles as the selection readout) and its
    // triangle count in the stats strip.
    m_viewport->SetOnSelectionChanged([this](const RenderMesh* sel)
    {
        m_selectedName = sel ? Utf8ToWide(sel->nodeName) : std::wstring();
        m_selectedKind = sel ? m_viewport->ShaderKindFor(*sel) : std::wstring();
        UpdatePathLabel();
        UpdateStatsLabel();
    });

    UpdatePathLabel();
    UpdateStatsLabel();
}

void NifComparePane::UpdatePathLabel()
{
    // While loading, show the pending path with a marker so the pane reads as
    // "this file, coming" rather than empty; a failed load says so.
    const std::wstring path = CurrentPath();
    std::wstring text;
    if (m_state == LoadState::Loading)
        text = L"Loading  " + path;
    else if (m_state == LoadState::Failed)
        text = L"Failed to load  " + path;
    else
        text = m_doc ? path : L"(no file)";
    if (!m_selectedName.empty())
    {
        text += L"    ▸ " + m_selectedName;
        if (!m_selectedKind.empty())
            text += L"  [" + m_selectedKind + L"]";
    }
    m_pathLabel->SetText(text);
    // Right-click copies just the .nif path, without the selection suffix.
    m_pathLabel->SetCopyText(path);
    m_pathLabel->Invalidate();
}

void NifComparePane::UpdateStatsLabel()
{
    std::wstring text;
    if (m_doc)
    {
        // File-wide shader kinds (see NifViewport::ShaderKindSummary), then
        // the selection/total triangle counts.
        const std::wstring shaders = m_viewport->ShaderKindSummary();
        if (!shaders.empty())
            text = L"Shaders: " + shaders + L"    |    ";
        if (const RenderMesh* sel = m_viewport->SelectedMesh(); sel && sel->geometry)
            text += L"Sel: " + FormatCount(sel->geometry->triangles.size()) + L"  |  ";
        // Trailing NBSPs keep the right-aligned text off the pane edge (Text
        // ignores Wnd margins, and plain trailing spaces collapse in DWrite's
        // trailing alignment).
        text += L"Total: " + FormatCount(m_viewport->TotalTriangleCount()) + L" tris\u00A0\u00A0";
    }
    m_statsLabel->SetText(text);
    m_statsLabel->Invalidate();
}

bool NifComparePane::Load(const std::wstring& path, std::string* error)
{
    if (path.empty())
        return false;

    // No pool wired (tests / headless): synchronous parse+build so callers that
    // rely on the immediate result still work.
    if (!m_resourceManager)
    {
        auto fresh = std::make_shared<NifDocument>();
        if (!fresh->loadFromFile(path, error) || !fresh->isValid())
            return false;
        AcceptLoaded(path, fresh, SceneBuilder::build(*fresh, m_viewport->ShowHiddenNodes()));
        return true;
    }

    // Async: enter Loading immediately (the viewport shows its placeholder grid
    // and the path label reads "Loading"), then parse+build on the shared pool.
    m_pendingPath = path;
    m_state = LoadState::Loading;
    // Retarget (this pane already shows a model, e.g. picking a sibling from the
    // thumbnail strip): keep the current model on screen until the new one is
    // ready, so the main view doesn't flash the empty placeholder grid between
    // them. A fresh/empty pane shows the placeholder + "Loading" overlay.
    if (!m_doc)
    {
        m_viewport->SetDocument(nullptr);
        m_viewport->SetLoading(true);
    }
    m_thumbStrip->SetActive(true); // reserve the strip's space up front (stable layout)
    UpdatePathLabel();
    UpdateStatsLabel();
    SubmitParseJob(path);
    return true;
}

void NifComparePane::SubmitParseJob(const std::wstring& path)
{
    // Queue the parse+build on the shared pool (ActivePane priority). Bumping
    // our generation drops any previous in-flight load (retarget). Kept minimal
    // and free of UI churn so the startup batch (StartPendingLoad) can queue
    // every pane's main job before any completion - and thus any lower-priority
    // thumbnail - runs.
    m_loadGen = m_resourceManager->BumpGeneration(this);
    m_loadSubmitted = true;
    ResourceManager* const mgr = m_resourceManager;
    NifComparePane* const self = this;
    const std::uint64_t gen = m_loadGen;
    const bool includeHidden = m_viewport->ShowHiddenNodes();
    mgr->Submit(ResourceManager::Priority::ActivePane, { self, gen },
        [mgr, self, gen, path, includeHidden]()
        {
            // Pool thread: parse (shared cache, IoGate-gated) + build, both free
            // of shared state. `self` is only forwarded to the UI completion,
            // never dereferenced here.
            std::shared_ptr<const NifDocument> doc = mgr->GetOrParseNif(
                path, nullptr, ResourceManager::Priority::ActivePane, /*throttle=*/true);
            std::vector<RenderMesh> meshes;
            if (doc)
                meshes = SceneBuilder::build(*doc, includeHidden);
            mgr->PostCompletion({ self, gen },
                [self, path, doc, meshes = std::move(meshes)]() mutable
                { self->AcceptLoaded(path, std::move(doc), std::move(meshes)); });
        });
}

void NifComparePane::StartPendingLoad()
{
    // The pane is already named + placeholdered (ShowPendingFile); just queue
    // the parse job. No SetDocument/SetLoading/label work here, so a batch of
    // these can't trigger a synchronous render (which would run a completion
    // and submit thumbnails) before every main is queued.
    if (m_state != LoadState::Loading || m_pendingPath.empty() || m_loadSubmitted ||
        !m_resourceManager)
        return;
    SubmitParseJob(m_pendingPath);
}

void NifComparePane::AcceptLoaded(const std::wstring& path,
                                  std::shared_ptr<const NifDocument> doc,
                                  std::vector<RenderMesh> meshes)
{
    m_viewport->SetLoading(false); // load finished (success or failure)
    if (!doc)
    {
        // Parse or build failed: leave the placeholder, mark the pane Failed.
        m_state = LoadState::Failed;
        m_pendingPath = path;
        m_doc.reset();
        m_viewport->SetDocument(nullptr);
        UpdatePathLabel();
        UpdateStatsLabel();
        if (m_onDocumentChanged)
            m_onDocumentChanged();
        return;
    }
    m_doc = std::move(doc);
    m_state = LoadState::Ready;
    m_pendingPath.clear();
    // Hand the worker-built scene straight to the viewport (no re-parse/build).
    m_viewport->SetPrebuiltScene(m_doc.get(), std::move(meshes));
    UpdatePathLabel();
    UpdateStatsLabel();
    m_thumbStrip->ShowForFile(path); // list the loaded file's folder, highlighted
    if (m_onDocumentChanged)
        m_onDocumentChanged();
    if (m_onFileOpened)
        m_onFileOpened(path);
}

void NifComparePane::ShowPendingFile(const std::wstring& path)
{
    if (path.empty())
    {
        Clear();
        return;
    }
    // Named placeholder, no load yet: the pane appears with its file name (and
    // the viewport's placeholder grid) immediately; Load() follows once the
    // archive scan is ready. CurrentPath() returns this pending path, so pane
    // placement and the session save already treat the pane as occupied.
    m_pendingPath = path;
    m_state = LoadState::Loading;
    m_loadSubmitted = false; // named only; StartPendingLoad will queue the job
    m_viewport->SetDocument(nullptr);
    m_viewport->SetLoading(true);
    // Reserve the thumbnail strip's space now, before its folder is listed, so
    // it doesn't pop in later and shrink the viewport (which reframes the model).
    m_thumbStrip->SetActive(true);
    UpdatePathLabel();
    UpdateStatsLabel();
}

void NifComparePane::Clear()
{
    // Drop any in-flight load so its late completion doesn't repopulate us.
    if (m_resourceManager)
        m_loadGen = m_resourceManager->BumpGeneration(this);
    m_state = LoadState::Empty;
    m_pendingPath.clear();
    m_loadSubmitted = false;
    m_doc.reset();
    m_viewport->SetDocument(nullptr);
    m_viewport->SetLoading(false);
    m_thumbStrip->SetActive(false); // empty pane: release the strip's reserved space
    UpdatePathLabel();
    UpdateStatsLabel();
    m_thumbStrip->ShowForFile(std::wstring()); // nothing loaded -> empty strip
    if (m_onDocumentChanged)
        m_onDocumentChanged();
}

NifComparePane::~NifComparePane()
{
    // Cancel any in-flight load so its completion (which captures `this`) is
    // dropped by the manager instead of dereferencing a destroyed pane.
    if (m_resourceManager)
        m_resourceManager->Cancel(this);
}

void NifComparePane::SetResourceResolver(ResourceResolver* resolver)
{
    m_viewport->SetResourceResolver(resolver);
    m_thumbStrip->SetResourceResolver(resolver);
}

void NifComparePane::SetTextureRepository(TextureRepository* repository)
{
    m_viewport->SetTextureRepository(repository);
    m_thumbStrip->SetTextureRepository(repository);
}

void NifComparePane::SetRenderDevice(RenderDevice* device)
{
    m_viewport->SetRenderDevice(device);
    m_thumbStrip->SetRenderDevice(device);
}

void NifComparePane::SetThumbnailStripEnabled(bool enabled)
{
    m_thumbStrip->SetEnabled(enabled);
}

void NifComparePane::InvalidateTextureCache()
{
    m_viewport->InvalidateTextureCache();
}

} // namespace nsk
