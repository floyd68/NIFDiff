// NifViewport.h - Qt-free replacement for src/glview.h+glview.cpp's role of
// hosting one 3D view (Phase 4).
//
// GLView was a QGLWidget subclass mixing camera-drag mouse handling, an
// immediate OpenGL draw call, and a paintEvent-driven Qt render loop.
// NifViewport is an FD2D::Wnd instead: FD2D::Wnd::OnRenderD3D() renders the
// current NifDocument's scene into this view's own RenderTarget (offscreen
// D3D11 texture) via the shared RenderDevice, and OnRender() (the D2D pass)
// composites that texture into the control's on-screen rect by wrapping it as
// an ID2D1Bitmap1 over the shared DXGI surface (same GPU device end-to-end, no
// CPU copy) - the same interop technique FD2D::Backplate already uses for its
// own offscreen double-buffer (see Backplate.cpp's CreateBitmapFromDxgiSurface
// calls). The shaders/states/IBL live once in the app-wide RenderDevice; only
// the framebuffer (RenderTarget) and geometry cache (RenderMeshCache) are
// per-view.
#pragma once

#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"
#include "../core/Camera.h"
#include "../core/ResourceResolver.h"
#include "../render/RenderDevice.h"
#include "../render/RenderTarget.h"
#include "../render/RenderMeshCache.h"
#include "../render/TextureCache.h"

#include <Wnd.h>
#include <d2d1_1.h>
#include <functional>
#include <memory>

namespace nsk
{

class NifViewport : public FD2D::Wnd
{
public:
    explicit NifViewport(const std::wstring& name);

    // doc may be null to clear the view. NifViewport does not own it -
    // caller (NifCompareView/app shell) keeps the loaded NifDocument alive
    // at least as long as it is set here.
    void SetDocument(const NifDocument* doc);
    const NifDocument* Document() const { return m_doc; }

    // Async pane-load hand-off: install a scene whose NifDocument parse and
    // SceneBuilder::build already ran on the load pool, so the UI thread only
    // does the post-build setup (texture prefetch + camera fit), never the
    // parse/build. `doc` must outlive this viewport (the pane holds it).
    void SetPrebuiltScene(const NifDocument* doc, std::vector<RenderMesh> meshes);

    // Whether hidden (NiAVObject-marked) subtrees are included in the built
    // scene - the async load worker needs it to build the same mesh set.
    bool ShowHiddenNodes() const { return m_showHiddenNodes; }

    // Shared Game Data / override / BSA resolver. Must outlive this viewport.
    void SetResourceResolver(ResourceResolver* resolver);

    // Shared cross-pane texture pool (owns the SRVs; this viewport's
    // TextureCache is only a resolution memo on top). Must outlive this
    // viewport and be set before it is attached to a backplate.
    void SetTextureRepository(TextureRepository* repository);

    // The single app-wide render core (shaders/states/IBL). Must outlive this
    // viewport and be set before it is attached to a backplate. The viewport
    // still owns its own RenderTarget (framebuffer) and RenderMeshCache.
    void SetRenderDevice(RenderDevice* device) { m_renderDevice = device; }

    void InvalidateTextureCache();

    Camera& GetCamera() { return m_camera; }
    const Camera& GetCamera() const { return m_camera; }
    void SetCamera(const Camera& cam) { m_camera = cam; Invalidate(); }
    using CameraChangedHandler = std::function<void(const Camera&)>;
    void SetOnCameraChanged(CameraChangedHandler handler) { m_onCameraChanged = std::move(handler); }

    // Click-to-select (NifSkope-style): a left click that never turned into
    // an orbit drag ray-picks the nearest sub-mesh under the cursor, which
    // is then re-drawn with a wireframe overlay. Clicking empty space (or
    // reloading the document) clears the selection.
    int SelectedMeshIndex() const { return m_selectedMesh; }
    const RenderMesh* SelectedMesh() const;
    std::size_t TotalTriangleCount() const; // across every mesh of the loaded scene
    // The current render list (world transforms + material copies), used by
    // the material diff panel to match the selected mesh across panes.
    const std::vector<RenderMesh>& Meshes() const { return m_meshes; }

