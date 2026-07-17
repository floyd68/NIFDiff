#include "ComparePane.h"

#include "NifComparePane.h"
#include "ImagePane.h"

#include "ImageCore/ImageDecodeDispatcher.h" // which extensions are textures

#include <Backplate.h>
#include <VirtualPath.h> // Floar: is the opened path an archive to browse into?

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace nsk
{

namespace
{
    // True when `path`'s extension is one ImageCore can decode (so it opens as
    // image content); anything else (incl. .nif and archives) is NIF content.
    bool PathIsImage(const std::wstring& path)
    {
        std::wstring ext = std::filesystem::path(path).extension().wstring();
        for (wchar_t& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        if (ext.empty())
            return false;
        for (const std::wstring& s : ImageCore::ImageDecodeDispatcher::GetSupportedExtensions())
            if (ext == s)
                return true;
        return false;
    }
}

ComparePane::ComparePane(const std::wstring& name)
    : FD2D::DockPanel(name)
{
    m_thumbStrip = std::make_shared<ThumbnailStrip>(name + L"_Thumbs");
    m_thumbStrip->SetOrientation(ThumbnailStrip::Orientation::Horizontal);
    m_thumbStrip->SetOnActivated([this](const std::wstring& path)
    {
        // Route through the owner (kind-aware, mirrored); fall back to an
        // in-place load when unowned (headless/tests) - safe, the strip stays.
        if (m_onThumbnailChosen)
            m_onThumbnailChosen(path);
        else
        {
            std::string err;
            Load(path, &err);
        }
    });
    m_thumbStrip->SetOnResize([this](float ext, bool committed)
    {
        if (m_onThumbStripResize)
            m_onThumbStripResize(ext, committed);
    });

    // Start with empty NIF content (shows the viewport's placeholder grid).
    EnsureContent(Kind::Nif);
}

ComparePane::~ComparePane() = default;

ComparePane::Kind ComparePane::PaneKind() const
{
    return m_content ? m_content->PaneKind() : Kind::Nif;
}

std::wstring ComparePane::CurrentPath() const
{
    return m_content ? m_content->CurrentPath() : std::wstring();
}

std::wstring ComparePane::SessionPath() const
{
    // A loaded file wins; otherwise, if we're browsing a folder/archive, persist
    // that container so a restored pane resumes browsing it (its path lives in
    // the strip, not in any content's CurrentPath).
    if (std::wstring loaded = CurrentPath(); !loaded.empty())
        return loaded;
    if (m_browsingContainer && m_thumbStrip)
        return m_thumbStrip->Folder();
    return std::wstring();
}

NifComparePane* ComparePane::NifContent()
{
    return dynamic_cast<NifComparePane*>(m_content.get());
}

ImagePane* ComparePane::ImageContent()
{
    return dynamic_cast<ImagePane*>(m_content.get());
}

PaneContent* ComparePane::EnsureContent(Kind kind)
{
    if (m_content && m_content->PaneKind() == kind)
        return m_content.get();

    const std::wstring cname = Name() + L"_Content";
    if (kind == Kind::Image)
        m_content = std::make_shared<ImagePane>(cname);
    else
        m_content = std::make_shared<NifComparePane>(cname);

    WireContent();
    Redock();
    return m_content.get();
}

bool ComparePane::Load(const std::wstring& path, std::string* error)
{
    if (path.empty())
        return false;

    // A container to browse rather than a file to view: a plain directory, or an
    // archive itself (.bsa/.ba2/.zip/... with no inner path). Descend into it in
    // the strip so the user picks something inside. (A path INSIDE an archive is
    // not IsArchiveFile and resolves to a real member, so it loads as content.)
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(path, ec);
    auto vp = Floar::VirtualPath::Parse(path);
    if (isDir || (vp && vp->IsArchiveFile()))
    {
        // No file loaded - the pane is browsing this container. Remember that so
        // a later ShowThumbnailFolder (e.g. ShowAllThumbnailStrips after the
        // archive scan) keeps the strip up instead of collapsing it for an empty
        // CurrentPath.
        m_browsingContainer = true;
        if (m_thumbStrip)
        {
            m_thumbStrip->SetActive(true);
            m_thumbStrip->ShowForFolder(path);
        }
        if (m_onFileOpened) // a browsed folder/archive is an "open" too (MRU/session)
            m_onFileOpened(path);
        Invalidate();
        return true;
    }

    m_browsingContainer = false; // a real file is loading - leave browse mode
    EnsureContent(PathIsImage(path) ? Kind::Image : Kind::Nif);
    const bool ok = m_content->Load(path, error);
    ShowThumbnailFolder(); // strip follows the loaded file's folder
    return ok;
}

void ComparePane::Clear()
{
    m_browsingContainer = false;
    if (m_content)
        m_content->Clear();
    ShowThumbnailFolder(); // now empty -> clears/collapses the strip
}

void ComparePane::SetRenderDevice(RenderDevice* device)
{
    m_renderDevice = device;
    if (m_thumbStrip) m_thumbStrip->SetRenderDevice(device);
    if (m_content) m_content->SetRenderDevice(device);
}

void ComparePane::SetResourceManager(ResourceManager* manager)
{
    m_resourceManager = manager;
    if (m_thumbStrip) m_thumbStrip->SetResourceManager(manager);
    if (m_content) m_content->SetResourceManager(manager);
}

void ComparePane::SetResourceResolver(ResourceResolver* resolver)
{
    m_resolver = resolver;
    if (m_thumbStrip) m_thumbStrip->SetResourceResolver(resolver);
    if (m_content) m_content->SetResourceResolver(resolver);
}

void ComparePane::SetTextureRepository(TextureRepository* repository)
{
    m_textureRepository = repository;
    if (m_thumbStrip) m_thumbStrip->SetTextureRepository(repository);
    if (m_content) m_content->SetTextureRepository(repository);
}

void ComparePane::WireContent()
{
    if (!m_content)
        return;
    // A freshly created content inherits the remembered resources...
    if (m_resolver) m_content->SetResourceResolver(m_resolver);
    if (m_textureRepository) m_content->SetTextureRepository(m_textureRepository);
    if (m_renderDevice) m_content->SetRenderDevice(m_renderDevice);
    if (m_resourceManager) m_content->SetResourceManager(m_resourceManager);
    // ...its file-open reports funnel up through the frame (one MRU channel for
    // every kind)...
    m_content->SetOnFileOpened([this](const std::wstring& p)
    {
        if (m_onFileOpened)
            m_onFileOpened(p);
    });
    // ...and it gets its kind-specific callbacks (re)wired by the view.
    if (m_onContentCreated)
        m_onContentCreated(m_content.get());
}

void ComparePane::Redock()
{
    // Rebuild the dock cleanly (per DockPanel: a plain remove leaves stale dock
    // bookkeeping). The strip object persists across swaps - only re-parented -
    // so its folder/scroll/selection survive. Fill (content) must come last.
    ClearDocks();
    if (m_thumbStrip)
    {
        AddChild(m_thumbStrip);
        SetChildDock(m_thumbStrip, FD2D::Dock::Bottom);
    }
    if (m_content)
    {
        AddChild(m_content);
        SetChildDock(m_content, FD2D::Dock::Fill);
    }
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

} // namespace nsk
