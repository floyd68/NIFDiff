#include "NifViewport.h"
#include "../core/StartupTrace.h"
#include <Backplate.h>
#include <Core.h> // FD2D::Core::DWriteFactory (Loading overlay text)
#include <Util.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

namespace nsk
{

namespace
{
    // Close-up navigation floor: once the eye is within this fraction of the
    // focus radius, panning stops using the (now near-zero) raw distance and
    // holds at focusRadius * this, so a drag keeps moving a usable amount. 0.2
    // means "a drag near the floor pans ~20% of the focus radius" - enough to
    // reposition, small enough that a few drags cross the focused object.
    constexpr float kNavCloseFloorFrac = 0.2f;

    // Minimum zoom step at extreme close-ups, as a fraction of the focus radius
    // (times the notch/pixel count). The exponential step wins at normal range;
    // this only takes over once the eye is close enough that a distance-
    // proportional step would collapse. Per wheel notch vs per dolly pixel.
    constexpr float kWheelCloseFloorFrac = 0.05f;
    constexpr float kDollyCloseFloorFrac = 0.004f;

    // Fly-through bound: the dolly-through-pivot advance can push the orbit
    // target forward at most this many scene radii from the scene center, so
    // runaway input can't send coordinates to infinity (numerically unsafe) or
    // lose the model beyond any recovery.
    constexpr float kFlyThroughLimit = 3.0f;

    // Rotate v about the world +Y axis by `a`, matching Camera::forwardVector's
    // yaw convention (yaw 0 = +Z, increasing yaw turns toward +X).
    Vector3 RotateAboutY(const Vector3& v, float a)
    {
        const float c = std::cos(a), s = std::sin(a);
        return Vector3(v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c);
    }

