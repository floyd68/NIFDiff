// RenderDevice.h - the single shared D3D11 rendering core (was D3D11Renderer,
// which duplicated every shader/state/IBL resource AND its own framebuffer
// per viewport).
//
// This object owns only device-level, immutable-once-built resources:
// shaders, input layouts, constant buffers, blend/depth/raster/sampler state
// objects, the 1x1 fallback textures, the procedural IBL cubemap, and the
// grid/axes buffer. One instance is created for the whole app (see
// NIFDiffApp.cpp) and shared by every NifViewport - and, for item 12, by the
// background thumbnail renderer - so those costs are paid once instead of
// once per pane. It does NOT own a framebuffer or a geometry cache: draw
// targets are RenderTarget (per view/thumbnail) and geometry buffers are
// RenderMeshCache (per view), both passed into RenderScene.
//
// It does NOT own an ID3D11Device/IDXGISwapChain either - EnsureInitialized()
// takes an existing device+context (created by FD2D::Backplate; when embedded
// into FICture2 later, the same call takes FICture2's shared device).
//
// Also folds in Phase 3's gltools.cpp port (grid/axes immediate-mode draws,
// tracked as phase3_tools), since both the lit mesh path and the unlit tools
// path share the same device/context/state-object plumbing - see
// shaders/Unlit.hlsl for why one unlit shader covers gltools' whole
// immediate-mode drawing surface.
#pragma once

#include "../core/SceneBuilder.h"
#include "RenderMeshCache.h"
#include "RenderTarget.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>
#include <string>

namespace nsk
{

class TextureCache;

struct RenderSettings
{
    Matrix4 view;
    Matrix4 proj;
    Vector3 lightDir { 0.3f, 0.6f, -0.75f };
    Vector3 eyePos;
    float ambient = 0.35f;
    float brightness = 1.0f;
    float parallaxHeightScale = 2.0f; // vanilla/_m POM HeightScale (0.1*scale UV depth budget); PBR unaffected

    // Extended-material feature toggles (bottom control strip). Off = the
    // material renders through the closest legacy interpretation instead:
    // no POM/displacement, complex materials as plain env-mapped surfaces,
    // True PBR through the vanilla lit path.
    bool enableParallax = true;
    bool enableComplexMaterial = true;
    bool enablePBR = true;

    // Render-channel toggles (bottom strip CHANNELS group): switch one
    // shading input off at a time to isolate why two panes differ.
    bool enableTextures = true;     // off: diffuse sampled as white
    bool enableVertexColors = true; // off: vertex color rgb whitened (alpha kept)
    bool enableSpecular = true;     // off: legacy Blinn-Phong AND PBR GGX specular dropped
    bool enableGlow = true;         // off: emissive/glow contributions dropped
    bool enableLighting = true;     // off: raw textured surface (unlit)
    Color4 clearColor { 0.09f, 0.09f, 0.10f, 1.0f };
    bool showGrid = true;
    bool showAxes = true;
    // Vertex normal/tangent line overlays (NifSkope's gltools drawNormals
    // port): cyan normals / magenta tangents, drawn for the selected mesh
    // only when one is picked, for every mesh otherwise.
    bool showNormals = false;
    bool showTangents = false;
    bool wireframe = false;
    int selectedMesh = -1; // index into RenderScene's meshes; drawn again as a wireframe overlay when valid
};

class RenderDevice
{
public:
    // Idempotent: the first call builds every shared resource from the given
    // device+context; later calls (each viewport's OnAttached passes the same
    // backplate device) are no-ops. Returns false only if the first build
    // fails.
    bool EnsureInitialized(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error = nullptr);
    bool IsInitialized() const { return m_device != nullptr; }

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }

    // Renders `meshes` into `target`, caching that view's geometry buffers in
    // `cache`. The caller sizes `target` (RenderTarget::Resize) first. The
    // shared shaders/states/IBL come from this device; nothing here is
    // per-view except the two by-reference parameters.
    void RenderScene(RenderTarget& target, RenderMeshCache& cache,
                     const std::vector<RenderMesh>& meshes, const RenderSettings& settings,
                     TextureCache* textures);

private:
    bool CreateShaders(std::string* error);
    bool CreateStateObjects();
    const GpuMesh* GetOrCreateGpuMesh(RenderMeshCache& cache, const NifGeometry* geometry);
    const GpuLineMesh* GetOrCreateLineMesh(RenderMeshCache& cache, const NifGeometry* geometry);
    void BuildGridAndAxesGeometry();
    void BuildIblCubemap();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_litVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_litPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_litLayout;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_unlitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_unlitPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_unlitLayout;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_highlightVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_highlightPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_highlightLayout;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerObject;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrameUnlit;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerObjectUnlit;

    // State object cache (Phase 3's "Blend/DepthStencil/Rasterizer 상태
    // 객체 캐시"). Blended materials carry their NiAlphaProperty src/dst
    // functions (additive fire glows etc.), so blend states beyond the two
    // fixed ones are created on demand and cached by (src,dst) pair.
    ID3D11BlendState* GetBlendState(std::uint8_t srcBlend, std::uint8_t dstBlend);
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendAlpha; // standard SRC_ALPHA/INV_SRC_ALPHA
    std::unordered_map<std::uint16_t, Microsoft::WRL::ComPtr<ID3D11BlendState>> m_blendCache;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDefault;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthNoWrite; // SLSF2_ZBuffer_Write cleared (test only)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterSolidNoCull; // SLSF2_Double_Sided materials
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterDecal;       // SLSF1_(Dynamic_)Decal: negative depth bias (glPolygonOffset(-1,-1) equivalent)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterDecalNoCull;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterWireframe;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterHighlight;  // wireframe + negative depth bias: selection overlay sits on its own surface
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteTexSRV;     // 1x1 white fallback (diffuse / env-mask)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_blackTexSRV;     // 1x1 black fallback (glow / backlight)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_flatNormalSRV;   // 1x1 flat normal + full spec mask
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_missingTexSRV;   // 1x1 magenta: named-but-unresolved diffuse marker
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_iblCubeSRV;      // procedural sky/ground cube (t9): PBR ambient specular stand-in

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_gridAxesVB;
    UINT m_gridVertexCount = 0;
    UINT m_axesVertexStart = 0;
    UINT m_axesVertexCount = 0;
};

} // namespace nsk
