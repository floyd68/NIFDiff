// ComparePane.h - common base for the panes a NifCompareView lays out.
//
// NifCompareView was written for a homogeneous set of NifComparePane; the
// texture view/compare port adds a second pane kind (an image/texture pane)
// that must sit in the SAME host tree and be arranged freely alongside NIF
// panes. This base carves out the surface the view needs to treat any pane
// generically - identity, the load/clear lifecycle, its kind, and the shared
// per-pane thumbnail strip (folder/archive browser). Kind-specific behaviour
// (a NIF pane's 3D viewport + material/animation tooling; an image pane's
// zoom/pan/channels) stays on the concrete panes and is reached via PaneKind.
#pragma once

#include "ThumbnailStrip.h" // strip + RenderDevice/ResourceResolver/... types

#include <DockPanel.h>

#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class NifViewport;

class ComparePane : public FD2D::DockPanel
{
public:
    enum class Kind
    {
        Nif,   // NifComparePane: a 3D NifViewport
        Image, // ImagePane: a decoded texture
    };

    explicit ComparePane(const std::wstring& name);
    ~ComparePane() override = default;

    // Which concrete pane this is; the view uses it to gate kind-specific work.
    virtual Kind PaneKind() const = 0;

    // Full path of the file shown (or loading) here; empty when the pane is
    // free. The single string every open path funnels through.
    virtual std::wstring CurrentPath() const = 0;

    // Open `path` into this pane. Returns false only when it can't be accepted.
    virtual bool Load(const std::wstring& path, std::string* error = nullptr) = 0;

    // Return the pane to its empty state.
    virtual void Clear() = 0;

    // Shared resources for this pane's thumbnail strip (and, for a NIF pane, its
    // viewport - NifComparePane overrides to wire both). Virtual so image panes
    // get the strip wired without the NIF viewport plumbing.
    virtual void SetRenderDevice(RenderDevice* device) { if (m_thumbStrip) m_thumbStrip->SetRenderDevice(device); }
    virtual void SetResourceManager(ResourceManager* manager) { if (m_thumbStrip) m_thumbStrip->SetResourceManager(manager); }
    virtual void SetResourceResolver(ResourceResolver* resolver) { if (m_thumbStrip) m_thumbStrip->SetResourceResolver(resolver); }
    virtual void SetTextureRepository(TextureRepository* repository) { if (m_thumbStrip) m_thumbStrip->SetTextureRepository(repository); }

    // --- Per-pane thumbnail strip (folder/archive browser) -------------------
    // List this pane's current file's folder (empty clears the strip).
    void ShowThumbnailFolder() { if (m_thumbStrip) m_thumbStrip->ShowForFile(CurrentPath()); }
    void SetThumbnailStripEnabled(bool enabled) { if (m_thumbStrip) m_thumbStrip->SetEnabled(enabled); }
    void SetThumbnailStripSize(float extent) { if (m_thumbStrip) m_thumbStrip->SetFixedExtent(extent); }
    void SetOnThumbnailStripResize(std::function<void(float, bool)> handler) { m_onThumbStripResize = std::move(handler); }
    // Fired when the user picks a file from this pane's strip; the view routes it
    // by kind (a texture opens as an image, a .nif as a NIF) and mirrors it.
    void SetOnThumbnailChosen(std::function<void(const std::wstring&)> handler) { m_onThumbnailChosen = std::move(handler); }

    // Keyboard browsing over the strip (owner-driven), all kinds.
    std::wstring StepThumbnailFile(int delta) { return m_thumbStrip ? m_thumbStrip->StepSelection(delta) : std::wstring(); }
    std::wstring EdgeThumbnailFile(bool last) { return m_thumbStrip ? m_thumbStrip->EdgeSelection(last) : std::wstring(); }
    bool ActivateThumbnailSelection() { return m_thumbStrip && m_thumbStrip->ActivateSelection(); }
    void NavigateThumbnailUp() { if (m_thumbStrip) m_thumbStrip->NavigateUp(); }
    bool ThumbnailStripHasFocus() const { return m_thumbStrip && m_thumbStrip->HasFocus(); }
    std::wstring TypeToSelectThumbnail(wchar_t ch) { return m_thumbStrip ? m_thumbStrip->TypeToSelect(ch) : std::wstring(); }
    void FocusThumbnailStrip() { if (m_thumbStrip) m_thumbStrip->RequestFocus(); }

    ThumbnailStrip* ThumbStrip() { return m_thumbStrip.get(); }

protected:
    // Owned by the base (created + wired in the ctor); each concrete pane docks
    // it along its bottom edge in its own constructor (Fill child must come last,
    // so the derived class controls dock order).
    std::shared_ptr<ThumbnailStrip> m_thumbStrip;
    std::function<void(const std::wstring&)> m_onThumbnailChosen;
    std::function<void(float, bool)> m_onThumbStripResize;
};

} // namespace nsk