    // Rodrigues rotation of v about unit axis k by angle a.
    Vector3 RotateAboutAxis(const Vector3& v, const Vector3& k, float a)
    {
        const float c = std::cos(a), s = std::sin(a);
        const Vector3 kxv = Vector3::crossproduct(k, v);
        const float kdotv = Vector3::dotproduct(k, v);
        return v * c + kxv * s + k * (kdotv * (1.0f - c));
    }

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
    const FD2D::GraphicsGeneration generation = backplate.GetGraphicsGeneration();
    m_boundDeviceGeneration = generation.device;
    m_boundTargetGeneration = generation.target;
    m_device = device;
    m_context = context;
    if (device && context && m_renderDevice)
    {
        StartupTrace::Phase p("  Viewport renderer init (shaders)");
        std::string err;
        // Idempotent: only the first attached viewport actually builds the
        // shared shaders/states/IBL; the rest reuse them.
        if (m_renderDevice->EnsureInitialized(device, context, generation.device, &err) &&
            m_textureRepository != nullptr)
        {
            m_textureRepository->BindDevice(device, generation.device);
            m_textures = std::make_unique<TextureCache>(m_textureRepository);
            if (!m_nifDirectory.empty())
                m_textures->SetNifDirectory(m_nifDirectory);
        }
    }
}

void NifViewport::OnGraphicsInvalidated(
    FD2D::GraphicsInvalidationReason reason,
    const FD2D::GraphicsGeneration& generation)
{
    const bool targetChanged = generation.target != m_boundTargetGeneration;
    const bool deviceChanged = generation.device != m_boundDeviceGeneration;

    if (targetChanged || deviceChanged)
    {
        m_d2dBitmap.Reset();
        m_d2dBitmapWidth = 0;
        m_d2dBitmapHeight = 0;
        m_d2dBitmapTex = nullptr;
    }
    if (deviceChanged)
    {
        m_target = {};
        m_meshCache.Clear();
        m_textures.reset();
        m_device = nullptr;
        m_context = nullptr;
    }

    m_boundDeviceGeneration = generation.device;
    m_boundTargetGeneration = generation.target;
    FD2D::Wnd::OnGraphicsInvalidated(reason, generation);
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

    FinishSceneLoad();
}

void NifViewport::SetPrebuiltScene(const NifDocument* doc, std::vector<RenderMesh> meshes)
{
    // The parse + SceneBuilder::build already ran on the load pool; here we
    // only swap in the result and do the UI-thread post-build setup.
    SetSelectedMesh(-1); // a new document invalidates any picked mesh
    m_doc = doc;
    if (doc && !doc->filePath().empty())
    {
        std::filesystem::path p(doc->filePath());
        m_nifDirectory = p.parent_path().wstring();
        if (m_textures)
            m_textures->SetNifDirectory(m_nifDirectory);
    }
    m_meshCache.Clear(); // old geometries about to be dropped
    m_meshes = std::move(meshes);
    FinishSceneLoad();
    Invalidate();
}

void NifViewport::PrefetchSceneTextures()
{
    // Front-load every texture the scene references through the pool's async
    // prefetch: decode + upload (~8ms each, measured 414ms for a 26-shape PBR
    // exterior) happen on the shared pool instead of blocking this UI-thread
    // load. Meshes render untextured for the first frame(s) and each texture
    // pops in as its decode completes (see TextureRepository::PrefetchAsync).
    if (!m_textures)
        return;
    StartupTrace::Phase p("    Texture prefetch (submit async)");
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
    m_textures->PrefetchAsync(paths);
}

void NifViewport::RefreshTextures()
{
    // The archive scan just landed: drop this view's texture memo (which cached
    // "not resolved yet" for BSA paths while the scan ran) and re-prefetch, so
    // archive-backed textures resolve and pop into the already-shown model.
    if (m_textures)
        m_textures->Clear();
    PrefetchSceneTextures();
    Invalidate();
}

void NifViewport::FrameCameraToScene()
{
    // Fit the camera to the combined bounding sphere of every mesh, mirroring
    // GLView's "center on load" behaviour (glview.cpp's Scene::updateSceneOptions
    // + centerOn() path prior to the first frame). Depends only on geometry, so
    // it is done FIRST (before any texture work) - the model is framed correctly
    // the instant it first draws, untextured, rather than jumping once textures
    // finish loading.
    m_camAnimating = false; // a fresh frame (load / Reset View) supersedes any tween
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

void NifViewport::FinishSceneLoad()
{
    // Frame FIRST (geometry-only), then submit the async texture prefetch: the
    // camera is positioned before the first (untextured) draw, so it never
    // shifts when the textures land.
    FrameCameraToScene();
    PrefetchSceneTextures();

    // Bind any NIF-embedded transform animations (controller managers /
    // standalone controllers) and rest at the range start, paused. The owner's
    // ANIMATION controls decide when to play.
    m_animPlaying = false;
    m_animSpeed = 1.0f;
    m_animLoop = true;
    if (m_doc != nullptr && m_doc->isValid())
    {
        m_animPlayer.bind(*m_doc);
        m_animTime = m_animPlayer.timeMin();
    }
    else
    {
        m_animPlayer = anim::AnimPlayer();
        m_animTime = 0.0f;
    }
}

void NifViewport::SelectAnimSequence(int index)
{
    m_animPlayer.selectSequence(index);
    // Re-pose at the (possibly different) range start of the new sequence.
    m_animTime = std::clamp(m_animTime, m_animPlayer.timeMin(), m_animPlayer.timeMax());
    SetAnimTime(m_animPlayer.timeMin());
}

void NifViewport::SetAnimTime(float t)
{
    if (!m_animPlayer.hasAnimations())
        return;
    m_animTime = t;
    m_animPlayer.update(t, m_meshes);
    Invalidate();
}

void NifViewport::SetAnimPlaying(bool playing)
{
    if (m_animPlaying == playing)
        return;
    m_animPlaying = playing && m_animPlayer.hasAnimations();
    m_animLastTickMs = 0; // next tick starts a fresh dt
}

bool NifViewport::TickAnimation(unsigned long long nowMs)
{
    if (!m_animPlaying)
        return false;
    if (m_animLastTickMs == 0)
    {
        m_animLastTickMs = nowMs;
        return true;
    }
    const float dt = static_cast<float>(nowMs - m_animLastTickMs) * 0.001f * m_animSpeed;
    m_animLastTickMs = nowMs;

    const float tMin = m_animPlayer.timeMin();
    const float tMax = m_animPlayer.timeMax();
    float t = m_animTime + dt;
    if (t > tMax)
    {
        if (m_animLoop && tMax > tMin)
            t = tMin + std::fmod(t - tMin, tMax - tMin); // wrap the scene clock
        else
        {
            t = tMax;
            m_animPlaying = false; // one-shot: hold the last pose
        }
    }
    SetAnimTime(t);
    return m_animPlaying;
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

void NifViewport::OnRenderD3D(ID3D11DeviceContext* context)
{
    if (!context || !m_renderDevice || !m_textureRepository || !m_backplate)
        return;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return;
    const std::uint64_t deviceGeneration =
        m_backplate->GetGraphicsGeneration().device;
    std::string error;
    if (!m_renderDevice->EnsureInitialized(
            device.Get(),
            context,
            deviceGeneration,
            &error))
        return;

    m_textureRepository->BindDevice(device.Get(), deviceGeneration);
    m_device = device.Get();
    m_context = context;
    m_boundDeviceGeneration = deviceGeneration;
    if (!m_textures)
    {
        m_textures = std::make_unique<TextureCache>(m_textureRepository);
        if (!m_nifDirectory.empty())
            m_textures->SetNifDirectory(m_nifDirectory);
        PrefetchSceneTextures();
    }

    D2D1_RECT_F rect = LayoutRect();
    UINT w = static_cast<UINT>((std::max)(1.0f, rect.right - rect.left));
    UINT h = static_cast<UINT>((std::max)(1.0f, rect.bottom - rect.top));
    // 4x MSAA for the on-screen panes when enabled (clamped to device support
    // inside RenderTarget). Antialiases model silhouettes, the grid/axes, and
    // the wireframe/normal overlays; resolved to a single-sample texture for
    // D2D. Off = single-sample.
    if (!m_target.Resize(m_device, w, h, m_msaaEnabled ? 4u : 1u))
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
    const float dist = (std::max)(m_camera.distance(), 1e-4f);
    // Near plane: just ahead of the scene sphere when the eye is well outside
    // it (empty space in front then wastes no depth range), else a small
    // fraction of the eye distance for close-ups. Written as nested max so the
    // result is always positive and never needs a two-sided std::clamp whose
    // bounds could invert - the old clamp(lo, hi) asserted when zooming a large
    // scene in close (lo grew past hi as dist shrank).
    float nearZ = (std::max)((dist - sceneR) * 0.2f, dist * 0.02f);
    nearZ = (std::max)(nearZ, 1e-4f);
    const float farZ = (std::max)(dist + (std::max)(sceneR * 4.0f, 600.0f), nearZ * 100.0f);
    if (m_orthographic)
    {
        // View height matched to the perspective frustum at the target plane
        // (same on-screen size when toggling); a wide symmetric depth range
        // around the target keeps the scene + grid unclipped.
        const float orthoH = 2.0f * dist * std::tan(m_fovY * 0.5f);
        const float margin = (std::max)(sceneR * 4.0f, 600.0f);
        m_settings.proj = Camera::orthographicMatrix(orthoH, aspect, dist - margin, dist + margin);
    }
    else
    {
        m_settings.proj = Camera::projectionMatrix(m_fovY, aspect, nearZ, farZ);
    }
    m_settings.eyePos = m_camera.eyePosition();
    m_settings.selectedMesh = m_selectedMesh;
    // While the archive scan runs, BSA diffuse textures resolve to null (not
    // ready yet) - render them plain, not the magenta "missing" marker.
    m_settings.texturesPending = m_resolver && !m_resolver->IsArchiveScanReady();

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
    if (m_d2dBitmap && w == m_d2dBitmapWidth && h == m_d2dBitmapHeight &&
        m_target.ColorTexture() == m_d2dBitmapTex)
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
        m_d2dBitmapTex = m_target.ColorTexture();
    }
}

void NifViewport::OnRender(ID2D1RenderTarget* target)
{
    if (!target)
        return;
    EnsureD2DTarget();

    const D2D1_RECT_F destRect = LayoutRect();
    if (m_d2dBitmap)
        target->DrawBitmap(m_d2dBitmap.Get(), destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

    // Centered "Loading..." overlay while the model is still parsing/building
    // on the pool (the scene bitmap under it is just the placeholder grid).
    if (m_loading)
    {
        if (!m_loadingFormat)
        {
            if (IDWriteFactory* dw = FD2D::Core::DWriteFactory())
            {
                if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"", &m_loadingFormat)))
                {
                    m_loadingFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_loadingFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
        }
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (m_loadingFormat &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.82f, 0.90f), &brush)))
        {
            const wchar_t* text = L"Loading…";
            target->DrawTextW(text, 8u, m_loadingFormat.Get(), destRect, brush.Get());
        }
    }

    // On-screen orientation gizmo (top-left corner): always drawn on top of the
    // scene so the current orbit orientation is readable and clickable.
    DrawNavGizmo(target);
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
        // Orientation gizmo takes clicks before the orbit/pick logic: an LMB on
        // an axis nub animation-snaps to that view instead of starting an orbit.
        if (event.button == MouseButton::Left)
        {
            const int nub = HitTestGizmo(event.point);
            if (nub >= 0)
            {
                m_camAnimating = false;
                SnapToGizmoAxis(nub);
                RequestFocus();
                return true;
            }
        }
        m_camAnimating = false; // a manual gesture cancels any camera tween
        // LMB (or Alt+LMB) orbits; Alt+LMB never resolves to a pick on release.
        if (event.button == MouseButton::Left)
        {
            m_dragging = true; m_dragMoved = false; m_dragAlt = event.modifiers.alt;
            m_dragDownPt = event.point; m_lastMousePt = event.point;
            // Pivot fixed for this gesture: the selection center when orbit-on-
            // selection is enabled, else the plain orbit target (scene center).
            m_orbitPivot = m_orbitAroundSelection ? SelectionCenterOrTarget() : m_camera.target();
            RequestFocus(); return true;
        }
        // MMB (and Alt+MMB) pan.
        if (event.button == MouseButton::Middle)
        {
            m_panning = true; m_panMoved = false;
            m_panDownPt = event.point; m_lastMousePt = event.point;
            RequestFocus(); return true;
        }
        // Alt+RMB is a Maya-style dolly-zoom; plain RMB pans (and, without
        // movement, bubbles up as the app context menu).
        if (event.button == MouseButton::Right)
        {
            if (event.modifiers.alt) { m_dollying = true; m_lastMousePt = event.point; }
            else { m_panning = true; m_panMoved = false; m_panDownPt = event.point; m_lastMousePt = event.point; }
            RequestFocus(); return true;
        }
        break;

    case InputEventType::MouseUp:
        // Only claim the event if this viewport was actually the one tracking
        // the drag/pan; otherwise fall through so it doesn't swallow mouse-up
        // events that belong to some other control (e.g. releasing a Splitter drag).
        if (event.button == MouseButton::Left && m_dragging)
        {
            m_dragging = false;
            // A left click that never crossed the drag-jitter threshold is a
            // pick, not an orbit (unless it was an explicit Alt+LMB orbit):
            // select the sub-mesh under the cursor (or clear it on empty space).
            if (!m_dragMoved && !m_dragAlt && event.hasPoint)
                SetSelectedMesh(PickMeshAt(event.point));
            return true;
        }
        if (event.button == MouseButton::Right && m_dollying)
        {
            m_dollying = false;
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
        // Gizmo hover highlight (only while no nav gesture is active - a plain
        // MouseMove doesn't repaint on its own, so re-invalidate when the
        // hovered nub changes).
        if (!m_dragging && !m_panning && !m_dollying)
        {
            const int hover = HitTestGizmo(event.point);
            if (hover != m_gizmoHover)
            {
                m_gizmoHover = hover;
                Invalidate();
            }
        }
        // Self-heal a lost button-up: if the drag/pan's button is no longer
        // held (the release landed on another control that consumed it - e.g.
        // dragging out of the viewport onto a checkbox), stop tracking so the
        // view doesn't keep orbiting/panning with no button down.
        if (m_dragging && !event.modifiers.leftButton)
            m_dragging = false;
        if (m_panning && !event.modifiers.middleButton && !event.modifiers.rightButton)
            m_panning = false;
        if (m_dollying && !event.modifiers.rightButton)
            m_dollying = false;
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
                const float k = 0.01f * m_orbitSensitivity;
                OrbitAroundPivot(dx * k, dy * k); // pivots on the selection
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
            float scale = NavReferenceDistance() * 0.0015f * m_panSensitivity;
            m_camera.pan(-dx * scale, dy * scale);
            m_lastMousePt = event.point;
            if (m_onCameraChanged) m_onCameraChanged(m_camera);
            Invalidate();
            return true;
        }
        if (m_dollying)
        {
            // Maya-style Alt+RMB dolly: horizontal drag scales the eye
            // distance about the target (right = closer). Exponential per
            // pixel keeps the feel scale-independent; Shift is finer.
            const float perPixel = event.modifiers.shift ? 0.9985f : 0.994f;
            const float dd = dx * m_zoomSensitivity;
            ApplyZoomDistance(FlooredZoomDistance(std::pow(perPixel, dd),
                                                  NavFocusRadius() * kDollyCloseFloorFrac * std::abs(dd)));
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
        // directly instead of sliding out of view. Exponential steps keep the
        // feel consistent at any scale (a fixed 20% of the eye distance per
        // notch: fast to traverse a large scene, still fine near the target
        // since 20% of a small distance is small). Shift = 4x finer per notch
        // for precise framing.
        m_camAnimating = false; // a manual zoom cancels any camera tween
        const float perNotch = event.modifiers.shift ? 0.95f : 0.8f;
        const float delta = static_cast<float>(event.wheelDelta) / 120.0f * m_zoomSensitivity;
        const float d = m_camera.distance();
        // Exponential step, but floored at close-ups so zoom (in or out) never
        // collapses to nothing near the model (same idea as the pan floor).
        const float dNew = FlooredZoomDistance(std::pow(perNotch, delta),
                                               NavFocusRadius() * kWheelCloseFloorFrac * std::abs(delta));

        Vector3 origin, dir;
        // Recenter toward the cursor only while still approaching the pivot; once
        // the step would carry us through it (dNew below the min), the fly-through
        // in ApplyZoomDistance drives the target instead.
        if (m_zoomToCursor && dNew >= ZoomMinDistance() && event.hasPoint &&
            RayThroughPoint(event.point, origin, dir))
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
        ApplyZoomDistance(dNew);
        if (m_onCameraChanged) m_onCameraChanged(m_camera);
        Invalidate();
        return true;
    }

    case InputEventType::MouseDoubleClick:
    {
        if (event.button != MouseButton::Left || !event.hasPoint ||
            !FD2D::Util::RectContainsPoint(LayoutRect(), event.point))
            break;
        // A double-click landing on the gizmo is just a repeated axis snap, not
        // a focus-on-selection (which would pick the mesh behind the widget).
        if (const int nub = HitTestGizmo(event.point); nub >= 0)
        {
            m_camAnimating = false;
            SnapToGizmoAxis(nub);
            return true;
        }
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
    const float tanHalf = std::tan(m_fovY * 0.5f);
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

    if (m_orthographic)
    {
        // Parallel rays in ortho: direction is the view forward, and the origin
        // slides across the eye plane by the pixel's world offset (same ortho
        // height the projection uses).
        const float orthoH = 2.0f * (std::max)(m_camera.distance(), 1e-4f) * tanHalf;
        outOrigin = m_camera.eyePosition()
                  + right * (ndcX * orthoH * aspect * 0.5f)
                  + up * (ndcY * orthoH * 0.5f);
        outDir = forward;
        outDir.normalize();
        return true;
    }

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

    // Frame keeps the current yaw/pitch; animate target + distance to it.
    const float dist = (std::max)(radius * 2.2f, 0.01f);
    AnimateCameraTo(m_camera.yaw(), m_camera.pitch(), dist, center);
}

