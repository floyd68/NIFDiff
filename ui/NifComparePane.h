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
#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class NifComparePane : public FD2D::DockPanel
{
public:
    explicit NifComparePane(const std::wstring& name);

    NifViewport& Viewport() { return *m_viewport; }
    const NifDocument* Document() const { return m_doc.get(); }
    // Full path of the .nif currently loaded here, empty when the pane is
    // cleared - the thumbnail strip follows this to list the pane's folder.
    std::wstring CurrentPath() const { return m_doc ? m_doc->filePath() : std::wstring(); }

    bool Load(const std::wstring& path, std::string* error = nullptr);
    void Clear();

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

    // Keyboard stepping over this pane's thumbnail strip (owner-driven).
    // StepThumbnailFile returns the prev/next sibling .nif to load (empty if
    // none); NavigateThumbnailUp browses the strip to the parent folder.
    std::wstring StepThumbnailFile(int delta) const { return m_thumbStrip->StepFile(delta); }
    std::wstring EdgeThumbnailFile(bool last) const { return m_thumbStrip->EdgeFile(last); }
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
};

} // namespace nsk
