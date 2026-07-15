#include "NifViewport.h"
#include "../core/StartupTrace.h"
#include <Backplate.h>
#include <Util.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

namespace nsk
{

namespace
{
    // Vertical field of view of the perspective projection - shared between
    // the render pass (OnRenderD3D's projectionMatrix call) and the pick
    // ray construction (PickMeshAt), which must agree exactly for the ray
    // to pass through the clicked pixel.
    constexpr float kFovY = 0.9f;

    // Moller-Trumbore, both-sided (picking ignores backface culling, like
    // NifSkope's selection). Returns the ray parameter in outT on hit.
    bool RayIntersectsTriangle(
        const Vector3& origin, const Vector3& dir,
        const Vector3& v0, const Vector3& v1, const Vector3& v2,
        float& outT)
    {
        constexpr float kEps = 1e-7f;
        const Vector3 e1 = v1 - v0;
        const Vector3 e2 = v2 - v0;
        const Vector3 p = Vector3::crossproduct(dir, e2);
        const float det = Vector3::dotproduct(e1, p);
        if (std::abs(det) < kEps)
            return false; // parallel to the triangle plane
        const float invDet = 1.0f / det;
        const Vector3 s = origin - v0;
        const float u = Vector3::dotproduct(s, p) * invDet;
        if (u < 0.0f || u > 1.0f)
            return false;
        const Vector3 q = Vector3::crossproduct(s, e1);
        const float v = Vector3::dotproduct(dir, q) * invDet;
        if (v < 0.0f || u + v > 1.0f)
            return false;
        const float t = Vector3::dotproduct(e2, q) * invDet;
        if (t <= 0.0f)
            return false;
        outT = t;
        return true;
    }
}

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
    m_device = device;
    m_context = context;
    if (device && context && m_renderDevice)
    {
        StartupTrace::Phase p("  Viewport renderer init (shaders)");
        std::string err;
        // Idempotent: only the first attached viewport actually builds the
        // shared shaders/states/IBL; the rest reuse them.
        if (m_renderDevice->EnsureInitialized(device, context, &err) && m_textureRepository != nullptr)
        {
            m_textureRepository->SetDevice(device);
            m_textures = std::make_unique<TextureCache>(m_textureRepository);
            if (!m_nifDirectory.empty())
                m_textures->SetNifDirectory(m_nifDirectory);
        }
    }
}

void NifViewport::SetResourceResolver(ResourceResolver* resolver)
{
    m_resolver = resolver;
}

