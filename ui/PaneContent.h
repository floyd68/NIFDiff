// PaneContent.h - the swappable content of a ComparePane.
//
// A ComparePane is a persistent frame (a folder/archive thumbnail strip plus
// the slot the content fills). Its content is one of these: a NIF viewport
// (NifComparePane) or a decoded texture (ImagePane). Opening a file of the
// other kind swaps ONLY the content - the strip stays put - so browsing and
// in-place kind changes are seamless and safe (the strip is never destroyed
// mid-callback). This interface is the surface the frame drives generically.
#pragma once

#include "ThumbnailStrip.h" // RenderDevice/ResourceResolver/... types

#include <DockPanel.h>

#include <functional>
#include <string>
#include <utility>

namespace nsk
{

class PaneContent : public FD2D::DockPanel
{
public:
    enum class Kind
    {
        Nif,   // NifComparePane: a 3D NifViewport
        Image, // ImagePane: a decoded texture
    };

    explicit PaneContent(const std::wstring& name) : FD2D::DockPanel(name) {}
    ~PaneContent() override = default;

    virtual Kind PaneKind() const = 0;
    virtual std::wstring CurrentPath() const = 0;
    virtual bool Load(const std::wstring& path, std::string* error = nullptr) = 0;
    virtual void Clear() = 0;

    // Show `text` in the content's header/path label instead of its usual
    // "(no file)" placeholder - used while the pane is browsing a container that
    // has no viewable file to load, so the pane still names where it is. Default
    // no-op; a NIF placeholder content overrides it.
    virtual void SetBrowsingLabel(const std::wstring& /*text*/) {}

    // Shared resources (the frame forwards these to the content on creation and
    // whenever they change). A NIF content wires its viewport; an image content
    // ignores what it doesn't need.
    virtual void SetRenderDevice(RenderDevice*) {}
    virtual void SetResourceManager(ResourceManager*) {}
    virtual void SetResourceResolver(ResourceResolver*) {}
    virtual void SetTextureRepository(TextureRepository*) {}

    // Fired when the content has successfully opened and can present a file -
    // a NIF once its async parse lands, an image after decode and presentation
    // resource creation succeed. The frame wires this so the app can record the
    // file (MRU / session). Every content kind reports through the same channel.
    void SetOnFileOpened(std::function<void(const std::wstring&)> handler)
    {
        m_onFileOpened = std::move(handler);
    }

protected:
    void NotifyFileOpened(const std::wstring& path)
    {
        if (m_onFileOpened)
            m_onFileOpened(path);
    }

private:
    std::function<void(const std::wstring&)> m_onFileOpened;
};

} // namespace nsk
