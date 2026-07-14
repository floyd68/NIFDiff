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

    bool Load(const std::wstring& path, std::string* error = nullptr);
    void Clear();

    void SetResourceResolver(ResourceResolver* resolver);
    void InvalidateTextureCache();

    // Fires after Load/Clear changed this pane's document - NifCompareView
    // uses it to refresh document-dependent control state (e.g. whether the
    // Parallax Height slider applies to anything currently loaded).
    void SetOnDocumentChanged(std::function<void()> handler) { m_onDocumentChanged = std::move(handler); }

private:
    void UpdatePathLabel();
    void UpdateStatsLabel();

    std::shared_ptr<NifViewport> m_viewport;
    std::shared_ptr<FD2D::Text> m_pathLabel;  // top strip: full path of the loaded .nif + picked sub-mesh name
    std::shared_ptr<FD2D::Text> m_statsLabel; // bottom strip, right-aligned: total (and selected sub-mesh) triangle counts
    std::wstring m_selectedName;              // name of the viewport's picked sub-mesh, empty when none
    std::wstring m_selectedKind;              // shader kind of the picked sub-mesh (see NifViewport::ShaderKindFor)
    std::function<void()> m_onDocumentChanged;
    std::unique_ptr<NifDocument> m_doc;
};

} // namespace nsk