    // Pooled texture entry (format/dims/mips/resolved source) for the
    // texture inspector; null when unresolvable or no cache yet.
    TextureRepository::Entry* TextureEntry(const std::string& relativePath)
    {
        return m_textures ? m_textures->EntryFor(relativePath) : nullptr;
    }
    // This viewport's texture resolution context (the loaded NIF's folder).
    const std::wstring& NifDirectory() const { return m_nifDirectory; }

    // Human-readable shader classification for the UI labels, covering the
    // extended-material conventions on top of the vanilla shader types:
    // "True PBR" (PBRNifPatcher flag), "Complex Material" (env mask with
    // height in alpha, probed like the shader does), "Parallax", "EnvMap",
    // "Glow", ..., plus "MSN"/"Decal"/"Refraction" suffixes.
    std::wstring ShaderKindFor(const RenderMesh& mesh) const;
    std::wstring SelectedMeshShaderKind() const; // empty when nothing is selected
    std::wstring ShaderKindSummary() const;      // e.g. "Default x8 · Parallax x3", empty when no meshes
    using SelectionChangedHandler = std::function<void(const RenderMesh* /*null when cleared*/)>;
    void SetOnSelectionChanged(SelectionChangedHandler handler) { m_onSelectionChanged = std::move(handler); }

    // Lighting/display settings, driven by NifCompareControlPanel.
    void SetAmbient(float v) { m_settings.ambient = v; Invalidate(); }
    void SetBrightness(float v) { m_settings.brightness = v; Invalidate(); }
    void SetLightDeclinationDegrees(float deg);
    void SetLightPlanarAngleDegrees(float deg);
    void SetFrontalLight(bool enabled) { m_frontalLight = enabled; Invalidate(); }
    void SetShowGrid(bool show) { m_settings.showGrid = show; Invalidate(); }
    void SetShowAxes(bool show) { m_settings.showAxes = show; Invalidate(); }
    // Vertex normal/tangent line overlays (selected mesh when one is
    // picked, all meshes otherwise) - see RenderSettings.
    void SetShowNormals(bool show) { m_settings.showNormals = show; Invalidate(); }
    void SetShowTangents(bool show) { m_settings.showTangents = show; Invalidate(); }
    // 4x MSAA on this pane's offscreen target (off = single-sample). Takes
    // effect on the next render (RenderTarget resizes to the new sample count).
    void SetMsaaEnabled(bool on) { m_msaaEnabled = on; Invalidate(); }
    void SetWireframe(bool wire) { m_settings.wireframe = wire; Invalidate(); }
    void SetParallaxHeightScale(float v) { m_settings.parallaxHeightScale = v; Invalidate(); }
    void SetEnableParallax(bool on) { m_settings.enableParallax = on; Invalidate(); }
    void SetEnableComplexMaterial(bool on) { m_settings.enableComplexMaterial = on; Invalidate(); }
    void SetEnablePBR(bool on) { m_settings.enablePBR = on; Invalidate(); }
    // Render-channel toggles (CHANNELS group): isolate one shading input.
    void SetEnableTextures(bool on)     { m_settings.enableTextures = on; Invalidate(); }
    void SetEnableVertexColors(bool on) { m_settings.enableVertexColors = on; Invalidate(); }
    void SetEnableSpecular(bool on)     { m_settings.enableSpecular = on; Invalidate(); }
    void SetEnableGlow(bool on)         { m_settings.enableGlow = on; Invalidate(); }
    void SetEnableLighting(bool on)     { m_settings.enableLighting = on; Invalidate(); }
    // NifSkope's "Show Hidden": opt NiAVObject-hidden subtrees (furniture
    // marker rigs, editor markers, bounds placeholders) back into the
    // scene. Rebuilds the mesh list, so the current selection is cleared.
    void SetShowHiddenNodes(bool show);
    void ResetCamera();

