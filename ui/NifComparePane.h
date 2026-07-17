// NifComparePane.h - one compare slot in a dynamic 1-8 pane NifCompareView:
// a NifViewport plus a top path strip, generalizing the ad-hoc per-side
// leftDock/rightDock pattern liteviewer's NifCompareView constructor used to
// hand-duplicate for exactly two panes (see NIF_DIFF_VIEWER.md / README.md's
// FICture2-composition note) into a reusable class NifCompareView can
// instantiate up to kMaxPanes of. Opening/closing a pane lives in the
// right-click context menu (see NifCompareView::SetOnContextMenuRequested),
// not in per-pane buttons.
#pragma once

#include "PaneContent.h"
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

class NifComparePane : public PaneContent
{
public:
    explicit NifComparePane(const std::wstring& name);
    ~NifComparePane();

    // PaneContent: this is the 3D NIF content.
    Kind PaneKind() const override { return Kind::Nif; }

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
    std::wstring CurrentPath() const override
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
    bool Load(const std::wstring& path, std::string* error = nullptr) override;
    void Clear() override;

    // Show a target file name immediately (Loading placeholder) WITHOUT
    // starting the load. Used at startup so every pane appears named before the
    // archive scan finishes; a later StartPendingLoad() does the actual load.
    void ShowPendingFile(const std::wstring& path);

    // Queue the parse job for a pane already named via ShowPendingFile, with no
    // UI churn - so the startup batch can queue every pane's main load before
    // any completion (and thus any lower-priority thumbnail) runs. No-op unless
    // the pane is in the Loading placeholder state.
    void StartPendingLoad();

    // Re-resolve this pane's textures after the archive scan lands (no-op until
    // a model is loaded). Archive textures that missed during the scan pop in.
    void RefreshTextures() { if (m_doc) m_viewport->RefreshTextures(); }

    // Wire the shared resources onto BOTH the 3D viewport and the base strip.
    void SetResourceResolver(ResourceResolver* resolver) override;
    void SetTextureRepository(TextureRepository* repository) override;
    void SetRenderDevice(RenderDevice* device) override;
    void SetResourceManager(ResourceManager* manager) override;
    void InvalidateTextureCache();

    // (Thumbnail strip enable/size, resize, keyboard browsing and the
    // thumbnail-chosen callback now live on ComparePane - the strip is shared.)

    // Fires after Load/Clear changed this pane's document - NifCompareView
    // uses it to refresh document-dependent control state (e.g. whether the
    // Parallax Height slider applies to anything currently loaded).
    void SetOnDocumentChanged(std::function<void()> handler) { m_onDocumentChanged = std::move(handler); }
    // (SetOnFileOpened lives on PaneContent now, shared by every content kind;
    // NIF reports through it once its async parse lands - see AcceptLoaded.)

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
    std::shared_ptr<FD2D::Text> m_pathLabel;  // top strip: full path of the loaded .nif + picked sub-mesh name
    std::shared_ptr<FD2D::Text> m_statsLabel; // bottom strip, right-aligned: total (and selected sub-mesh) triangle counts
    std::wstring m_selectedName;              // name of the viewport's picked sub-mesh, empty when none
    std::wstring m_selectedKind;              // shader kind of the picked sub-mesh (see NifViewport::ShaderKindFor)
    std::function<void()> m_onDocumentChanged;
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
