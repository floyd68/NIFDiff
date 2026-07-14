// NifViewport.h - Qt-free replacement for src/glview.h+glview.cpp's role of
// hosting one 3D view (Phase 4).
//
// GLView was a QGLWidget subclass mixing camera-drag mouse handling, an
// immediate OpenGL draw call, and a paintEvent-driven Qt render loop.
// NifViewport is an FD2D::Wnd instead: FD2D::Wnd::OnRenderD3D() renders the
// current NifDocument's scene into an offscreen D3D11 texture via
// D3D11Renderer, and OnRender() (the D2D pass) composites that texture into
// the control's on-screen rect by wrapping it as an ID2D1Bitmap1 over the
// shared DXGI surface (same GPU device end-to-end, no CPU copy) - the same
// interop technique FD2D::Backplate already uses for its own offscreen
// double-buffer (see Backplate.cpp's CreateBitmapFromDxgiSurface calls).
#pragma once

#include "../core/NifDocument.h"
#include "../core/SceneBuilder.h"
#include "../core/Camera.h"
#include "../core/ResourceResolver.h"
#include "../render/D3D11Renderer.h"
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

    // Shared Game Data / override / BSA resolver. Must outlive this viewport.
    void SetResourceResolver(ResourceResolver* resolver);

    // Shared cross-pane texture pool (owns the SRVs; this viewport's
    // TextureCache is only a resolution memo on top). Must outlive this
    // viewport and be set before it is attached to a backplate.
    void SetTextureRepository(TextureRepository* repository);

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
    void SetWireframe(bool wire) { m_settings.wireframe = wire; Invalidate(); }
    void SetParallaxHeightScale(float v) { m_settings.parallaxHeightScale = v; Invalidate(); }
    void SetEnableParallax(bool on) { m_settings.enableParallax = on; Invalidate(); }
    void SetEnableComplexMaterial(bool on) { m_settings.enableComplexMaterial = on; Invalidate(); }
    void SetEnablePBR(bool on) { m_settings.enablePBR = on; Invalidate(); }
    // NifSkope's "Show Hidden": opt NiAVObject-hidden subtrees (furniture
    // marker rigs, editor markers, bounds placeholders) back into the
    // scene. Rebuilds the mesh list, so the current selection is cleared.
    void SetShowHiddenNodes(bool show);
    void ResetCamera();

    // Saves this viewport's last rendered 3D frame (the offscreen color
    // target - clean render, no path/stats chrome) as a PNG.
    bool SaveScreenshot(const std::wstring& path, std::string* error = nullptr)
    {
        return m_renderer.SaveColorToPng(path, error);
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
    void EnsureD2DTarget();
    void UpdateFrontalLight();
    int PickMeshAt(POINT pt) const; // -1 when no mesh under pt
    void SetSelectedMesh(int index);

    const NifDocument* m_doc = nullptr;
    std::vector<RenderMesh> m_meshes;
    Camera m_camera;
    RenderSettings m_settings;
    float m_lightDeclinationDeg = 45.0f;
    float m_lightPlanarAngleDeg = 45.0f;
    bool m_frontalLight = false;
    bool m_showHiddenNodes = false;

    D3D11Renderer m_renderer;
    std::unique_ptr<TextureCache> m_textures;
    ResourceResolver* m_resolver = nullptr;
    TextureRepository* m_textureRepository = nullptr;
    std::wstring m_nifDirectory;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_d2dBitmap;
    UINT m_d2dBitmapWidth = 0;
    UINT m_d2dBitmapHeight = 0;

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