void NifViewport::SetTextureRepository(TextureRepository* repository)
{
    m_textureRepository = repository;
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
    SetSelectedMesh(-1); // a new (or cleared) document invalidates any picked mesh
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
    // Drop this view's cached geometry buffers before the old document's
    // geometries are freed (the cache only holds their pointers as keys).
    m_meshCache.Clear();
    if (!m_doc || !m_doc->isValid())
        return;

    {
        StartupTrace::Phase p("    SceneBuilder::build");
        m_meshes = SceneBuilder::build(*m_doc, m_showHiddenNodes);
    }

    // Front-load every texture the scene references through the pool's
    // parallel prefetch: the first frame's per-draw GetOrLoad calls then
    // hit the pool instead of serially reading+uploading ~8ms per texture
    // (measured 414ms -> parallel for a 26-shape PBR exterior).
    if (m_textures)
    {
        StartupTrace::Phase p("    Texture prefetch (parallel)");
        std::vector<std::string> paths;
        paths.reserve(m_meshes.size() * 4);
        for (const RenderMesh& mesh : m_meshes)
        {
            const NifMaterial& m = mesh.material;
            for (const std::string* tex : { &m.diffuseTexture, &m.normalTexture, &m.glowTexture,
                                            &m.heightTexture, &m.cubeTexture, &m.envMaskTexture,
                                            &m.innerTexture, &m.backlightTexture, &m.greyscaleTexture })
            {
                if (!tex->empty())
                    paths.push_back(*tex);
            }
        }
        m_textures->Prefetch(paths);
    }

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
        // Frame the model's world-space bbox: orbit target at its center,
        // distance proportional to its bounding radius, and the default
        // slightly-elevated orbit so a fresh load (or Center/Reset View)
        // always lands on the comfortable looking-down-at-the-model view
        // rather than whatever angles the camera was left at.
        m_sceneCenter = (minB + maxB) * 0.5f;
        m_sceneRadius = (std::max)((maxB - minB).length() * 0.5f, 1.0f);
        m_camera.frame(m_sceneCenter, m_sceneRadius);
        m_camera.setOrbit(Camera::kDefaultYaw, Camera::kDefaultPitch);
    }
    else
    {
        // Empty pane: same elevated resting view aimed at the grid origin -
        // a level (or upward-looking) camera at ground height renders the
        // grid edge-on/from below, which reads as a broken viewport.
        m_sceneCenter = Vector3();
        m_sceneRadius = 50.0f;
        m_camera.frame(Vector3(), 50.0f);
        m_camera.setOrbit(Camera::kDefaultYaw, Camera::kDefaultPitch);
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
    if (!m_renderDevice || !m_renderDevice->IsInitialized() || !m_device)
        return;

    D2D1_RECT_F rect = LayoutRect();
    UINT w = static_cast<UINT>((std::max)(1.0f, rect.right - rect.left));
    UINT h = static_cast<UINT>((std::max)(1.0f, rect.bottom - rect.top));
    if (!m_target.Resize(m_device, w, h))
        return;

    if (m_frontalLight)
        UpdateFrontalLight();

    m_settings.view = m_camera.viewMatrix();
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    // Adaptive clip planes (the old fixed 1..100000 pair clipped tiny
    // models when leaning in and huge exteriors at their far side): the
    // near plane pulls in as the eye approaches the scene sphere - down to
    // a relative floor so silverware-sized meshes survive close
    // inspection - and the far plane always covers the scene plus the
    // fixed 200-unit grid.
    const float sceneR = (std::max)(m_sceneRadius, 1.0f);
    const float dist = m_camera.distance();
    const float nearZ = std::clamp((dist - sceneR) * 0.2f,
                                   (std::max)(sceneR, dist) * 1e-4f,
                                   dist * 0.25f);
    const float farZ = (std::max)(dist + (std::max)(sceneR * 4.0f, 600.0f), nearZ * 100.0f);
    m_settings.proj = Camera::projectionMatrix(kFovY, aspect, nearZ, farZ);
    m_settings.eyePos = m_camera.eyePosition();
    m_settings.selectedMesh = m_selectedMesh;

    // One-shot: the very first 3D scene render in the process is where the
    // lazy texture loads land, so it is a startup phase in its own right.
    static bool s_firstSceneRendered = false;
    if (!s_firstSceneRendered && !m_meshes.empty())
    {
        s_firstSceneRendered = true;
        StartupTrace::Phase p("First 3D frame (incl. texture loads)");
        m_renderDevice->RenderScene(m_target, m_meshCache, m_meshes, m_settings, m_textures.get());
    }
    else
    {
        m_renderDevice->RenderScene(m_target, m_meshCache, m_meshes, m_settings, m_textures.get());
    }
}

