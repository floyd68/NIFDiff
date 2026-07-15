#include "ThumbnailStrip.h"

#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"
#include "../core/Camera.h"
#include "../render/TextureCache.h"

#include <Backplate.h>
#include <Core.h>
#include <Util.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace nsk
{

namespace
{
    constexpr float kFixedExtent = 196.0f; // strip's fixed dim (width if vertical, height if horizontal)
    constexpr float kPad = 8.0f;
    constexpr float kLabelH = 20.0f;
    constexpr float kHeaderH = 26.0f;      // count header (vertical mode only)
    constexpr UINT kThumbPx = 168;         // offscreen render resolution
    constexpr int kPerFrame = 2;           // thumbnails generated per frame
    constexpr float kFovY = 0.9f;
}

ThumbnailStrip::ThumbnailStrip(const std::wstring& name)
    : FD2D::Wnd(name)
{
}

void ThumbnailStrip::SetOrientation(Orientation o)
{
    const bool h = (o == Orientation::Horizontal);
    if (h == m_horizontal)
        return;
    m_horizontal = h;
    m_scroll = 0.0f;
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

void ThumbnailStrip::SetEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    m_hoverCard = -1;
    // Toggling presence changes our measured extent (0 when off), so relayout;
    // re-enabling resumes generation from where it left off (m_nextToRender).
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

float ThumbnailStrip::ThumbSide() const
{
    // Horizontal leaves room below the square thumbnail for the label row;
    // vertical uses the full width for the thumbnail (label sits under it).
    return m_horizontal ? (kFixedExtent - kLabelH - kPad * 3.0f)
                        : (kFixedExtent - kPad * 2.0f);
}

float ThumbnailStrip::CardMain() const
{
    // Size of one card along the scroll axis.
    return m_horizontal ? (ThumbSide() + kPad * 2.0f)
                        : (ThumbSide() + kLabelH + kPad * 2.0f);
}

float ThumbnailStrip::LeadGutter() const
{
    return m_horizontal ? kPad : kHeaderH;
}

float ThumbnailStrip::ContentExtent() const
{
    return LeadGutter() + static_cast<float>(m_entries.size()) * CardMain() + kPad;
}

void ThumbnailStrip::OnAttached(FD2D::Backplate& backplate)
{
    FD2D::Wnd::OnAttached(backplate);
    m_device = backplate.D3DDevice();
    m_context = backplate.D3DContext();
}

void ThumbnailStrip::ShowForFile(const std::wstring& nifPath)
{
    if (nifPath.empty())
    {
        // The active pane has nothing loaded: clear the strip.
        if (!m_folder.empty() || !m_entries.empty())
            NavigateTo(std::wstring(), std::wstring());
        return;
    }
    std::filesystem::path p(nifPath);
    const std::wstring dir = p.has_parent_path() ? p.parent_path().wstring() : std::wstring();
    if (dir == m_folder && !m_entries.empty())
    {
        // Same folder already listed - just move the highlight, no re-list.
        if (m_currentFile != nifPath)
        {
            m_currentFile = nifPath;
            Invalidate();
        }
        return;
    }
    NavigateTo(dir, nifPath);
}

void ThumbnailStrip::NavigateTo(std::wstring folder, std::wstring selectPath)
{
    m_entries.clear();
    m_nextToRender = 0;
    m_scroll = 0.0f;
    m_hoverCard = -1;
    m_folder = folder;
    m_currentFile = selectPath;
    m_thumbCache.Clear();

    auto lessNoCase = [](const Entry& a, const Entry& b)
    {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };

    if (!folder.empty())
    {
        std::error_code ec;
        std::filesystem::path dir(folder);
        if (std::filesystem::is_directory(dir, ec))
        {
            // ".." to the parent (unless this is a filesystem root).
            const std::filesystem::path parent = dir.parent_path();
            if (!parent.empty() && parent != dir)
            {
                Entry up;
                up.kind = EntryKind::Up;
                up.path = parent.wstring();
                up.name = L"..";
                up.rendered = true;
                m_entries.push_back(std::move(up));
            }

            std::vector<Entry> folders, files;
            for (const auto& de : std::filesystem::directory_iterator(dir, ec))
            {
                if (ec) break;
                std::error_code ec2;
                if (de.is_directory(ec2))
                {
                    Entry f;
                    f.kind = EntryKind::Folder;
                    f.path = de.path().wstring();
                    f.name = de.path().filename().wstring();
                    f.rendered = true;
                    folders.push_back(std::move(f));
                }
                else if (de.is_regular_file(ec2))
                {
                    std::wstring ext = de.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (ext != L".nif") continue;
                    Entry e;
                    e.kind = EntryKind::File;
                    e.path = de.path().wstring();
                    e.name = de.path().filename().wstring();
                    files.push_back(std::move(e));
                }
            }
            std::sort(folders.begin(), folders.end(), lessNoCase);
            std::sort(files.begin(), files.end(), lessNoCase);
            m_entries.insert(m_entries.end(), std::make_move_iterator(folders.begin()),
                             std::make_move_iterator(folders.end()));
            m_entries.insert(m_entries.end(), std::make_move_iterator(files.begin()),
                             std::make_move_iterator(files.end()));
        }
    }

    // Presence (and thus our measured extent) may have changed, so relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

FD2D::Size ThumbnailStrip::Measure(FD2D::Size available)
{
    if (!HasContent() || !m_enabled)
    {
        m_desired = { 0.0f, 0.0f };
        return m_desired;
    }
    m_desired = m_horizontal ? FD2D::Size { available.w, kFixedExtent }
                             : FD2D::Size { kFixedExtent, available.h };
    return m_desired;
}

bool ThumbnailStrip::RenderNextThumbnail()
{
    // Folder/Up tiles need no 3D render - skip past them (they sort before the
    // .nif files, so this just advances the cursor to the first file once).
    while (m_nextToRender < m_entries.size() &&
           m_entries[m_nextToRender].kind != EntryKind::File)
        ++m_nextToRender;
    if (m_nextToRender >= m_entries.size())
        return false;
    if (!m_renderDevice || !m_renderDevice->IsInitialized() || !m_device ||
        !m_context || !m_textureRepository)
        return false;

    Entry& e = m_entries[m_nextToRender++];

    NifDocument doc;
    std::string err;
    if (!doc.loadFromFile(e.path, &err) || !doc.isValid())
    {
        e.failed = true; e.rendered = true;
        return true;
    }
    std::vector<RenderMesh> meshes = SceneBuilder::build(doc);
    if (meshes.empty())
    {
        e.failed = true; e.rendered = true;
        return true;
    }

    // World-space bounds -> framing (same as NifViewport's fresh-load fit).
    Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
    bool any = false;
    for (const RenderMesh& mesh : meshes)
    {
        if (!mesh.geometry) continue;
        for (const Vector3& p : mesh.geometry->positions)
        {
            Vector3 wp = mesh.worldTransform * p;
            minB.boundMin(wp); maxB.boundMax(wp); any = true;
        }
    }
    if (!any)
    {
        e.failed = true; e.rendered = true;
        return true;
    }
    const Vector3 center = (minB + maxB) * 0.5f;
    const float radius = (std::max)((maxB - minB).length() * 0.5f, 1.0f);

    if (!m_thumbTarget.Resize(m_device, kThumbPx, kThumbPx, 1))
    {
        e.failed = true; e.rendered = true;
        return true;
    }
    m_thumbCache.Clear();

    std::filesystem::path nifPath(e.path);
    TextureCache textures(m_textureRepository);
    textures.SetNifDirectory(nifPath.has_parent_path() ? nifPath.parent_path().wstring() : std::wstring());

    Camera cam;
    cam.frame(center, radius);
    cam.setOrbit(Camera::kDefaultYaw, Camera::kDefaultPitch);

    RenderSettings s;
    s.view = cam.viewMatrix();
    const float dist = cam.distance();
    const float nearZ = (std::max)(dist * 0.02f, 1e-4f);
    const float farZ = dist + radius * 4.0f + 10.0f;
    s.proj = Camera::projectionMatrix(kFovY, 1.0f, nearZ, farZ);
    s.eyePos = cam.eyePosition();
    s.brightness = 1.15f;   // thumbnails read a touch brighter than the panes
    s.showGrid = false;
    s.showAxes = false;
    s.clearColor = Color4 { 0.11f, 0.11f, 0.13f, 1.0f };

    m_renderDevice->RenderScene(m_thumbTarget, m_thumbCache, meshes, s, &textures);

    // Copy the render into a persistent per-thumbnail texture (the reusable
    // target is about to be overwritten by the next thumbnail).
    const D3D11_TEXTURE2D_DESC td {
        .Width = kThumbPx, .Height = kThumbPx,
        .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> persistent;
    if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &persistent)) && m_thumbTarget.ColorTexture())
    {
        m_context->CopyResource(persistent.Get(), m_thumbTarget.ColorTexture());
        e.tex = std::move(persistent);
    }
    else
    {
        e.failed = true;
    }
    e.rendered = true;
    return true;
}

void ThumbnailStrip::OnRenderD3D(ID3D11DeviceContext* context)
{
    // Off: the loader idles - no parsing/building/rendering happens.
    if (!m_enabled)
    {
        FD2D::Wnd::OnRenderD3D(context);
        return;
    }
    // Generate a few thumbnails per frame in the D3D pass (RenderScene binds
    // its own targets), then keep painting until the whole folder is done.
    for (int i = 0; i < kPerFrame; ++i)
        if (!RenderNextThumbnail())
            break;
    if (m_nextToRender < m_entries.size())
        Invalidate(); // schedule the next batch

    FD2D::Wnd::OnRenderD3D(context);
}

void ThumbnailStrip::EnsureBitmap(Entry& entry)
{
    if (entry.bitmap || !entry.tex || !m_backplate)
        return;
    ID2D1RenderTarget* rt = m_backplate->RenderTarget();
    if (!rt) return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))) || !dc) return;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(entry.tex->QueryInterface(IID_PPV_ARGS(&surface)))) return;

    D2D1_BITMAP_PROPERTIES1 bp {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    dc->GetDpi(&bp.dpiX, &bp.dpiY);
    (void)dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &entry.bitmap);
}