void NifViewport::FrameScene()
{
    // Whole-scene bounds (computed at load into m_sceneCenter/m_sceneRadius),
    // keeping the current orientation - "View All" ignoring any selection.
    const float dist = (std::max)(m_sceneRadius * 2.2f, 0.01f);
    AnimateCameraTo(m_camera.yaw(), m_camera.pitch(), dist, m_sceneCenter);
}

Vector3 NifViewport::SelectionCenterOrTarget() const
{
    if (const RenderMesh* sel = SelectedMesh(); sel && sel->geometry)
    {
        Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
        bool any = false;
        for (const Vector3& p : sel->geometry->positions)
        {
            Vector3 wp = sel->worldTransform * p;
            minB.boundMin(wp);
            maxB.boundMax(wp);
            any = true;
        }
        if (any)
            return (minB + maxB) * 0.5f;
    }
    return m_camera.target();
}

float NifViewport::NavFocusRadius() const
{
    if (const RenderMesh* sel = SelectedMesh(); sel && sel->geometry)
    {
        Vector3 minB(1e9f, 1e9f, 1e9f), maxB(-1e9f, -1e9f, -1e9f);
        bool any = false;
        for (const Vector3& p : sel->geometry->positions)
        {
            Vector3 wp = sel->worldTransform * p;
            minB.boundMin(wp);
            maxB.boundMax(wp);
            any = true;
        }
        if (any)
            return (std::max)((maxB - minB).length() * 0.5f, 1e-3f);
    }
    return (std::max)(m_sceneRadius, 1.0f);
}

