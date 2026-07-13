// D3D11Renderer.h - replacement for src/gl/renderer.h (Phase 3).
//
// See NifDocument.h / SceneBuilder.h for the data-side scope notes. On the
// render side: this class does NOT own an ID3D11Device/IDXGISwapChain -
// Initialize() takes an existing device+context. For the standalone exe
// (this plan's first deliverable) that device is created by FD2D::Backplate
// (see NifViewport.cpp); when this is later embedded into FICture2, the same
// Initialize() call takes FICture2's already-shared device instead, with no
// interface change. This directly satisfies the plan's Phase 3 note: "이후
// FICture2 통합 시 디바이스 공유로 교체 예정임을 인터페이스에 반영".
//
// Also folds in Phase 3's gltools.cpp port (grid/axes immediate-mode draws,
// tracked as phase3_tools) as DrawGrid/DrawAxes, since both the lit mesh
// path and the unlit tools path share the same device/context/state-object
// plumbing - see Shaders.h for why one unlit shader covers gltools' whole
// immediate-mode drawing surface.
#pragma once

#include "../core/SceneBuilder.h"
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
    Color4 clearColor { 0.09f, 0.09f, 0.10f, 1.0f };
    bool showGrid = true;
    bool showAxes = true;
    bool wireframe = false;
};

class D3D11Renderer
{
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error = nullptr);
    bool IsInitialized() const { return m_device != nullptr; }

    // (Re)creates the offscreen color+depth render targets at the given
    // pixel size. Safe to call every frame; it is a no-op if the size did
    // not change.
    bool Resize(UINT width, UINT height);
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

    ID3D11Texture2D* ColorTexture() const { return m_colorTex.Get(); }
    ID3D11ShaderResourceView* ColorSRV() const { return m_colorSRV.Get(); }

    // Drops all cached per-geometry GPU buffers. Call when a new NIF has
    // been loaded so stale geometry pointers cannot be reused.
    void InvalidateMeshCache();

    void RenderScene(const std::vector<RenderMesh>& meshes, const RenderSettings& settings, TextureCache* textures);

private:
    struct GpuMesh
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        UINT indexCount = 0;
    };

    bool CreateShaders(std::string* error);
    bool CreateStateObjects();
    const GpuMesh* GetOrCreateGpuMesh(const NifGeometry* geometry);
    void DrawImmediateLines(const std::vector<float>& interleavedPosColor, const Matrix4& world, const Matrix4& viewProj, D3D11_PRIMITIVE_TOPOLOGY topology);
    void BuildGridAndAxesGeometry();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    UINT m_width = 0;
    UINT m_height = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_colorRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_colorSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthDSV;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_litVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_litPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_litLayout;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_unlitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_unlitPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_unlitLayout;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerObject;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrameUnlit;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerObjectUnlit;

    // State object cache (Phase 3's "Blend/DepthStencil/Rasterizer 상태
    // 객체 캐시"): a handful of fixed variants is enough since materials in
    // this parser's scope only distinguish opaque vs alpha-blended.
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendAlpha;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDefault;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterSolidNoCull; // SLSF2_Double_Sided materials
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterDecal;       // SLSF1_(Dynamic_)Decal: negative depth bias (glPolygonOffset(-1,-1) equivalent)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterDecalNoCull;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterWireframe;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteTexSRV;     // 1x1 white fallback (diffuse / env-mask)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_blackTexSRV;     // 1x1 black fallback (glow / backlight)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_flatNormalSRV;   // 1x1 flat normal + full spec mask
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_missingTexSRV;   // 1x1 magenta: named-but-unresolved diffuse marker

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_gridAxesVB;
    UINT m_gridVertexCount = 0;
    UINT m_axesVertexStart = 0;
    UINT m_axesVertexCount = 0;

    std::unordered_map<const NifGeometry*, GpuMesh> m_meshCache;
};

} // namespace nsk