void NifViewport::EnsureD2DTarget()
{
    if (!m_backplate || !m_target.ColorTexture())
        return;
    UINT w = m_target.Width();
    UINT h = m_target.Height();
    if (m_d2dBitmap && w == m_d2dBitmapWidth && h == m_d2dBitmapHeight)
        return;

    ID2D1RenderTarget* rt = m_backplate->RenderTarget();
    if (!rt)
        return;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))) || !dc)
        return;

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(m_target.ColorTexture()->QueryInterface(IID_PPV_ARGS(&surface))))
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
        if (event.button == MouseButton::Left) { m_dragging = true; m_dragMoved = false; m_dragDownPt = event.point; m_lastMousePt = event.point; RequestFocus(); return true; }
        if (event.button == MouseButton::Middle || event.button == MouseButton::Right) { m_panning = true; m_panMoved = false; m_panDownPt = event.point; m_lastMousePt = event.point; RequestFocus(); return true; }
        break;

    case InputEventType::MouseUp:
        // Only claim the event if this viewport was actually the one tracking
        // the drag/pan; otherwise fall through so it doesn't swallow mouse-up
        // events that belong to some other control (e.g. releasing a Splitter drag).
        if (event.button == MouseButton::Left && m_dragging)
        {
            m_dragging = false;
            // A left click that never crossed the drag-jitter threshold is a
            // pick, not an orbit: select the sub-mesh under the cursor (or
            // clear the selection when the click lands on empty space).
            if (!m_dragMoved && event.hasPoint)
                SetSelectedMesh(PickMeshAt(event.point));
            return true;
        }
        if ((event.button == MouseButton::Middle || event.button == MouseButton::Right) && m_panning)
        {
            m_panning = false;
            // A right-click that never moved past the click-jitter threshold
            // is not a pan: leave it unhandled so it bubbles up to
            // NifCompareView, which opens the app context menu.
            if (event.button == MouseButton::Right && !m_panMoved)
                break;
            return true;
        }
        break;

    case InputEventType::MouseMove:
    {
        if (!event.hasPoint) break;
        float dx = static_cast<float>(event.point.x - m_lastMousePt.x);
        float dy = static_cast<float>(event.point.y - m_lastMousePt.y);
        if (m_dragging)
        {
            // Same click-jitter threshold as panning: the orbit only starts
            // once the pointer really moves, so a slightly shaky click still
            // counts as a pick on MouseUp instead of a micro-rotation.
            if (!m_dragMoved &&
                (std::abs(event.point.x - m_dragDownPt.x) > 3 || std::abs(event.point.y - m_dragDownPt.y) > 3))
            {
                m_dragMoved = true;
            }
            if (m_dragMoved)
            {
                m_camera.orbit(dx * 0.01f, dy * 0.01f);
                if (m_onCameraChanged) m_onCameraChanged(m_camera);
                Invalidate();
            }
            m_lastMousePt = event.point;
            return true;
        }
        if (m_panning)
        {
            if (!m_panMoved &&
                (std::abs(event.point.x - m_panDownPt.x) > 3 || std::abs(event.point.y - m_panDownPt.y) > 3))
            {
                m_panMoved = true;
            }
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
        // Zoom toward the cursor, not the orbit target: the world point
        // under the cursor (at target depth) stays put while the distance
        // scales, so content sitting off the bbox center can be zoomed at
        // directly instead of sliding out of view. Exponential steps give
        // the same per-notch feel as the old linear dolly at distance.
        const float delta = static_cast<float>(event.wheelDelta) / 120.0f;
        const float d = m_camera.distance();
        const float dNew = (std::max)(d * std::pow(0.9f, delta), 0.01f);

        Vector3 origin, dir;
        if (event.hasPoint && RayThroughPoint(event.point, origin, dir))
        {
            Vector3 fwd = m_camera.forwardVector();
            fwd.normalize();
            const float denom = Vector3::dotproduct(dir, fwd);
            if (denom > 1e-4f)
            {
                const Vector3 pivot = origin + dir * (d / denom);
                const Vector3 t = m_camera.target();
                m_camera.setTarget(pivot + (t - pivot) * (dNew / d));
            }
        }
        m_camera.setDistance(dNew);
        if (m_onCameraChanged) m_onCameraChanged(m_camera);
        Invalidate();
        return true;
    }

    case InputEventType::MouseDoubleClick:
    {
        if (event.button != MouseButton::Left || !event.hasPoint ||
            !FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            break;
        // Double-click focuses: on a mesh it selects + frames that mesh,
        // on empty space it re-frames the whole scene (keeping the orbit).
        const int hit = PickMeshAt(event.point);
        if (hit >= 0)
            SetSelectedMesh(hit);
        FocusOnSelection();
        return true;
    }

    default:
        break;
    }

    return FD2D::Wnd::OnInputEvent(event);
}

std::wstring NifViewport::ShaderKindFor(const RenderMesh& mesh) const
{
    const NifMaterial& m = mesh.material;

    std::wstring kind;
    if (m.isPBR)
    {
        kind = m.pbrSubsurface ? L"True PBR (SSS)" : L"True PBR";
    }
    else if (m.isEffectShader)
    {
        kind = L"Effect";
    }
    else if (m.hasEnvironmentMap && !m.envMaskTexture.empty() && m_textures
             && m_textures->HasComplexMaterialAlpha(m.envMaskTexture))
    {
        kind = L"Complex Material";
    }
    else
    {
        switch (m.shaderType)
        {
        case 0:  kind = L"Default"; break;
        case 1:  kind = L"EnvMap"; break;
        case 2:  kind = L"Glow"; break;
        case 3:  kind = L"Parallax"; break;
        case 4:  kind = L"Face Tint"; break;
        case 5:  kind = L"Skin Tint"; break;
        case 6:  kind = L"Hair Tint"; break;
        case 11: kind = L"MultiLayer"; break;
        case 14: kind = L"Sparkle"; break;
        case 16: kind = L"Eye EnvMap"; break;
        default: kind = L"Type " + std::to_wstring(m.shaderType); break;
        }
    }

    if (m.hasModelSpaceNormals)
        kind += L" · MSN";
    if (m.isDecal)
        kind += L" · Decal";
    if (m.hasRefraction)
        kind += L" · Refraction";
    return kind;
}