float NifViewport::NavReferenceDistance() const
{
    return (std::max)(m_camera.distance(), NavFocusRadius() * kNavCloseFloorFrac);
}

float NifViewport::FlooredZoomDistance(float relFactor, float minStep) const
{
    const float d = m_camera.distance();
    float step = d - d * relFactor; // signed: >0 zooms in (closer), <0 out (farther)
    if (std::abs(step) < minStep)
        step = (step < 0.0f) ? -minStep : minStep;
    return d - step; // raw (may be < ZoomMinDistance / negative); ApplyZoomDistance handles it
}

float NifViewport::ZoomMinDistance() const
{
    return (std::max)(0.02f, NavFocusRadius() * 0.001f);
}

void NifViewport::ApplyZoomDistance(float rawNewDistance)
{
    const float minD = ZoomMinDistance();
    if (rawNewDistance < minD)
    {
        // Would cross the pivot: hold the distance at the floor and advance the
        // target forward instead, so the rig dollies through the pivot rather
        // than walling at the origin (lets zoom continue past the axis gizmo).
        Vector3 fwd = m_camera.forwardVector();
        fwd.normalize();
        Vector3 newTarget = m_camera.target() + fwd * (minD - rawNewDistance);
        // Bound the advance so runaway input can't push the pivot to infinity
        // (unsafe) or lose the model: keep it within a few scene radii of center.
        const float limit = (std::max)(m_sceneRadius, 1.0f) * kFlyThroughLimit;
        Vector3 off = newTarget - m_sceneCenter;
        const float len = off.length();
        if (len > limit)
            newTarget = m_sceneCenter + off * (limit / len);
        m_camera.setTarget(newTarget);
        m_camera.setDistance(minD);
    }
    else
    {
        m_camera.setDistance(rawNewDistance);
    }
}

