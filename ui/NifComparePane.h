// NifComparePane.h - one compare slot in a dynamic 2-4 pane NifCompareView:
// a NifViewport plus its own "Open ..."/"Close" button row docked directly
// beneath it (so loading/closing a file is anchored right next to the view
// it affects), generalizing the ad-hoc per-side leftDock/rightDock pattern
// liteviewer's NifCompareView constructor used to hand-duplicate for exactly
// two panes (see NIF_DIFF_VIEWER.md / README.md's FICture2-composition note)
// into a reusable class that NifCompareView can instantiate 1-4 of.
#pragma once

#include "NifViewport.h"
#include "../core/NifDocument.h"
#include "../core/ResourceResolver.h"

#include <DockPanel.h>
#include <Button.h>
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

    void SetOnOpen(std::function<void()> handler);
    void SetOnClose(std::function<void()> handler);

private:
    void UpdatePathLabel();

    std::shared_ptr<NifViewport> m_viewport;
    std::shared_ptr<FD2D::Text> m_pathLabel; // top strip: full path of the loaded .nif
    std::shared_ptr<FD2D::Button> m_openBtn;
    std::shared_ptr<FD2D::Button> m_closeBtn;
    std::unique_ptr<NifDocument> m_doc;
};

} // namespace nsk
