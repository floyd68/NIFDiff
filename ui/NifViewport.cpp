#include "NifViewport.h"
#include <Backplate.h>
#include <Util.h>
#include <cmath>
#include <algorithm>
#include <filesystem>

namespace nsk
{

NifViewport::NifViewport(const std::wstring& name)
    : FD2D::Wnd(name)
{
    m_settings.showGrid = true;
    m_settings.showAxes = true;
}

void NifViewport::OnAttached(FD2D::Backplate& backplate)
{
    FD2D::Wnd::OnAttached(backplate);

    ID3D11Device* device = backplate.D3DDevice();
    ID3D11DeviceContext* context = backplate.D3DContext();
    if (device && context)
    {
        std::string err;
        if (m_renderer.Initialize(device, context, &err))
        {
            m_textures = std::make_unique<TextureCache>(device, m_resolver);
            if (!m_nifDirectory.empty())
                m_textures->SetNifDirectory(m_nifDirectory);
        }
    }
}

void NifViewport::SetResourceResolver(ResourceResolver* resolver)
{
    m_resolver = resolver;
    if (m_textures)
        m_textures->SetResolver(resolver);
}

void NifViewport::InvalidateTextureCache()
{
    if (m_textures)
        m_textures->Clear();
    Invalidate();
}

FD2D::Size NifViewport::Measure(FD2D::Size available)
{
    m_desired = available;
    return available;
}

void NifViewport::SetDocument(const NifDocument* doc)
{
    m_doc = doc;
    if (doc && !doc->filePath().empty())
    {
        std::filesystem::path p(doc->filePath());
        m_nifDirectory = p.parent_path().wstring();
        if (m_textures)
            m_textures->SetNifDirectory(m_nifDirectory);
    }
    RebuildScene();
    Invalidate();
}

void NifViewport::RebuildScene()
{
    m_meshes.clear();
    m_renderer.InvalidateMeshCache();
    if (!m_doc || !m_doc->isValid())
        return;

    m_meshes = SceneBuilder::build(*m_doc);

    // Fit the camera to the combined bounding sphere of every mesh, mirroring
    // GLView's "center on load" behaviour (glview.cpp's Scene::updateSceneOptions
    // + centerOn() path prior to the first frame).
    Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
    bool any = false;
    for (const RenderMesh& mesh : m_meshes)
    {
        if (!mesh.geometry) continue;
        for (const Vector3& p : mesh.geometry->positions)
        {
            Vector3 wp = mesh.worldTransform * p;
            minB.boundMin(wp);
            maxB.boundMax(wp);
            any = true;
        }
    }
    if (any)
    {
        Vector3 center = (minB + maxB) * 0.5f;
        float radius = (maxB - minB).length() * 0.5f;
        m_camera.frame(center, (std::max)(radius, 1.0f));
    }
    else
    {
        m_camera.frame(Vector3(), 50.0f);
    }
}

void NifViewport::ResetCamera()
{
    RebuildScene();
    Invalidate();
}

void NifViewport::SetLightDeclinationDegrees(float deg)
{
    m_lightDeclinationDeg = deg;
    UpdateFrontalLight();
}

void NifViewport::SetLightPlanarAngleDegrees(float deg)
{
    m_lightPlanarAngleDeg = deg;
    UpdateFrontalLight();
}

void NifViewport::UpdateFrontalLight()
{
    if (m_frontalLight)
    {
        // Light follows the camera (GLView's "Frontal Light" toggle).
        m_settings.lightDir = m_camera.forwardVector() * -1.0f;
    }
    else
    {
        float decl = m_lightDeclinationDeg * (NSK_PI / 180.0f);
        float planar = m_lightPlanarAngleDeg * (NSK_PI / 180.0f);
        m_settings.lightDir = Vector3(std::sin(planar) * std::cos(decl), std::sin(decl), std::cos(planar) * std::cos(decl));
    }
    Invalidate();
}

void NifViewport::OnRenderD3D(ID3D11DeviceContext* /*context*/)
{
    if (!m_renderer.IsInitialized())
        return;

    D2D1_RECT_F rect = LayoutRect();
    UINT w = static_cast<UINT>((std::max)(1.0f, rect.right - rect.left));
    UINT h = static_cast<UINT>((std::max)(1.0f, rect.bottom - rect.top));
    if (!m_renderer.Resize(w, h))
        return;

    if (m_frontalLight)
        UpdateFrontalLight();

    m_settings.view = m_camera.viewMatrix();
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    m_settings.proj = Camera::projectionMatrix(0.9f, aspect, 1.0f, 100000.0f);
    m_settings.eyePos = m_camera.eyePosition();

    m_renderer.RenderScene(m_meshes, m_settings, m_textures.get());
}

void NifViewport::EnsureD2DTarget()
{
    if (!m_backplate || !m_renderer.ColorTexture())
        return;
    UINT w = m_renderer.Width();
    UINT h = m_renderer.Height();
    if (m_d2dBitmap && w == m_d2dBitmapWidth && h == m_d2dBitmapHeight)
        return;

    ID2D1RenderTarget* rt = m_backplate->RenderTarget();
    if (!rt)
        return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))) || !dc)
        return;

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(m_renderer.ColorTexture()->QueryInterface(IID_PPV_ARGS(&surface))))
        return;

    D2D1_BITMAP_PROPERTIES1 bp {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE; // the 3D pass fully overwrites every pixel each frame
    dc->GetDpi(&bp.dpiX, &bp.dpiY);

    m_d2dBitmap.Reset();
    if (SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &m_d2dBitmap)))
    {
        m_d2dBitmapWidth = w;
        m_d2dBitmapHeight = h;
    }
}