void NifViewport::OrbitAroundPivot(float deltaYawRad, float deltaPitchRad)
{
    const Vector3 P = m_orbitPivot;
    Vector3 e = m_camera.eyePosition() - P; // eye relative to the pivot

    // Apply the turntable delta to the camera (clamps pitch at the poles), then
    // rotate the eye offset by the SAME rotation so the whole rig revolves
    // rigidly about P: yaw about world-Y, then pitch about the (post-yaw) right
    // axis (invariant under pitch, so it can be read off the new forward).
    const float pitch0 = m_camera.pitch();
    m_camera.orbit(deltaYawRad, deltaPitchRad);
    const float appliedPitch = m_camera.pitch() - pitch0;

    e = RotateAboutY(e, deltaYawRad);
    Vector3 f = m_camera.forwardVector();
    f.normalize();
    Vector3 right = Vector3::crossproduct(Vector3(0.0f, 1.0f, 0.0f), f);
    if (right.squaredLength() < 1e-8f)
        right = Vector3(1.0f, 0.0f, 0.0f);
    right.normalize();
    // A +pitch about right = cross(Y,F) tilts F's y DOWN, but Camera::orbit
    // raises pitch (F.y up), so the eye offset must rotate by the negated
    // applied pitch to revolve around the pivot (otherwise vertical drag reads
    // as a camera-relative tilt rather than an orbit about the selection).
    e = RotateAboutAxis(e, right, -appliedPitch);

    // Keep the eye at P+e; the look-at target follows from the new forward so
    // eyePosition() == P + e still holds (target = eye + forward*distance).
    m_camera.setTarget((P + e) + f * m_camera.distance());
}