void ThumbnailStrip::EnsureTextFormat()
{
    if (m_textFormat) return;
    IDWriteFactory* dw = FD2D::Core::DWriteFactory();
    if (!dw) return;
    (void)dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"", &m_textFormat);
    if (m_textFormat)
    {
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(m_textFormat.Get(), &ellipsis)))
        {
            DWRITE_TRIMMING trim { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            m_textFormat->SetTrimming(&trim, ellipsis.Get());
        }
    }
}

std::wstring ThumbnailStrip::TooltipText() const
{
    // Backplate only queries this on the window its own deep hit-test placed
    // under the cursor (us), so derive the card straight from the live cursor
    // instead of a routed hover index (MouseMove may not reach a docked strip).
    if (!m_backplate)
        return std::wstring();
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(m_backplate->Window(), &pt))
        return std::wstring();
    const int card = CardAtPoint(pt);
    if (card >= 0 && card < static_cast<int>(m_entries.size()))
        return m_entries[static_cast<std::size_t>(card)].path;
    return std::wstring();
}

void ThumbnailStrip::ClampScroll()
{
    const D2D1_RECT_F r = LayoutRect();
    const float viewMain = m_horizontal ? (r.right - r.left) : (r.bottom - r.top);
    const float maxScroll = (std::max)(0.0f, ContentExtent() - viewMain);
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
}

