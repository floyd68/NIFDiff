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
    constexpr float kStripWidth = 184.0f;
    constexpr float kPad = 8.0f;
    constexpr float kThumb = kStripWidth - kPad * 2.0f; // square thumbnail
    constexpr float kLabelH = 20.0f;
    constexpr float kCardH = kThumb + kLabelH + kPad * 2.0f;
    constexpr float kHeaderH = 26.0f;
    constexpr UINT kThumbPx = 168;         // offscreen render resolution
    constexpr int kPerFrame = 2;           // thumbnails generated per frame
    constexpr float kFovY = 0.9f;
}

ThumbnailStrip::ThumbnailStrip(const std::wstring& name)
    : FD2D::Wnd(name)
{
}

void ThumbnailStrip::OnAttached(FD2D::Backplate& backplate)
{
    FD2D::Wnd::OnAttached(backplate);
    m_device = backplate.D3DDevice();
    m_context = backplate.D3DContext();
}

void ThumbnailStrip::SetFolder(const std::wstring& folder)
{
    m_entries.clear();
    m_nextToRender = 0;
    m_scrollY = 0.0f;
    m_hoverCard = -1;
    m_folder = folder;
    m_thumbCache.Clear();

    if (!folder.empty())
    {
        std::error_code ec;
        std::filesystem::path dir(folder);
        if (std::filesystem::is_directory(dir, ec))
        {
            for (const auto& de : std::filesystem::directory_iterator(dir, ec))
            {
                if (ec) break;
                if (!de.is_regular_file(ec)) continue;
                std::wstring ext = de.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (ext != L".nif") continue;
                Entry e;
                e.path = de.path().wstring();
                e.name = de.path().filename().wstring();
                m_entries.push_back(std::move(e));
            }
        }
        std::sort(m_entries.begin(), m_entries.end(),
                  [](const Entry& a, const Entry& b) { return a.name < b.name; });
    }

    // Width changes (0 <-> kStripWidth) with content presence, so relayout.
    if (FD2D::Backplate* bp = BackplateRef())
        bp->RequestLayout();
    Invalidate();
}

FD2D::Size ThumbnailStrip::Measure(FD2D::Size available)
{
    const float w = HasContent() ? kStripWidth : 0.0f;
    m_desired = { w, available.h };
    return m_desired;
}

bool ThumbnailStrip::RenderNextThumbnail()
{
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

float ThumbnailStrip::ContentHeight() const
{
    return kHeaderH + static_cast<float>(m_entries.size()) * kCardH + kPad;
}

void ThumbnailStrip::ClampScroll()
{
    const D2D1_RECT_F r = LayoutRect();
    const float viewH = r.bottom - r.top;
    const float maxScroll = (std::max)(0.0f, ContentHeight() - viewH);
    m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);
}

void ThumbnailStrip::OnRender(ID2D1RenderTarget* target)
{
    if (!target || !HasContent())
        return;
    EnsureTextFormat();

    const D2D1_RECT_F r = LayoutRect();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;

    // Strip background + right hairline separator.
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.12f), &brush)))
        target->FillRectangle(r, brush.Get());
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.29f, 0.33f), &brush)))
        target->FillRectangle(D2D1::RectF(r.right - 1.0f, r.top, r.right, r.bottom), brush.Get());

    target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);

    // Header: "<count> models".
    if (m_textFormat && SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.66f, 0.72f), &brush)))
    {
        const std::wstring hdr = std::to_wstring(m_entries.size()) + L" model" +
                                 (m_entries.size() == 1 ? L"" : L"s");
        target->DrawTextW(hdr.c_str(), static_cast<UINT32>(hdr.size()), m_textFormat.Get(),
            D2D1::RectF(r.left + kPad, r.top + 6.0f, r.right - kPad, r.top + kHeaderH),
            brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    for (std::size_t i = 0; i < m_entries.size(); ++i)
    {
        Entry& e = m_entries[i];
        const float top = r.top + kHeaderH + static_cast<float>(i) * kCardH - m_scrollY;
        if (top + kCardH < r.top || top > r.bottom)
            continue; // off-screen

        const D2D1_RECT_F card { r.left + 2.0f, top, r.right - 2.0f, top + kCardH };
        const D2D1_RECT_F thumb { r.left + kPad, top + kPad, r.left + kPad + kThumb, top + kPad + kThumb };

        if (static_cast<int>(i) == m_hoverCard &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.19f, 0.24f), &brush)))
            target->FillRectangle(card, brush.Get());

        EnsureBitmap(e);
        if (e.bitmap)
        {
            target->DrawBitmap(e.bitmap.Get(), thumb, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            // Placeholder box: dark for pending, reddish for a failed load.
            const D2D1_COLOR_F c = e.failed ? D2D1::ColorF(0.28f, 0.14f, 0.14f)
                                            : D2D1::ColorF(0.14f, 0.14f, 0.17f);
            if (SUCCEEDED(target->CreateSolidColorBrush(c, &brush)))
                target->FillRectangle(thumb, brush.Get());
        }

        if (m_textFormat &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.82f, 0.84f, 0.88f), &brush)))
        {
            const D2D1_RECT_F lbl { thumb.left, thumb.bottom + 2.0f, thumb.right, thumb.bottom + 2.0f + kLabelH };
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
    if (x < r.left || x > r.right || y < r.top + kHeaderH || y > r.bottom)
        return -1;
    const float local = y - (r.top + kHeaderH) + m_scrollY;
    if (local < 0.0f) return -1;
    const int idx = static_cast<int>(local / kCardH);
    return (idx >= 0 && idx < static_cast<int>(m_entries.size())) ? idx : -1;
}

bool ThumbnailStrip::OnInputEvent(const FD2D::InputEvent& event)
{
    using FD2D::InputEventType;
    using FD2D::MouseButton;

    const D2D1_RECT_F r = LayoutRect();
    auto inStrip = [&](const POINT& p) {
        return HasContent() && p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
    };

    switch (event.type)
    {
    case InputEventType::MouseWheel:
        if (event.hasPoint && inStrip(event.point))
        {
            m_scrollY -= static_cast<float>(event.wheelDelta) / 120.0f * kCardH * 0.5f;
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
                if (m_onActivated)
                    m_onActivated(m_entries[static_cast<std::size_t>(hit)].path);
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