void NifViewport::AnimateCameraTo(float yaw, float pitch, float distance,
                                  const Vector3& target, unsigned durationMs)
{
    // Snapshot the start pose.
    m_camAnimStartYaw = m_camera.yaw();
    m_camAnimStartPitch = m_camera.pitch();
    m_camAnimStartDist = m_camera.distance();
    m_camAnimStartTarget = m_camera.target();

    // Shortest-path yaw so a Back<->Front snap doesn't wind the long way round.
    float dyaw = yaw - m_camAnimStartYaw;
    while (dyaw > NSK_PI)  dyaw -= 2.0f * NSK_PI;
    while (dyaw < -NSK_PI) dyaw += 2.0f * NSK_PI;
    m_camAnimEndYaw = m_camAnimStartYaw + dyaw;
    m_camAnimEndPitch = std::clamp(pitch, -1.55f, 1.55f);
    m_camAnimEndDist = (std::max)(distance, 0.01f);
    m_camAnimEndTarget = target;

    if (durationMs == 0)
    {
        m_camera.setOrbit(m_camAnimEndYaw, m_camAnimEndPitch);
        m_camera.setDistance(m_camAnimEndDist);
        m_camera.setTarget(m_camAnimEndTarget);
        m_camAnimating = false;
        if (m_onCameraChanged) m_onCameraChanged(m_camera);
        Invalidate();
        return;
    }
    m_camAnimDurMs = durationMs;
    m_camAnimStartMs = GetTickCount64();
    m_camAnimating = true;
    if (m_onCameraAnimateRequested) m_onCameraAnimateRequested();
}

void NifViewport::AnimateToPreset(int presetIndex)
{
    float yaw = 0.0f, pitch = 0.0f;
    Camera::presetOrbit(presetIndex, yaw, pitch);
    AnimateCameraTo(yaw, pitch, m_camera.distance(), m_camera.target());
}