void ThumbnailStrip::DrawFolderIcon(ID2D1RenderTarget* target, const D2D1_RECT_F& rc, bool up) const
{
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.16f, 0.19f), &brush)))
        target->FillRectangle(rc, brush.Get());

    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
    if (up)
    {
        // Up chevron for the ".." tile.
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.76f, 0.82f), &brush)))
        {
            const float s = (std::min)(w, h) * 0.26f;
            target->DrawLine({ cx - s, cy + s * 0.55f }, { cx, cy - s * 0.75f }, brush.Get(), 3.5f);
            target->DrawLine({ cx, cy - s * 0.75f }, { cx + s, cy + s * 0.55f }, brush.Get(), 3.5f);
        }
    }
    else
    {
        // Folder glyph: a body rect with a small tab on top-left.
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.83f, 0.70f, 0.36f), &brush)))
        {
            const float fw = w * 0.52f, fh = h * 0.36f;
            const float l = cx - fw * 0.5f, t = cy - fh * 0.42f;
            const D2D1_RECT_F tab { l, t - fh * 0.28f, l + fw * 0.42f, t + 1.0f };
            const D2D1_RECT_F body { l, t, l + fw, t + fh };
            target->FillRectangle(tab, brush.Get());
            target->FillRectangle(body, brush.Get());
        }
    }
}