std::wstring NifViewport::SelectedMeshShaderKind() const
{
    const RenderMesh* sel = SelectedMesh();
    return sel ? ShaderKindFor(*sel) : std::wstring();
}

std::wstring NifViewport::ShaderKindSummary() const
{
    // Aggregate by label, keeping first-appearance order so the summary
    // reads in scene order.
    std::vector<std::pair<std::wstring, int>> counts;
    for (const RenderMesh& mesh : m_meshes)
    {
        const std::wstring kind = ShaderKindFor(mesh);
        auto it = std::find_if(counts.begin(), counts.end(),
            [&](const auto& p) { return p.first == kind; });
        if (it != counts.end())
            ++it->second;
        else
            counts.emplace_back(kind, 1);
    }

    std::wstring out;
    for (const auto& [kind, n] : counts)
    {
        if (!out.empty())
            out += L"  ·  ";
        out += kind;
        if (n > 1)
            out += L" ×" + std::to_wstring(n);
    }
    return out;
}

bool NifViewport::HasActiveParallax()
{
    for (const RenderMesh& mesh : m_meshes)
    {
        const NifMaterial& m = mesh.material;
        if (m.isPBR)
            continue; // authored displacement_scale, not slider-driven
        // The material must WANT parallax and its _p must actually resolve:
        // parallax-edition mod meshes routinely ship ST_Heightmap materials
        // whose height texture is absent from the install (the renderer
        // already degrades those to no-POM), so path presence alone would
        // light the slider/toggle for a control that changes nothing.
        if (m.hasHeightMap && m_textures && m_textures->GetOrLoad(m.heightTexture) != nullptr)
            return true;
        if (m.hasEnvironmentMap && !m.envMaskTexture.empty() && m_textures
            && m_textures->HasComplexMaterialAlpha(m.envMaskTexture)
            && m_textures->HasComplexMaterialHeight(m.envMaskTexture))
            return true;
    }
    return false;
}

bool NifViewport::HasParallaxMaterials()
{
    if (HasActiveParallax())
        return true;
    for (const RenderMesh& mesh : m_meshes)
    {
        // Same resolution requirement as HasActiveParallax above.
        if (mesh.material.isPBR && !mesh.material.heightTexture.empty()
            && m_textures && m_textures->GetOrLoad(mesh.material.heightTexture) != nullptr)
            return true;
    }
    return false;
}

bool NifViewport::HasComplexMaterials()
{
    for (const RenderMesh& mesh : m_meshes)
    {
        const NifMaterial& m = mesh.material;
        if (!m.isPBR && m.hasEnvironmentMap && !m.envMaskTexture.empty() && m_textures
            && m_textures->HasComplexMaterialAlpha(m.envMaskTexture))
            return true;
    }
    return false;
}

bool NifViewport::HasPBRMaterials() const
{
    for (const RenderMesh& mesh : m_meshes)
    {
        if (mesh.material.isPBR)
            return true;
    }
    return false;
}

std::size_t NifViewport::TotalTriangleCount() const
{
    std::size_t total = 0;
    for (const RenderMesh& mesh : m_meshes)
    {
        if (mesh.geometry)
            total += mesh.geometry->triangles.size();
    }
    return total;
}

const RenderMesh* NifViewport::SelectedMesh() const
{
    if (m_selectedMesh < 0 || static_cast<std::size_t>(m_selectedMesh) >= m_meshes.size())
        return nullptr;
    return &m_meshes[static_cast<std::size_t>(m_selectedMesh)];
}