bool NifViewport::TickCameraAnimation(unsigned long long nowMs)
{
    if (!m_camAnimating)
        return false;
    const float t = (m_camAnimDurMs == 0)
        ? 1.0f
        : static_cast<float>(nowMs - m_camAnimStartMs) / static_cast<float>(m_camAnimDurMs);
    const bool done = t >= 1.0f;
    const float e = done ? 1.0f : (t * t * (3.0f - 2.0f * t)); // smoothstep ease-in-out
    m_camera.setOrbit(m_camAnimStartYaw + (m_camAnimEndYaw - m_camAnimStartYaw) * e,
                      m_camAnimStartPitch + (m_camAnimEndPitch - m_camAnimStartPitch) * e);
    m_camera.setDistance(m_camAnimStartDist + (m_camAnimEndDist - m_camAnimStartDist) * e);
    m_camera.setTarget(m_camAnimStartTarget + (m_camAnimEndTarget - m_camAnimStartTarget) * e);
    if (done)
        m_camAnimating = false;
    if (m_onCameraChanged) m_onCameraChanged(m_camera); // drives Sync Views mirroring
    Invalidate();
    return !done;
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

namespace
{
    // Axis colors matching the in-scene world axes (X red, Y green, Z blue).
    D2D1_COLOR_F GizmoAxisColor(int colorAxis, float alpha)
    {
        switch (colorAxis)
        {
        case 0:  return D2D1::ColorF(0.91f, 0.30f, 0.34f, alpha); // X
        case 1:  return D2D1::ColorF(0.52f, 0.78f, 0.30f, alpha); // Y
        default: return D2D1::ColorF(0.30f, 0.56f, 0.96f, alpha); // Z
        }
    }
}

void NifViewport::DrawNavGizmo(ID2D1RenderTarget* target)
{
    m_gizmoLive = false;
    if (!m_navGizmoEnabled || !target)
        return;

    const D2D1_RECT_F rect = LayoutRect();
    if (rect.right - rect.left < 120.0f || rect.bottom - rect.top < 120.0f)
        return; // too cramped to be useful (tiny pane)

    // Widget geometry: a small disc of axis nubs in the top-left corner, just
    // inside the viewport (clear of the top path strip and the material panel,
    // which lives top-right).
    constexpr float kRadius = 34.0f;   // center -> axis-nub distance
    constexpr float kMargin = 18.0f;
    const D2D1_POINT_2F center { rect.left + kMargin + kRadius,
                                 rect.top + kMargin + kRadius };
    m_gizmoCenter = center;
    m_gizmoRadius = kRadius;

    // Camera basis, identical to viewMatrix()/RayThroughPoint so the widget
    // agrees with what is rendered.
    Vector3 forward = m_camera.forwardVector();
    forward.normalize();
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    Vector3 right = Vector3::crossproduct(worldUp, forward);
    if (right.squaredLength() < 1e-8f)
        right = Vector3(1.0f, 0.0f, 0.0f);
    right.normalize();
    Vector3 up = Vector3::crossproduct(forward, right);
    up.normalize();

    struct AxisDef { Vector3 dir; int colorAxis; bool positive; const wchar_t* label; };
    static const AxisDef defs[6] = {
        { Vector3( 1.0f, 0.0f, 0.0f), 0, true,  L"X" },
        { Vector3(-1.0f, 0.0f, 0.0f), 0, false, L""  },
        { Vector3( 0.0f, 1.0f, 0.0f), 1, true,  L"Y" },
        { Vector3( 0.0f,-1.0f, 0.0f), 1, false, L""  },
        { Vector3( 0.0f, 0.0f, 1.0f), 2, true,  L"Z" },
        { Vector3( 0.0f, 0.0f,-1.0f), 2, false, L""  },
    };

    // Project each axis: x/y across the screen plane (y flips for pixel space),
    // depth along the forward axis (>0 = pointing away from the viewer).
    for (int i = 0; i < 6; ++i)
    {
        const Vector3& d = defs[i].dir;
        const float sx = Vector3::dotproduct(d, right);
        const float sy = Vector3::dotproduct(d, up);
        m_gizmoNubs[i].pos = { center.x + sx * kRadius, center.y - sy * kRadius };
        m_gizmoNubs[i].axis = d;
        m_gizmoNubs[i].colorAxis = defs[i].colorAxis;
        m_gizmoNubs[i].positive = defs[i].positive;
        m_gizmoNubs[i].depth = Vector3::dotproduct(d, forward);
    }
    m_gizmoLive = true;

    // Faint backing disc for legibility over bright models.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.10f, 0.12f, 0.42f), &bgBrush)))
    {
        D2D1_ELLIPSE disc = D2D1::Ellipse(center, kRadius + 14.0f, kRadius + 14.0f);
        target->FillEllipse(disc, bgBrush.Get());
    }

    if (!m_gizmoLetterFormat)
    {
        if (IDWriteFactory* dw = FD2D::Core::DWriteFactory())
        {
            if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"", &m_gizmoLetterFormat)))
            {
                m_gizmoLetterFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_gizmoLetterFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }

    // Draw far-to-near so nubs pointing toward the viewer land on top.
    int order[6] = { 0, 1, 2, 3, 4, 5 };
    std::sort(order, order + 6, [this](int a, int b) { return m_gizmoNubs[a].depth > m_gizmoNubs[b].depth; });

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush, textBrush, hollowBrush;
    target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &brush);
    target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.11f, 0.13f), &textBrush);
    target->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.14f, 0.17f), &hollowBrush);
    if (!brush)
        return;

    for (int idx = 0; idx < 6; ++idx)
    {
        const int i = order[idx];
        const GizmoNub& nub = m_gizmoNubs[i];
        // Fade with depth: nearest (depth=-1) fully opaque, farthest (+1) dim.
        const float a = 0.45f + 0.55f * (0.5f - 0.5f * nub.depth);
        const bool hovered = (i == m_gizmoHover);
        const D2D1_COLOR_F col = GizmoAxisColor(nub.colorAxis, hovered ? 1.0f : a);

        // Positive axes get a stem line from the center to the nub.
        if (nub.positive)
        {
            brush->SetColor(col);
            target->DrawLine(center, nub.pos, brush.Get(), 2.0f);
        }

        const float r = (nub.positive ? 8.5f : 6.0f) + (hovered ? 2.0f : 0.0f);
        D2D1_ELLIPSE e = D2D1::Ellipse(nub.pos, r, r);
        if (nub.positive)
        {
            brush->SetColor(col);
            target->FillEllipse(e, brush.Get());
            if (hovered)
            {
                brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
                target->DrawEllipse(e, brush.Get(), 1.5f);
            }
            if (m_gizmoLetterFormat && textBrush)
            {
                const D2D1_RECT_F lr { nub.pos.x - r, nub.pos.y - r, nub.pos.x + r, nub.pos.y + r };
                target->DrawTextW(defs[i].label, 1u, m_gizmoLetterFormat.Get(), lr, textBrush.Get());
            }
        }
        else
        {
            // Negative axes: a hollow ring (filled with the disc color) so they
            // read as "the back side" of each axis.
            if (hollowBrush)
                target->FillEllipse(e, hollowBrush.Get());
            brush->SetColor(col);
            target->DrawEllipse(e, brush.Get(), hovered ? 2.5f : 1.8f);
        }
    }
}

