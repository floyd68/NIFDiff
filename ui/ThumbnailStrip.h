// ThumbnailStrip.h - a folder browser that renders each .nif to a small 3D
// thumbnail and lists them in a scrollable side strip (FICture2's
// ThumbnailPane + AsyncThumbLoader equivalent, tracked as todo #12).
//
// Enabled by the render split (RenderDevice + RenderTarget + RenderMeshCache):
// the strip owns ONE small offscreen RenderTarget and reuses the app-wide
// shared RenderDevice to draw each model headlessly, then copies the result
// into a per-thumbnail texture wrapped as an ID2D1Bitmap1 for display. No
// extra device/shaders/IBL - those are shared. Thumbnails are generated a few
// per frame (in the D3D pass) so opening a large folder never blocks the UI;
// the parse itself still happens on the UI thread (a background loader is a
// later refinement). Clicking a thumbnail activates it (the owner loads it
// into a pane).
#pragma once

#include "../core/ResourceResolver.h"
#include "../render/RenderDevice.h"
#include "../render/RenderTarget.h"
#include "../render/RenderMeshCache.h"
#include "../render/TextureRepository.h"

#include <Wnd.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nsk
{

class ThumbnailStrip : public FD2D::Wnd
{
public:
    // Vertical = a fixed-width column of stacked cards (docks Left/Right);
    // Horizontal = a fixed-height row of cards (docks Top/Bottom). The scroll
    // axis, hit-test and Measure axis all follow this.
    enum class Orientation { Vertical, Horizontal };

    explicit ThumbnailStrip(const std::wstring& name);

    // Layout orientation; the owner must dock the strip on the matching edge.
    void SetOrientation(Orientation o);
    Orientation GetOrientation() const { return m_horizontal ? Orientation::Horizontal : Orientation::Vertical; }

    // Shared, app-owned; must outlive the strip. Set before attach/use.
    void SetRenderDevice(RenderDevice* device) { m_renderDevice = device; }
    void SetResourceResolver(ResourceResolver* resolver) { m_resolver = resolver; }
    void SetTextureRepository(TextureRepository* repository) { m_textureRepository = repository; }

    // Point the strip at a folder: enumerates *.nif (non-recursive, sorted)
    // and queues each for thumbnail generation. Empty string clears the strip.
    void SetFolder(const std::wstring& folder);
    const std::wstring& Folder() const { return m_folder; }
    bool HasContent() const { return !m_entries.empty(); }

    // Fires when a thumbnail is clicked, with its full .nif path.
    void SetOnActivated(std::function<void(const std::wstring&)> handler) { m_onActivated = std::move(handler); }

    // Full path of the thumbnail under the cursor, so Backplate's hover
    // tooltip reveals it (the card label only shows the file name, ellipsized).
    std::wstring TooltipText() const override;

    void OnAttached(FD2D::Backplate& backplate) override;
    FD2D::Size Measure(FD2D::Size available) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    struct Entry
    {
        std::wstring path;
        std::wstring name;      // file name only, for the label
        bool rendered = false;  // 3D pass produced a texture
        bool failed = false;    // parse/build failed - show a placeholder
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;   // persistent copy of the render
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;   // D2D view of tex (built lazily in the D2D pass)
    };

    // Renders one queued entry's model into m_thumbTarget and copies it into a
    // persistent per-entry texture. Returns false when nothing was pending.
    bool RenderNextThumbnail();
    // Builds the D2D bitmap for an entry whose texture exists but bitmap doesn't.
    void EnsureBitmap(Entry& entry);
    void EnsureTextFormat();
    int CardAtPoint(const POINT& pt) const; // -1 when none
    // Geometry helpers, orientation-aware. thumb = square thumbnail side;
    // cardMain = per-card size along the scroll axis; leadGutter = space before
    // the first card (the header in vertical mode).
    float ThumbSide() const;
    float CardMain() const;
    float LeadGutter() const;
    float ContentExtent() const; // total size along the scroll axis
    void ClampScroll();

    RenderDevice* m_renderDevice = nullptr;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    RenderTarget m_thumbTarget;   // reused for every thumbnail render
    RenderMeshCache m_thumbCache; // cleared per model

    std::wstring m_folder;
    std::vector<Entry> m_entries;
    std::size_t m_nextToRender = 0; // index of the next entry to generate
    bool m_horizontal = false;
    float m_scroll = 0.0f;          // offset along the scroll axis (Y or X)
    int m_hoverCard = -1;

    std::function<void(const std::wstring&)> m_onActivated;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
};

} // namespace nsk