bool NifViewport::RayThroughPoint(POINT pt, Vector3& outOrigin, Vector3& outDir) const
{
    const D2D1_RECT_F rect = LayoutRect();
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 0.0f || h <= 0.0f)
        return false;

    // Build the world-space ray through the pixel from the same camera
    // basis + projection parameters OnRenderD3D renders with, so no matrix
    // inversion is needed: in view space the pixel maps to direction
    // (ndcX * tan(fov/2) * aspect, ndcY * tan(fov/2), 1).
    const float ndcX = 2.0f * (static_cast<float>(pt.x) - rect.left) / w - 1.0f;
    const float ndcY = 1.0f - 2.0f * (static_cast<float>(pt.y) - rect.top) / h;
    const float tanHalf = std::tan(kFovY * 0.5f);
    const float aspect = w / h;

    Vector3 forward = m_camera.forwardVector();
    forward.normalize();
    Vector3 worldUp(0.0f, 1.0f, 0.0f);
    Vector3 right = Vector3::crossproduct(worldUp, forward);
    if (right.squaredLength() < 1e-8f)
        right = Vector3(1.0f, 0.0f, 0.0f);
    right.normalize();
    Vector3 up = Vector3::crossproduct(forward, right);
    up.normalize();

    outOrigin = m_camera.eyePosition();
    outDir = right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf) + forward;
    outDir.normalize();
    return true;
}

void NifViewport::FocusOnSelection()
{
    Vector3 center = m_sceneCenter;
    float radius = (std::max)(m_sceneRadius, 1.0f);

    if (const RenderMesh* sel = SelectedMesh())
    {
        Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
        bool any = false;
        if (sel->geometry)
        {
            for (const Vector3& p : sel->geometry->positions)
            {
                Vector3 wp = sel->worldTransform * p;
                minB.boundMin(wp);
                maxB.boundMax(wp);
                any = true;
            }
        }
        if (any)
        {
            center = (minB + maxB) * 0.5f;
            radius = (std::max)((maxB - minB).length() * 0.5f, 0.05f);
        }
    }

    m_camera.frame(center, radius); // keeps the current yaw/pitch
    if (m_onCameraChanged) m_onCameraChanged(m_camera);
    Invalidate();
}

void NifViewport::SetShowHiddenNodes(bool show)
{
    if (m_showHiddenNodes == show)
        return;
    m_showHiddenNodes = show;
    m_selectedMesh = -1; // the mesh list is about to be rebuilt
    // RebuildScene refits the camera to the (now different) scene bounds -
    // right for a fresh load, jarring for a display toggle. Keep the view
    // where the user left it; hidden markers usually sit inside the model's
    // frame anyway (NifSkope's Show Hidden keeps the camera too).
    const Camera keep = m_camera;
    RebuildScene();
    m_camera = keep;
    // The mesh set changed (triangle counts, shader-kind summary), so the
    // owner's labels must refresh even when there was no selection to clear
    // - fire the selection handler unconditionally (it is idempotent).
    if (m_onSelectionChanged)
        m_onSelectionChanged(nullptr);
    Invalidate();
}

void NifViewport::SetSelectedMesh(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_meshes.size())
        index = -1;
    if (index == m_selectedMesh)
        return;
    m_selectedMesh = index;
    if (m_onSelectionChanged)
        m_onSelectionChanged(SelectedMesh());
    Invalidate();
}

int NifViewport::PickMeshAt(POINT pt) const
{
    Vector3 origin, dir;
    if (m_meshes.empty() || !RayThroughPoint(pt, origin, dir))
        return -1;

    // Nearest hit across every mesh's triangles, in world space (the CPU
    // triangle counts of NIF shapes are small enough that a brute-force
    // test per click is instantaneous).
    int best = -1;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < m_meshes.size(); ++i)
    {
        const RenderMesh& mesh = m_meshes[i];
        if (!mesh.geometry)
            continue;
        const std::vector<Vector3>& pos = mesh.geometry->positions;
        for (const Triangle& tri : mesh.geometry->triangles)
        {
            if (tri[0] >= pos.size() || tri[1] >= pos.size() || tri[2] >= pos.size())
                continue;
            const Vector3 v0 = mesh.worldTransform * pos[tri[0]];
            const Vector3 v1 = mesh.worldTransform * pos[tri[1]];
            const Vector3 v2 = mesh.worldTransform * pos[tri[2]];
            float t = 0.0f;
            if (RayIntersectsTriangle(origin, dir, v0, v1, v2, t) && t < bestT)
            {
                bestT = t;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

} // namespace nsk