void NifViewport::OnRender(ID2D1RenderTarget* target)
{
    if (!target)
        return;
    EnsureD2DTarget();
    if (!m_d2dBitmap)
        return;

    D2D1_RECT_F destRect = LayoutRect();
    target->DrawBitmap(m_d2dBitmap.Get(), destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

bool NifViewport::OnInputEvent(const FD2D::InputEvent& event)
{
    using FD2D::InputEventType;
    using FD2D::MouseButton;

    switch (event.type)
    {
    case InputEventType::MouseDown:
        // Wnd::OnInputEvent's default dispatch does not hit-test children for
        // MouseDown/MouseMove/MouseUp (only MouseWheel gets that treatment -
        // see Wnd.cpp), so without this check whichever viewport happens to
        // be visited first in the parent SplitPanel's child order would
        // unconditionally grab every MouseDown in the window, including
        // clicks that actually landed on a Splitter's drag handle.
        if (!event.hasPoint || !FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            break;
        if (event.button == MouseButton::Left) { m_dragging = true; m_lastMousePt = event.point; RequestFocus(); return true; }
        if (event.button == MouseButton::Middle || event.button == MouseButton::Right) { m_panning = true; m_lastMousePt = event.point; RequestFocus(); return true; }
        break;

    case InputEventType::MouseUp:
        // Only claim the event if this viewport was actually the one tracking
        // the drag/pan; otherwise fall through so it doesn't swallow mouse-up
        // events that belong to some other control (e.g. releasing a Splitter drag).
        if (event.button == MouseButton::Left && m_dragging) { m_dragging = false; return true; }
        if ((event.button == MouseButton::Middle || event.button == MouseButton::Right) && m_panning) { m_panning = false; return true; }
        break;

    case InputEventType::MouseMove:
    {
        if (!event.hasPoint) break;
        float dx = static_cast<float>(event.point.x - m_lastMousePt.x);
        float dy = static_cast<float>(event.point.y - m_lastMousePt.y);
        if (m_dragging)
        {
            m_camera.orbit(dx * 0.01f, dy * 0.01f);
            m_lastMousePt = event.point;
            if (m_onCameraChanged) m_onCameraChanged(m_camera);
            Invalidate();
            return true;
        }
        if (m_panning)
        {
            float scale = m_camera.distance() * 0.0015f;
            m_camera.pan(-dx * scale, dy * scale);
            m_lastMousePt = event.point;
            if (m_onCameraChanged) m_onCameraChanged(m_camera);
            Invalidate();
            return true;
        }
        break;
    }

    case InputEventType::MouseWheel:
    {
        float delta = static_cast<float>(event.wheelDelta) / 120.0f;
        m_camera.dolly(-delta * m_camera.distance() * 0.1f);
        if (m_onCameraChanged) m_onCameraChanged(m_camera);
        Invalidate();
        return true;
    }

    default:
        break;
    }

    return FD2D::Wnd::OnInputEvent(event);
}

} // namespace nsk