    // Frames the picked sub-mesh's world bounds (or the whole scene when
    // nothing is selected), keeping the current orbit orientation - the
    // remedy for scenes whose interesting meshes sit far off the overall
    // bbox center. Double-clicking a mesh selects and focuses it in one
    // go; double-clicking empty space re-frames the whole scene.
    void FocusOnSelection();

    // Saves this viewport's last rendered 3D frame (the offscreen color
    // target - clean render, no path/stats chrome) as a PNG.
    bool SaveScreenshot(const std::wstring& path, std::string* error = nullptr)
    {
        return m_target.SaveColorToPng(m_device, m_context, path, error);
    }

    // Whether any loaded mesh actually runs the height-based parallax the
    // "Parallax Height" slider controls: a vanilla parallax material
    // (ST_Heightmap + _p.dds) or a complex material carrying height in its
    // env mask alpha. True PBR displacement is excluded - it follows the
    // authored displacement_scale, not the slider.
    bool HasActiveParallax();

    // Per-feature presence tests for the bottom strip's extended-material
    // toggles - each checkbox is only enabled while some loaded pane has
    // material the toggle would actually change. HasParallaxMaterials is
    // HasActiveParallax plus True PBR displacement (the Parallax toggle
    // masks PBR's _p POM too, unlike the slider).
    bool HasParallaxMaterials();
    bool HasComplexMaterials();
    bool HasPBRMaterials() const;

    void OnAttached(FD2D::Backplate& backplate) override;
    FD2D::Size Measure(FD2D::Size available) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    void RebuildScene();
    // Post-build setup shared by RebuildScene and SetPrebuiltScene (async load):
    // async texture prefetch + camera framing from the current m_meshes.
    void FinishSceneLoad();
    void EnsureD2DTarget();
    void UpdateFrontalLight();
    int PickMeshAt(POINT pt) const; // -1 when no mesh under pt
    // World-space ray through the given client pixel, from the same camera
    // basis/fov the render uses. False when the viewport has no size.
    bool RayThroughPoint(POINT pt, Vector3& outOrigin, Vector3& outDir) const;
    void SetSelectedMesh(int index);

    const NifDocument* m_doc = nullptr;
    std::vector<RenderMesh> m_meshes;
    Vector3 m_sceneCenter;       // world bounds of the current mesh list,
    float m_sceneRadius = 0.0f;  // for camera framing + adaptive clip planes
    Camera m_camera;
    RenderSettings m_settings;
    float m_lightDeclinationDeg = 45.0f;
    float m_lightPlanarAngleDeg = 45.0f;
    bool m_frontalLight = false;
    bool m_showHiddenNodes = false;
    bool m_msaaEnabled = true;

    // Shared render core (device-level resources) + this view's own
    // framebuffer and geometry cache (see the render/ headers).
    RenderDevice* m_renderDevice = nullptr;
    RenderTarget m_target;
    RenderMeshCache m_meshCache;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::unique_ptr<TextureCache> m_textures;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    std::wstring m_nifDirectory;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_d2dBitmap;
    UINT m_d2dBitmapWidth = 0;
    UINT m_d2dBitmapHeight = 0;
    // The color texture the current bitmap wraps. The RenderTarget can swap
    // this texture out at the same pixel size (e.g. toggling MSAA changes the
    // sample count), so the bitmap must be rebuilt whenever it changes, not
    // just on a resize.
    ID3D11Texture2D* m_d2dBitmapTex = nullptr;

    bool m_dragging = false;
    bool m_dragMoved = false; // left drag actually orbited (a still click picks a mesh instead)
    bool m_panning = false;
    bool m_panMoved = false; // pan actually moved (right-click without movement bubbles up for the app context menu)
    POINT m_dragDownPt {};
    POINT m_panDownPt {};
    POINT m_lastMousePt {};

    int m_selectedMesh = -1; // index into m_meshes, -1 = none

    CameraChangedHandler m_onCameraChanged {};
    SelectionChangedHandler m_onSelectionChanged {};
};

} // namespace nsk