int NifViewport::HitTestGizmo(POINT pt) const
{
    if (!m_gizmoLive || !m_navGizmoEnabled)
        return -1;
    // Prefer the nub nearest the viewer (smallest depth) among those the cursor
    // is over, so a front nub wins when it overlaps a back one.
    constexpr float kHitR = 11.0f;
    int best = -1;
    float bestDepth = 1e9f;
    for (int i = 0; i < 6; ++i)
    {
        const float dx = static_cast<float>(pt.x) - m_gizmoNubs[i].pos.x;
        const float dy = static_cast<float>(pt.y) - m_gizmoNubs[i].pos.y;
        if (dx * dx + dy * dy <= kHitR * kHitR && m_gizmoNubs[i].depth < bestDepth)
        {
            bestDepth = m_gizmoNubs[i].depth;
            best = i;
        }
    }
    return best;
}

void NifViewport::SnapToGizmoAxis(int nubIndex)
{
    if (nubIndex < 0 || nubIndex >= 6)
        return;
    // Snap to the named-view orientation for the clicked axis, matching the
    // View presets (Camera::presetOrbit): each preset looks *along* its axis
    // (Top = forward +Y, Front = forward +Z, Left = forward +X, ...), so the
    // +Y nub gives the bird's-eye Top view (not Bottom). Solve yaw/pitch from
    // forward = (sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch)) = axis.
    const Vector3 f = m_gizmoNubs[nubIndex].axis;
    float yaw, pitch;
    if (std::abs(f[1]) > 0.9999f)
    {
        // Pole (Top/Bottom): keep the current heading so the view doesn't spin
        // about vertical on the way there.
        yaw = m_camera.yaw();
        pitch = f[1] > 0.0f ? (NSK_PI * 0.5f) : -(NSK_PI * 0.5f);
    }
    else
    {
        yaw = std::atan2(f[0], f[2]);
        pitch = std::asin(std::clamp(f[1], -1.0f, 1.0f));
    }
    AnimateCameraTo(yaw, pitch, m_camera.distance(), m_camera.target());
}

} // namespace nsk
