// ComparePane.h - one slot in a NifCompareView: a persistent folder/archive
// thumbnail strip plus a swappable content (a NIF viewport or a texture).
//
// Opening a file changes only the CONTENT to match the file's kind - the strip
// stays put - so a texture picked while browsing a NIF folder (or vice versa)
// loads IN PLACE and browsing continues, with no pane teardown. Because the
// strip is never destroyed mid-callback, the swap is safe to do synchronously
// from the strip's own pick handler. Content lives behind PaneContent; the view
// reaches kind-specific behaviour via NifContent() / ImageContent().
#pragma once

#include "PaneContent.h"  // Kind + RenderDevice/... types
#include "ThumbnailStrip.h"

#include <DockPanel.h>

#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class NifComparePane;
class ImagePane;

class ComparePane : public FD2D::DockPanel
{
public:
    using Kind = PaneContent::Kind;

    explicit ComparePane(const std::wstring& name);
    ~ComparePane() override;

    // The pane's kind is its current content's kind.
    Kind PaneKind() const;
    std::wstring CurrentPath() const;

    // The path to persist for a session restore: the loaded file's path, or -
    // when the pane is browsing a folder/archive with no file open - that
    // container's path (so the restored pane resumes browsing it). Empty when
    // the pane holds nothing.
    std::wstring SessionPath() const;

    // Open `path`, swapping the content to the kind the path needs first (an
    // image into a NIF pane becomes an image content, and vice versa) - the
    // strip persists. Returns false only when it can't be accepted.
    bool Load(const std::wstring& path, std::string* error = nullptr);
    void Clear();

    // Kind-specific content, or null when the pane is the other kind. The view
    // uses these (via AsNif/AsImage) to reach viewport / image behaviour.
    NifComparePane* NifContent();
    ImagePane* ImageContent();
    PaneContent* Content() { return m_content.get(); }
    ThumbnailStrip* ThumbStrip() { return m_thumbStrip.get(); }

    // Ensure the content is `kind` (creating/swapping if needed) and return it.
    PaneContent* EnsureContent(Kind kind);

    // Shared resources: remembered so a later content swap inherits them, and
    // forwarded to the strip + current content now.
    void SetRenderDevice(RenderDevice* device);
    void SetResourceManager(ResourceManager* manager);
    void SetResourceResolver(ResourceResolver* resolver);
    void SetTextureRepository(TextureRepository* repository);

    // --- Persistent thumbnail strip (folder/archive browser) -----------------
    void ShowThumbnailFolder()
    {
        if (!m_thumbStrip) return;
        if (m_browsingContainer)
        {
            // Browsing a folder/archive with no file loaded: keep the strip up
            // and leave its listing alone (ShowForFile("") would clear it).
            m_thumbStrip->SetActive(true);
            return;
        }
        m_thumbStrip->SetActive(!CurrentPath().empty()); // reserve space while occupied
        m_thumbStrip->ShowForFile(CurrentPath());
    }
    void SetThumbnailStripEnabled(bool enabled) { if (m_thumbStrip) m_thumbStrip->SetEnabled(enabled); }
    void SetThumbnailStripSize(float extent) { if (m_thumbStrip) m_thumbStrip->SetFixedExtent(extent); }
    void SetOnThumbnailStripResize(std::function<void(float, bool)> handler) { m_onThumbStripResize = std::move(handler); }
    void SetOnThumbnailChosen(std::function<void(const std::wstring&)> handler) { m_onThumbnailChosen = std::move(handler); }

    std::wstring StepThumbnailFile(int delta) { return m_thumbStrip ? m_thumbStrip->StepSelection(delta) : std::wstring(); }
    std::wstring PageThumbnailFile(int direction) { return m_thumbStrip ? m_thumbStrip->PageSelection(direction) : std::wstring(); }
    std::wstring EdgeThumbnailFile(bool last) { return m_thumbStrip ? m_thumbStrip->EdgeSelection(last) : std::wstring(); }
    bool ActivateThumbnailSelection() { return m_thumbStrip && m_thumbStrip->ActivateSelection(); }
    void NavigateThumbnailUp() { if (m_thumbStrip) m_thumbStrip->NavigateUp(); }
    bool ThumbnailStripHasFocus() const { return m_thumbStrip && m_thumbStrip->HasFocus(); }
    std::wstring TypeToSelectThumbnail(wchar_t ch) { return m_thumbStrip ? m_thumbStrip->TypeToSelect(ch) : std::wstring(); }
    void FocusThumbnailStrip() { if (m_thumbStrip) m_thumbStrip->RequestFocus(); }

    // Fired after the content is (re)created, so the view can wire kind-specific
    // callbacks (NIF camera sync + doc/file-opened; image view-transform sync).
    // Re-run on every content swap.
    void SetOnContentCreated(std::function<void(PaneContent*)> handler) { m_onContentCreated = std::move(handler); }

    // Fired for every open through this pane, whatever its kind: a NIF (once its
    // async parse lands), a texture, or a folder/archive being browsed into. The
    // app wires this for the recent-files (MRU) list, so all four kinds land
    // there. The content kinds report via PaneContent::SetOnFileOpened (wired in
    // WireContent); the container case fires directly from Load.
    void SetOnFileOpened(std::function<void(const std::wstring&)> handler) { m_onFileOpened = std::move(handler); }

private:
    bool FinishContainerListing(
        const std::wstring& path,
        std::string* error = nullptr);
    void Redock();     // ClearDocks + re-add strip (Bottom) then content (Fill)
    void WireContent(); // apply remembered resources + fire m_onContentCreated

    std::shared_ptr<ThumbnailStrip> m_thumbStrip; // persistent across content swaps
    std::shared_ptr<PaneContent> m_content;       // NifComparePane XOR ImagePane
    bool m_browsingContainer = false;             // strip is showing a folder/archive, no file loaded
    std::wstring m_pendingContainerListing;       // initial async archive listing awaiting default selection

    RenderDevice* m_renderDevice = nullptr;
    ResourceManager* m_resourceManager = nullptr;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;

    std::function<void(const std::wstring&)> m_onThumbnailChosen;
    std::function<void(float, bool)> m_onThumbStripResize;
    std::function<void(PaneContent*)> m_onContentCreated;
    std::function<void(const std::wstring&)> m_onFileOpened;
};

} // namespace nsk
