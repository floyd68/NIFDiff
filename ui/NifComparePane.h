// NifComparePane.h - one compare slot in a dynamic 1-8 pane NifCompareView:
// a NifViewport plus a top path strip, generalizing the ad-hoc per-side
// leftDock/rightDock pattern liteviewer's NifCompareView constructor used to
// hand-duplicate for exactly two panes (see NIF_DIFF_VIEWER.md / README.md's
// FICture2-composition note) into a reusable class NifCompareView can
// instantiate up to kMaxPanes of. Opening/closing a pane lives in the
// right-click context menu (see NifCompareView::SetOnContextMenuRequested),
// not in per-pane buttons.
#pragma once

#include "NifViewport.h"
#include "ThumbnailStrip.h"
#include "../core/NifDocument.h"
#include "../core/ResourceResolver.h"

#include <DockPanel.h>
#include <Text.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class NifComparePane : public FD2D::DockPanel
{
public:
    explicit NifComparePane(const std::wstring& name);
    ~NifComparePane();

    // Load lifecycle: an async Load moves Empty/Ready -> Loading immediately
    // (viewport shows its placeholder grid) and settles on Ready or Failed when
    // the pool finishes the parse+build.
    enum class LoadState { Empty, Loading, Ready, Failed };

    NifViewport& Viewport() { return *m_viewport; }
    const NifDocument* Document() const { return m_doc.get(); }
    LoadState State() const { return m_state; }
    bool IsLoading() const { return m_state == LoadState::Loading; }
    // Full path of the .nif loaded (or loading) here, empty only when the pane
    // is truly free - the thumbnail strip follows this, and pane placement
    // treats a still-loading pane as occupied (its pending path is non-empty).
    std::wstring CurrentPath() const
    {
        if (m_state == LoadState::Loading || m_state == LoadState::Failed)
            return m_pendingPath;
        return m_doc ? m_doc->filePath() : std::wstring();
    }

    // Accepts the load (sets the pending path + Loading state, queues the
    // parse+build on the shared pool) and returns true; the model fills in when
    // the pool completes. Returns false only when the path can't be accepted
    // (empty). Without a resource manager wired it loads synchronously and
    // returns the real parse result (tests / headless).
    bool Load(const std::wstring& path, std::string* error = nullptr);
    void Clear();

    // Show a target file name immediately (Loading placeholder) WITHOUT
    // starting the load. Used at startup so every pane appears named before the
    // archive scan finishes; a later StartPendingLoad() does the actual load.
    void ShowPendingFile(const std::wstring& path);

    // Queue the parse job for a pane already named via ShowPendingFile, with no
    // UI churn - so the startup batch can queue every pane's main load before
    // any completion (and thus any lower-priority thumbnail) runs. No-op unless
    // the pane is in the Loading placeholder state.
    void StartPendingLoad();

    // Show this pane's thumbnail strip for its current/pending folder now, so
    // the strip is present at its proper height before the model finishes
    // loading (called after the startup batch queues every main, so its
    // lower-priority thumbnails still trail the mains in the shared queue).
    void ShowThumbnailFolder() { m_thumbStrip->ShowForFile(CurrentPath()); }

    // Re-resolve this pane's textures after the archive scan lands (no-op until
    // a model is loaded). Archive textures that missed during the scan pop in.
    void RefreshTextures() { if (m_doc) m_viewport->RefreshTextures(); }

    void SetResourceResolver(ResourceResolver* resolver);
    void SetTextureRepository(TextureRepository* repository);
    void SetRenderDevice(RenderDevice* device);
    void SetResourceManager(ResourceManager* manager)
    {
        m_resourceManager = manager;               // pane loads reuse the shared NifCache
        m_thumbStrip->SetResourceManager(manager); // strip parses feed the same cache
    }
    void InvalidateTextureCache();

    // This pane's own thumbnail strip (FICture2's ThumbnailPane, one per pane):
    // it lists the folder of THIS pane's open .nif. Master on/off is a global
    // UI toggle applied to every pane.
    void SetThumbnailStripEnabled(bool enabled);
    // Thumbnail-strip thickness (card size); a global UI setting, all panes
    // share it. Use ThumbnailStrip::kSizeSmall/Medium/Large.
    void SetThumbnailStripSize(float extent) { m_thumbStrip->SetFixedExtent(extent); }
    // Fired while the user drags THIS pane's strip grip (committed=false live,
    // true on release) - NifCompareView mirrors the size onto every pane.
    void SetOnThumbnailStripResize(std::function<void(float, bool)> handler) { m_onThumbStripResize = std::move(handler); }

    // Keyboard browsing over this pane's thumbnail strip (owner-driven). The
    // selection cursor spans files AND folders/"..": Step/Edge return the .nif
    // path to load when the newly-selected tile is a file (empty for a folder,
    // which is only highlighted), and ActivateThumbnailSelection (Enter) enters
    // a selected folder. NavigateThumbnailUp is a direct jump to the parent.
    std::wstring StepThumbnailFile(int delta) { return m_thumbStrip->StepSelection(delta); }
    std::wstring EdgeThumbnailFile(bool last) { return m_thumbStrip->EdgeSelection(last); }
    bool ActivateThumbnailSelection() { return m_thumbStrip->ActivateSelection(); }
    void NavigateThumbnailUp() { m_thumbStrip->NavigateUp(); }

    // Fires after Load/Clear changed this pane's document - NifCompareView
    // uses it to refresh document-dependent control state (e.g. whether the
    // Parallax Height slider applies to anything currently loaded).
    void SetOnDocumentChanged(std::function<void()> handler) { m_onDocumentChanged = std::move(handler); }

    // Fires only on a successful Load(), carrying the loaded path - the
    // single choke point every open path (file dialog, drag&drop, command
    // line, session restore, IPC) funnels through, so NifCompareView routes
    // it to the app's recent-files (MRU) list.
    void SetOnFileOpened(std::function<void(const std::wstring&)> handler) { m_onFileOpened = std::move(handler); }

    // Fires when the user picks a .nif from THIS pane's thumbnail strip (after
    // it loads here) - NifCompareView uses it to sync the same file name into
    // the other panes' folders. Distinct from SetOnFileOpened, which fires for
    // every load path (and would recurse if it drove the sync).
    void SetOnThumbnailChosen(std::function<void(const std::wstring&)> handler) { m_onThumbnailChosen = std::move(handler); }

private:
    void UpdatePathLabel();
    void UpdateStatsLabel();
    // Queue the ActivePane-priority parse+build job for `path` (no UI work).
    void SubmitParseJob(const std::wstring& path);
    // UI-thread completion of an async Load (already generation-checked by the
    // manager). `doc` null => the parse/build failed.
    void AcceptLoaded(const std::wstring& path, std::shared_ptr<const NifDocument> doc,
                      std::vector<RenderMesh> meshes);

    std::shared_ptr<NifViewport> m_viewport;
    std::shared_ptr<ThumbnailStrip> m_thumbStrip; // bottom strip: this pane's folder browser
    std::shared_ptr<FD2D::Text> m_pathLabel;  // top strip: full path of the loaded .nif + picked sub-mesh name
    std::shared_ptr<FD2D::Text> m_statsLabel; // bottom strip, right-aligned: total (and selected sub-mesh) triangle counts
    std::wstring m_selectedName;              // name of the viewport's picked sub-mesh, empty when none
    std::wstring m_selectedKind;              // shader kind of the picked sub-mesh (see NifViewport::ShaderKindFor)
    std::function<void()> m_onDocumentChanged;
    std::function<void(const std::wstring&)> m_onFileOpened;
    std::function<void(const std::wstring&)> m_onThumbnailChosen;
    std::function<void(float, bool)> m_onThumbStripResize;
    // Shared with the ResourceManager's NifCache and this pane's thumbnail of
    // its own file: the doc is parsed once and held here (pinning its entry).
    std::shared_ptr<const NifDocument> m_doc;
    ResourceManager* m_resourceManager = nullptr;

    // Async-load state. m_loadGen is this pane's ResourceManager generation;
    // Load bumps it so a superseded load (retarget) or a closed pane drops its
    // in-flight result. m_pendingPath is the file being loaded (shown as the
    // pane's path while Loading, and saved with the session if closed mid-load).
    LoadState m_state = LoadState::Empty;
    std::wstring m_pendingPath;
    std::uint64_t m_loadGen = 0;
    bool m_loadSubmitted = false; // parse job queued (guards StartPendingLoad re-calls)
};

} // namespace nsk