void ThumbnailStrip::OnRender(ID2D1RenderTarget* target)
{
    if (!target || !HasContent() || !m_enabled)
        return;
    EnsureTextFormat();

    const D2D1_RECT_F r = LayoutRect();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;

    // Strip background + a hairline separator on the edge facing the content
    // (right when docked left, top when docked bottom).
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.12f), &brush)))
        target->FillRectangle(r, brush.Get());
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.29f, 0.33f), &brush)))
    {
        const D2D1_RECT_F sep = m_horizontal ? D2D1::RectF(r.left, r.top, r.right, r.top + 1.0f)
                                             : D2D1::RectF(r.right - 1.0f, r.top, r.right, r.bottom);
        target->FillRectangle(sep, brush.Get());
    }

    target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);

    // Header count (vertical mode only - the horizontal strip has no room).
    if (m_textFormat)
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!m_horizontal && m_textFormat && !m_folder.empty() &&
        SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.66f, 0.72f), &brush)))
    {
        // Current folder name, so the strip reads as "the active pane's folder".
        const std::wstring hdr = std::filesystem::path(m_folder).filename().wstring();
        target->DrawTextW(hdr.c_str(), static_cast<UINT32>(hdr.size()), m_textFormat.Get(),
            D2D1::RectF(r.left + kPad, r.top + 6.0f, r.right - kPad, r.top + kHeaderH),
            brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const float thumb = ThumbSide();
    const float cardMain = CardMain();
    const float lead = LeadGutter();

    // Card labels are centered under the thumbnail; long names ellipsize (the
    // format carries character-granularity trimming), full path on hover.
    if (m_textFormat)
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        Entry& e = m_entries[i];
        const float off = lead + static_cast<float>(i) * cardMain - m_scroll;

        D2D1_RECT_F card, thumbRect;
        if (m_horizontal)
        {
            const float left = r.left + off;
            if (left + cardMain < r.left || left > r.right)
                continue;
            card = { left, r.top + 1.0f, left + cardMain, r.bottom };
            thumbRect = { left + kPad, r.top + kPad, left + kPad + thumb, r.top + kPad + thumb };
        }
        else
        {
            const float top = r.top + off;
            if (top + cardMain < r.top || top > r.bottom)
                continue;
            card = { r.left + 2.0f, top, r.right - 2.0f, top + cardMain };
            thumbRect = { r.left + kPad, top + kPad, r.left + kPad + thumb, top + kPad + thumb };
        }

        if (static_cast<int>(i) == m_hoverCard &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.19f, 0.24f), &brush)))
            target->FillRectangle(card, brush.Get());

        if (e.kind != EntryKind::File)
        {
            // Folder / ".." navigation tile.
            DrawFolderIcon(target, thumbRect, e.kind == EntryKind::Up);
        }
        else
        {
            EnsureBitmap(e);
            if (e.bitmap)
            {
                target->DrawBitmap(e.bitmap.Get(), thumbRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            else
            {
                const D2D1_COLOR_F c = e.failed ? D2D1::ColorF(0.28f, 0.14f, 0.14f)
                                                : D2D1::ColorF(0.14f, 0.14f, 0.17f);
                if (SUCCEEDED(target->CreateSolidColorBrush(c, &brush)))
                    target->FillRectangle(thumbRect, brush.Get());
            }
            // Accent border on the active pane's current file.
            if (!m_currentFile.empty() && e.path == m_currentFile &&
                SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.62f, 0.96f), &brush)))
            {
                const D2D1_RECT_F b { thumbRect.left - 1.5f, thumbRect.top - 1.5f,
                                      thumbRect.right + 1.5f, thumbRect.bottom + 1.5f };
                target->DrawRectangle(b, brush.Get(), 2.5f);
            }
        }

        if (m_textFormat &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.82f, 0.84f, 0.88f), &brush)))
        {
            const D2D1_RECT_F lbl { thumbRect.left, thumbRect.bottom + 2.0f,
                                    thumbRect.right, thumbRect.bottom + 2.0f + kLabelH };
            target->DrawTextW(e.name.c_str(), static_cast<UINT32>(e.name.size()), m_textFormat.Get(),
                lbl, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    target->PopAxisAlignedClip();
}

int ThumbnailStrip::CardAtPoint(const POINT& pt) const
{
    const D2D1_RECT_F r = LayoutRect();
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    if (x < r.left || x > r.right || y < r.top || y > r.bottom)
        return -1;

    float local;
    if (m_horizontal)
        local = (x - r.left) - LeadGutter() + m_scroll;
    else
    {
        if (y < r.top + kHeaderH) return -1; // header row
        local = (y - r.top) - LeadGutter() + m_scroll;
    }
    if (local < 0.0f) return -1;
    const int idx = static_cast<int>(local / CardMain());
    return (idx >= 0 && idx < static_cast<int>(m_entries.size())) ? idx : -1;
}

bool ThumbnailStrip::OnInputEvent(const FD2D::InputEvent& event)
{
    using FD2D::InputEventType;
    using FD2D::MouseButton;

    const D2D1_RECT_F r = LayoutRect();
    auto inStrip = [&](const POINT& p) {
        return HasContent() && m_enabled && p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
    };

    switch (event.type)
    {
    case InputEventType::MouseWheel:
        if (event.hasPoint && inStrip(event.point))
        {
            // Wheel scrolls the strip's main axis (down/away = later items).
            m_scroll -= static_cast<float>(event.wheelDelta) / 120.0f * CardMain() * 0.5f;
            ClampScroll();
            Invalidate();
            return true;
        }
        break;

    case InputEventType::MouseMove:
    {
        if (!event.hasPoint) break;
        const int hit = inStrip(event.point) ? CardAtPoint(event.point) : -1;
        if (hit != m_hoverCard)
        {
            m_hoverCard = hit;
            Invalidate();
        }
        break;
    }

    case InputEventType::MouseDown:
        if (event.button == MouseButton::Left && event.hasPoint && inStrip(event.point))
        {
            const int hit = CardAtPoint(event.point);
            if (hit >= 0)
            {
                RequestFocus();
                const Entry& e = m_entries[static_cast<std::size_t>(hit)];
                if (e.kind == EntryKind::File)
                {
                    // Load the sibling into the active pane (owner's handler).
                    if (m_onActivated)
                        m_onActivated(e.path);
                }
                else
                {
                    // Folder / ".." tile: navigate the strip in place, keeping
                    // the current-file highlight (matches only if it reappears).
                    NavigateTo(e.path, m_currentFile);
                }
                return true;
            }
        }
        break;

    default:
        break;
    }

    return FD2D::Wnd::OnInputEvent(event);
}

} // namespace nsk
