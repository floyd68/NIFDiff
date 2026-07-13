#include "D3D11Renderer.h"
#include "Shaders.h"
#include "TextureCache.h"

#include <d3dcompiler.h>
#include <vector>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace nsk
{

namespace
{
    struct Vertex { float pos[3]; float normal[3]; float uv[2]; float color[4]; float tangent[3]; };
    struct LineVertex { float pos[3]; float color[4]; };

    struct CBPerFrameLit { float viewProj[16]; float lightDir[3]; float ambient; float eyePos[3]; float brightness; };
    struct CBPerObjectLit
    {
        float world[16];
        float glowColor[4];     // lighting: emissive rgb (already * emissiveMultiple) + material alpha
                                 // effect:   Base Color rgba
        float spec[4];           // specular color (rgb) + glossiness (a)
        float uvTransform[4];    // UV Scale (xy) + UV Offset (zw)
        float tintColor[4];      // skin/hair tint rgb + multilayer outer reflection strength (w)
        float params[4];         // specStrength|baseColorScale, lightingEffect1, lightingEffect2, envReflection
        float innerParams[4];    // multilayer: inner scale xy, inner thickness, outer refraction
        float falloffParams[4];  // effect: start angle, stop angle, start opacity, stop opacity
        std::uint32_t flags;     // feature bitmask - see Shaders.h's table / kLit* below
        float alphaTest;         // alpha-test threshold, used when kLitAlphaTest is set
        float pad[2];
    };
    struct CBPerFrameUnlit { float viewProj[16]; };
    struct CBPerObjectUnlit { float world[16]; };

    static_assert(sizeof(CBPerFrameLit) == 96, "CBPerFrameLit must match Shaders.h PerFrame layout");
    static_assert(sizeof(CBPerObjectLit) == 192, "CBPerObjectLit must match Shaders.h PerObject layout");

    // gFlags bits - keep in sync with Shaders.h's kLitShaderHLSL table.
    constexpr std::uint32_t kLitHasDiffuse        = 1;
    constexpr std::uint32_t kLitHasNormalMap      = 2;
    constexpr std::uint32_t kLitHasGlowMap        = 4;
    constexpr std::uint32_t kLitHasEmit           = 8;
    constexpr std::uint32_t kLitHasSoftlight      = 16;
    constexpr std::uint32_t kLitHasRimlight       = 32;
    constexpr std::uint32_t kLitHasBacklight      = 64;
    constexpr std::uint32_t kLitHasCubeMap        = 128;
    constexpr std::uint32_t kLitHasEnvMask        = 256;
    constexpr std::uint32_t kLitHasHeightMap      = 512;
    constexpr std::uint32_t kLitHasTintColor      = 1024;
    constexpr std::uint32_t kLitHasSpecular       = 2048;
    constexpr std::uint32_t kLitModelSpaceNormals = 4096;
    constexpr std::uint32_t kLitHasSpecularMap    = 8192;    // t7 = MSN specular map (not backlight)
    constexpr std::uint32_t kLitHasDetailMask     = 16384;   // t3 = face detail mask (not height)
    constexpr std::uint32_t kLitHasTintMask       = 32768;   // t8 = face tint mask
    constexpr std::uint32_t kLitMultiLayer        = 65536;   // t8 = multilayer inner map
    constexpr std::uint32_t kLitIsEffect          = 131072;  // sk_effectshader path; t8 = greyscale palette
    constexpr std::uint32_t kLitEffectUseFalloff  = 262144;
    constexpr std::uint32_t kLitEffectGreyscaleColor = 524288;
    constexpr std::uint32_t kLitEffectGreyscaleAlpha = 1048576;
    constexpr std::uint32_t kLitEffectWeaponBlood = 2097152;
    constexpr std::uint32_t kLitAlphaTest         = 4194304; // NiAlphaProperty alpha test -> clip()

    void UploadDynamicCB(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const void* data, std::size_t size)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, data, size);
            ctx->Unmap(buf, 0);
        }
    }

    ComPtr<ID3D11Buffer> CreateDynamicCB(ID3D11Device* device, UINT byteSize)
    {
        const D3D11_BUFFER_DESC desc {
            .ByteWidth = byteSize,
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        ComPtr<ID3D11Buffer> buf;
        device->CreateBuffer(&desc, nullptr, &buf);
        return buf;
    }
}

bool D3D11Renderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, std::string* error)
{
    if (!device || !context)
    {
        if (error) *error = "D3D11Renderer::Initialize: null device/context";
        return false;
    }
    m_device = device;
    m_context = context;

    if (!CreateShaders(error))
        return false;
    if (!CreateStateObjects())
    {
        if (error) *error = "D3D11Renderer: failed to create state objects";
        return false;
    }

    m_cbPerFrame = CreateDynamicCB(m_device.Get(), sizeof(CBPerFrameLit));
    m_cbPerObject = CreateDynamicCB(m_device.Get(), sizeof(CBPerObjectLit));
    m_cbPerFrameUnlit = CreateDynamicCB(m_device.Get(), sizeof(CBPerFrameUnlit));
    m_cbPerObjectUnlit = CreateDynamicCB(m_device.Get(), sizeof(CBPerObjectUnlit));

    // 1x1 fallback textures, mirroring the defaults NifSkope's renderer
    // binds per sampler (renderer.cpp's white/black/gray/default_n): every
    // Sample() call in the lit shader always has a valid resource bound,
    // even before any texture ever loads successfully.
    {
        auto makeSolid = [this](std::uint32_t rgba, ComPtr<ID3D11ShaderResourceView>& out)
        {
            const D3D11_TEXTURE2D_DESC td {
                .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1,
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .SampleDesc = { .Count = 1 },
                .Usage = D3D11_USAGE_IMMUTABLE,
                .BindFlags = D3D11_BIND_SHADER_RESOURCE,
            };
            const D3D11_SUBRESOURCE_DATA sd { .pSysMem = &rgba, .SysMemPitch = sizeof(rgba) };
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(m_device->CreateTexture2D(&td, &sd, &tex)))
                m_device->CreateShaderResourceView(tex.Get(), nullptr, &out);
        };
        makeSolid(0xFFFFFFFFu, m_whiteTexSRV);      // diffuse/env-mask default
        makeSolid(0xFF000000u, m_blackTexSRV);       // glow/backlight default (no contribution)
        makeSolid(0xFFFF8080u, m_flatNormalSRV);     // (0.5,0.5,1,1) ABGR: flat normal + full spec mask; also NifSkope's default_n LightMask stand-in
        makeSolid(0xFFFF00FFu, m_missingTexSRV);     // magenta (1,0,1): a material's diffuse path that fails to RESOLVE renders loudly instead of blending in as white
    }

    BuildGridAndAxesGeometry();
    return true;
}

bool D3D11Renderer::CreateShaders(std::string* error)
{
    auto compile = [&](const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool
    {
        ComPtr<ID3DBlob> errBlob;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, &outBlob, &errBlob);
        if (FAILED(hr))
        {
            if (error)
            {
                *error = std::string("Shader compile failed (") + entry + "): ";
                if (errBlob)
                    *error += std::string(static_cast<const char*>(errBlob->GetBufferPointer()), errBlob->GetBufferSize());
            }
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> litVsBlob, litPsBlob, unlitVsBlob, unlitPsBlob;
    if (!compile(kLitShaderHLSL, "VSMain", "vs_5_0", litVsBlob)) return false;
    if (!compile(kLitShaderHLSL, "PSMain", "ps_5_0", litPsBlob)) return false;
    if (!compile(kUnlitShaderHLSL, "VSMain", "vs_5_0", unlitVsBlob)) return false;
    if (!compile(kUnlitShaderHLSL, "PSMain", "ps_5_0", unlitPsBlob)) return false;

    m_device->CreateVertexShader(litVsBlob->GetBufferPointer(), litVsBlob->GetBufferSize(), nullptr, &m_litVS);
    m_device->CreatePixelShader(litPsBlob->GetBufferPointer(), litPsBlob->GetBufferSize(), nullptr, &m_litPS);
    m_device->CreateVertexShader(unlitVsBlob->GetBufferPointer(), unlitVsBlob->GetBufferSize(), nullptr, &m_unlitVS);
    m_device->CreatePixelShader(unlitPsBlob->GetBufferPointer(), unlitPsBlob->GetBufferSize(), nullptr, &m_unlitPS);

    D3D11_INPUT_ELEMENT_DESC litLayoutDesc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex, uv),     D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, tangent), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(litLayoutDesc, 5, litVsBlob->GetBufferPointer(), litVsBlob->GetBufferSize(), &m_litLayout);

    D3D11_INPUT_ELEMENT_DESC unlitLayoutDesc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(LineVertex, pos),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(LineVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(unlitLayoutDesc, 2, unlitVsBlob->GetBufferPointer(), unlitVsBlob->GetBufferSize(), &m_unlitLayout);

    return true;
}

bool D3D11Renderer::CreateStateObjects()
{
    const D3D11_BLEND_DESC blendOpaqueDesc {
        .RenderTarget = { {
            .BlendEnable = FALSE,
            .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
        } },
    };
    m_device->CreateBlendState(&blendOpaqueDesc, &m_blendOpaque);

    const D3D11_BLEND_DESC blendAlphaDesc {
        .RenderTarget = { {
            .BlendEnable = TRUE,
            .SrcBlend = D3D11_BLEND_SRC_ALPHA,
            .DestBlend = D3D11_BLEND_INV_SRC_ALPHA,
            .BlendOp = D3D11_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D11_BLEND_ONE,
            .DestBlendAlpha = D3D11_BLEND_ZERO,
            .BlendOpAlpha = D3D11_BLEND_OP_ADD,
            .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
        } },
    };
    m_device->CreateBlendState(&blendAlphaDesc, &m_blendAlpha);

    const D3D11_DEPTH_STENCIL_DESC depthDesc {
        .DepthEnable = TRUE,
        .DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL,
        .DepthFunc = D3D11_COMPARISON_LESS_EQUAL,
    };
    m_device->CreateDepthStencilState(&depthDesc, &m_depthDefault);

    const D3D11_RASTERIZER_DESC rasterSolid {
        .FillMode = D3D11_FILL_SOLID,
        .CullMode = D3D11_CULL_BACK,
        // With the D3D viewport/projection convention used by this renderer,
        // NIF's outward-facing triangles rasterize clockwise.
        .FrontCounterClockwise = FALSE,
        .DepthClipEnable = TRUE,
    };
    m_device->CreateRasterizerState(&rasterSolid, &m_rasterSolid);

    D3D11_RASTERIZER_DESC rasterNoCull = rasterSolid;
    rasterNoCull.CullMode = D3D11_CULL_NONE; // SLSF2_Double_Sided materials
    m_device->CreateRasterizerState(&rasterNoCull, &m_rasterSolidNoCull);

    // SLSF1_Decal / SLSF1_Dynamic_Decal materials: pull the depth toward the
    // camera so a decal lying on (or a hair above) its base surface always
    // wins the depth test instead of z-fighting it - the D3D equivalent of
    // NifSkope's glPolygonOffset(-1.0, -1.0) (renderer.cpp). The constant
    // term is a few 24-bit-depth ulps rather than GL's single "smallest
    // resolvable difference" unit because the fixed 1..100000 near/far range
    // (NifViewport.cpp) leaves fewer effective depth bits than NifSkope's
    // bounds-fitted planes; visually the offset is still sub-unit at typical
    // framing distances.
    D3D11_RASTERIZER_DESC rasterDecal = rasterSolid;
    rasterDecal.DepthBias = -4;
    rasterDecal.SlopeScaledDepthBias = -1.0f;
    m_device->CreateRasterizerState(&rasterDecal, &m_rasterDecal);

    D3D11_RASTERIZER_DESC rasterDecalNoCull = rasterDecal;
    rasterDecalNoCull.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rasterDecalNoCull, &m_rasterDecalNoCull);

    D3D11_RASTERIZER_DESC rasterWire = rasterSolid;
    rasterWire.FillMode = D3D11_FILL_WIREFRAME;
    rasterWire.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rasterWire, &m_rasterWireframe);

    const D3D11_SAMPLER_DESC sampDesc {
        .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
        .AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
        .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    m_device->CreateSamplerState(&sampDesc, &m_sampler);

    return m_blendOpaque && m_blendAlpha && m_depthDefault && m_rasterSolid && m_rasterSolidNoCull
        && m_rasterDecal && m_rasterDecalNoCull && m_rasterWireframe && m_sampler;
}

bool D3D11Renderer::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;
    if (width == m_width && height == m_height && m_colorTex)
        return true;

    m_colorRTV.Reset(); m_colorSRV.Reset(); m_colorTex.Reset();
    m_depthDSV.Reset(); m_depthTex.Reset();

    const D3D11_TEXTURE2D_DESC colorDesc {
        .Width = width, .Height = height,
        .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM, // matches D2D's CreateBitmapFromDxgiSurface expectations (see NifViewport.cpp)
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    if (FAILED(m_device->CreateTexture2D(&colorDesc, nullptr, &m_colorTex)))
        return false;
    m_device->CreateRenderTargetView(m_colorTex.Get(), nullptr, &m_colorRTV);
    m_device->CreateShaderResourceView(m_colorTex.Get(), nullptr, &m_colorSRV);

    D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthTex)))
        return false;
    m_device->CreateDepthStencilView(m_depthTex.Get(), nullptr, &m_depthDSV);

    m_width = width;
    m_height = height;
    return true;
}

void D3D11Renderer::InvalidateMeshCache()
{
    m_meshCache.clear();
}

const D3D11Renderer::GpuMesh* D3D11Renderer::GetOrCreateGpuMesh(const NifGeometry* geometry)
{
    auto it = m_meshCache.find(geometry);
    if (it != m_meshCache.end())
        return &it->second;

    std::size_t n = geometry->positions.size();
    std::vector<Vertex> verts(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        Vertex& v = verts[i];
        v.pos[0] = geometry->positions[i][0]; v.pos[1] = geometry->positions[i][1]; v.pos[2] = geometry->positions[i][2];
        if (i < geometry->normals.size())
        {
            v.normal[0] = geometry->normals[i][0]; v.normal[1] = geometry->normals[i][1]; v.normal[2] = geometry->normals[i][2];
        }
        else
        {
            v.normal[0] = 0.0f; v.normal[1] = 0.0f; v.normal[2] = 1.0f;
        }
        if (i < geometry->uvs.size())
        {
            v.uv[0] = geometry->uvs[i][0]; v.uv[1] = geometry->uvs[i][1];
        }
        else
        {
            v.uv[0] = 0.0f; v.uv[1] = 0.0f;
        }
        if (i < geometry->colors.size())
        {
            v.color[0] = geometry->colors[i][0]; v.color[1] = geometry->colors[i][1];
            v.color[2] = geometry->colors[i][2]; v.color[3] = geometry->colors[i][3];
        }
        else
        {
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
        }
        if (i < geometry->tangents.size())
        {
            v.tangent[0] = geometry->tangents[i][0]; v.tangent[1] = geometry->tangents[i][1]; v.tangent[2] = geometry->tangents[i][2];
        }
        else
        {
            // Harmless placeholder: only sampled by the shader when
            // gHasNormalMap is set, which only happens when the material
            // actually resolved a normal texture - a mesh with no tangent
            // data in the file also has no meaningful normal map to apply.
            v.tangent[0] = 1.0f; v.tangent[1] = 0.0f; v.tangent[2] = 0.0f;
        }
    }

    std::vector<std::uint16_t> indices;
    indices.reserve(geometry->triangles.size() * 3);
    for (const Triangle& t : geometry->triangles)
    {
        indices.push_back(t[0]);
        indices.push_back(t[1]);
        indices.push_back(t[2]);
    }

    GpuMesh mesh;
    mesh.indexCount = static_cast<UINT>(indices.size());

    const D3D11_BUFFER_DESC vbDesc {
        .ByteWidth = static_cast<UINT>(sizeof(Vertex) * verts.size()),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_VERTEX_BUFFER,
    };
    const D3D11_SUBRESOURCE_DATA vbData { .pSysMem = verts.data() };
    m_device->CreateBuffer(&vbDesc, &vbData, &mesh.vertexBuffer);

    const D3D11_BUFFER_DESC ibDesc {
        .ByteWidth = static_cast<UINT>(sizeof(std::uint16_t) * indices.size()),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_INDEX_BUFFER,
    };
    const D3D11_SUBRESOURCE_DATA ibData { .pSysMem = indices.data() };
    m_device->CreateBuffer(&ibDesc, &ibData, &mesh.indexBuffer);

    auto [insertedIt, _] = m_meshCache.emplace(geometry, std::move(mesh));
    return &insertedIt->second;
}

void D3D11Renderer::BuildGridAndAxesGeometry()
{
    // Port of gltools.cpp's drawGrid()/drawAxes() immediate-mode
    // glBegin(GL_LINES) calls (phase3_tools): a flat 21x21 grid on the XZ
    // plane plus 3 colored axis lines, now baked into one static vertex
    // buffer of colored line segments instead of ~90 per-frame glVertex3f
    // calls.
    std::vector<LineVertex> verts;
    const int halfLines = 10;
    const float step = 20.0f;
    const float extent = halfLines * step;
    const float gridColor[4] = { 0.35f, 0.35f, 0.38f, 1.0f };

    for (int i = -halfLines; i <= halfLines; ++i)
    {
        float coord = static_cast<float>(i) * step;
        verts.push_back({ { coord, 0.0f, -extent }, { gridColor[0], gridColor[1], gridColor[2], gridColor[3] } });
        verts.push_back({ { coord, 0.0f,  extent }, { gridColor[0], gridColor[1], gridColor[2], gridColor[3] } });
        verts.push_back({ { -extent, 0.0f, coord }, { gridColor[0], gridColor[1], gridColor[2], gridColor[3] } });
        verts.push_back({ {  extent, 0.0f, coord }, { gridColor[0], gridColor[1], gridColor[2], gridColor[3] } });
    }
    m_gridVertexCount = static_cast<UINT>(verts.size());
    m_axesVertexStart = m_gridVertexCount;

    const float axisLen = extent * 0.5f;
    auto pushAxis = [&](float x, float y, float z, float r, float g, float b)
    {
        verts.push_back({ { 0.0f, 0.0f, 0.0f }, { r, g, b, 1.0f } });
        verts.push_back({ { x, y, z }, { r, g, b, 1.0f } });
    };
    pushAxis(axisLen, 0, 0, 1.0f, 0.2f, 0.2f); // X - red
    pushAxis(0, axisLen, 0, 0.2f, 1.0f, 0.2f); // Y - green
    pushAxis(0, 0, axisLen, 0.3f, 0.5f, 1.0f); // Z - blue
    m_axesVertexCount = static_cast<UINT>(verts.size()) - m_axesVertexStart;

    const D3D11_BUFFER_DESC desc {
        .ByteWidth = static_cast<UINT>(sizeof(LineVertex) * verts.size()),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_VERTEX_BUFFER,
    };
    const D3D11_SUBRESOURCE_DATA data { .pSysMem = verts.data() };
    m_device->CreateBuffer(&desc, &data, &m_gridAxesVB);
}

void D3D11Renderer::RenderScene(const std::vector<RenderMesh>& meshes, const RenderSettings& settings, TextureCache* textures)
{
    if (!m_colorRTV || !m_depthDSV)
        return;

    ID3D11RenderTargetView* rtvs[] = { m_colorRTV.Get() };
    m_context->OMSetRenderTargets(1, rtvs, m_depthDSV.Get());

    float clear[4] = { settings.clearColor[0], settings.clearColor[1], settings.clearColor[2], settings.clearColor[3] };
    m_context->ClearRenderTargetView(m_colorRTV.Get(), clear);
    m_context->ClearDepthStencilView(m_depthDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    const D3D11_VIEWPORT vp {
        .TopLeftX = 0.0f, .TopLeftY = 0.0f,
        .Width = static_cast<float>(m_width), .Height = static_cast<float>(m_height),
        .MinDepth = 0.0f, .MaxDepth = 1.0f,
    };
    m_context->RSSetViewports(1, &vp);

    Matrix4 viewProj = settings.proj * settings.view;

    m_context->OMSetDepthStencilState(m_depthDefault.Get(), 0);
    m_context->RSSetState(settings.wireframe ? m_rasterWireframe.Get() : m_rasterSolid.Get());

    // --- Grid + axes (unlit) ---
    if ((settings.showGrid || settings.showAxes) && m_gridAxesVB)
    {
        m_context->IASetInputLayout(m_unlitLayout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        UINT stride = sizeof(LineVertex), offset = 0;
        ID3D11Buffer* vb = m_gridAxesVB.Get();
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        m_context->VSSetShader(m_unlitVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_unlitPS.Get(), nullptr, 0);
        m_context->OMSetBlendState(m_blendOpaque.Get(), nullptr, 0xFFFFFFFF);

        CBPerFrameUnlit cbFrame;
        std::memcpy(cbFrame.viewProj, viewProj.data(), sizeof(cbFrame.viewProj));
        UploadDynamicCB(m_context.Get(), m_cbPerFrameUnlit.Get(), &cbFrame, sizeof(cbFrame));
        ID3D11Buffer* frameCb = m_cbPerFrameUnlit.Get();
        m_context->VSSetConstantBuffers(0, 1, &frameCb);

        Matrix4 identity;
        CBPerObjectUnlit cbObj;
        std::memcpy(cbObj.world, identity.data(), sizeof(cbObj.world));
        UploadDynamicCB(m_context.Get(), m_cbPerObjectUnlit.Get(), &cbObj, sizeof(cbObj));
        ID3D11Buffer* objCb = m_cbPerObjectUnlit.Get();
        m_context->VSSetConstantBuffers(1, 1, &objCb);

        if (settings.showGrid)
            m_context->Draw(m_gridVertexCount, 0);
        if (settings.showAxes)
            m_context->Draw(m_axesVertexCount, m_axesVertexStart);
    }

    // --- Meshes (lit) ---
    if (!meshes.empty())
    {
        m_context->IASetInputLayout(m_litLayout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_litVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_litPS.Get(), nullptr, 0);
        m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

        CBPerFrameLit cbFrame {};
        std::memcpy(cbFrame.viewProj, viewProj.data(), sizeof(cbFrame.viewProj));
        Vector3 lightDir = settings.lightDir; lightDir.normalize();
        cbFrame.lightDir[0] = lightDir[0]; cbFrame.lightDir[1] = lightDir[1]; cbFrame.lightDir[2] = lightDir[2];
        cbFrame.ambient = settings.ambient;
        cbFrame.eyePos[0] = settings.eyePos[0]; cbFrame.eyePos[1] = settings.eyePos[1]; cbFrame.eyePos[2] = settings.eyePos[2];
        cbFrame.brightness = settings.brightness;
        UploadDynamicCB(m_context.Get(), m_cbPerFrame.Get(), &cbFrame, sizeof(cbFrame));
        ID3D11Buffer* frameCb = m_cbPerFrame.Get();
        m_context->VSSetConstantBuffers(0, 1, &frameCb);
        m_context->PSSetConstantBuffers(0, 1, &frameCb);

        for (const RenderMesh& mesh : meshes)
        {
            if (!mesh.geometry)
                continue;
            const GpuMesh* gpu = GetOrCreateGpuMesh(mesh.geometry);
            if (!gpu || !gpu->vertexBuffer || gpu->indexCount == 0)
                continue;

            const NifMaterial& mat = mesh.material;

            CBPerObjectLit cbObj {};
            std::memcpy(cbObj.world, mesh.worldTransform.data(), sizeof(cbObj.world));
            cbObj.glowColor[0] = mat.emissiveColor[0] * mat.emissiveMultiple;
            cbObj.glowColor[1] = mat.emissiveColor[1] * mat.emissiveMultiple;
            cbObj.glowColor[2] = mat.emissiveColor[2] * mat.emissiveMultiple;
            cbObj.glowColor[3] = mat.alpha;
            cbObj.spec[0] = mat.specularColor[0];
            cbObj.spec[1] = mat.specularColor[1];
            cbObj.spec[2] = mat.specularColor[2];
            cbObj.spec[3] = mat.glossiness;
            cbObj.uvTransform[0] = mat.uvScale[0];
            cbObj.uvTransform[1] = mat.uvScale[1];
            cbObj.uvTransform[2] = mat.uvOffset[0];
            cbObj.uvTransform[3] = mat.uvOffset[1];
            cbObj.tintColor[0] = mat.tintColor[0];
            cbObj.tintColor[1] = mat.tintColor[1];
            cbObj.tintColor[2] = mat.tintColor[2];
            cbObj.tintColor[3] = mat.outerReflectionStrength;
            cbObj.params[0] = mat.specularStrength;
            cbObj.params[1] = mat.lightingEffect1;
            cbObj.params[2] = mat.lightingEffect2;
            cbObj.params[3] = mat.environmentReflection;
            cbObj.innerParams[0] = mat.innerTextureScale[0];
            cbObj.innerParams[1] = mat.innerTextureScale[1];
            cbObj.innerParams[2] = mat.innerThickness;
            cbObj.innerParams[3] = mat.outerRefractionStrength;
            cbObj.falloffParams[0] = mat.falloffStartAngle;
            cbObj.falloffParams[1] = mat.falloffStopAngle;
            cbObj.falloffParams[2] = mat.falloffStartOpacity;
            cbObj.falloffParams[3] = mat.falloffStopOpacity;
            if (mat.isEffectShader)
            {
                // Effect shader repurposes gGlowColor as Base Color (rgba)
                // and gParams.x as Base Color Scale - see Shaders.h.
                cbObj.glowColor[0] = mat.emissiveColor[0];
                cbObj.glowColor[1] = mat.emissiveColor[1];
                cbObj.glowColor[2] = mat.emissiveColor[2];
                cbObj.glowColor[3] = mat.effectEmissiveAlpha;
                cbObj.params[0] = mat.emissiveMultiple;
            }

            // Texture bindings + flag bits. The material's derived has*
            // booleans (see NifMaterial's struct comment) say what the
            // shader flags REQUEST; a texture-backed feature's bit is only
            // set once its texture actually resolves, so a failed load
            // degrades to the un-textured shading path instead of sampling
            // a fallback pretending to be real data.
            auto resolve = [&](const std::string& path, ID3D11ShaderResourceView* fallback) -> std::pair<ID3D11ShaderResourceView*, bool>
            {
                if (textures && !path.empty())
                    if (ID3D11ShaderResourceView* loaded = textures->GetOrLoad(path))
                        return { loaded, true };
                return { fallback, false };
            };
            const std::string kNone;

            auto [diffuseSrv, hasDiffuse] = resolve(mat.diffuseTexture, m_whiteTexSRV.Get());
            if (!hasDiffuse && !mat.diffuseTexture.empty())
            {
                // The material NAMES a diffuse texture but it didn't resolve
                // (missing loose file/archive entry or a failed decode):
                // sample the magenta marker instead of degrading to the
                // untextured white path, so missing content is unmissable.
                // Materials with no diffuse path at all keep the white
                // constant - that's authored, not broken.
                diffuseSrv = m_missingTexSRV.Get();
                hasDiffuse = true;
            }
            auto [normalSrv, hasNormal] = resolve(mat.normalTexture, m_flatNormalSRV.Get());
            auto [cubeSrv, hasCube] = resolve(mat.hasCubeMap ? mat.cubeTexture : kNone, nullptr);
            auto [envMaskSrv, hasEnvMask] = resolve(mat.useEnvironmentMask ? mat.envMaskTexture : kNone, m_whiteTexSRV.Get());

            // t3 is dual-purpose: the parallax height map (shader type 3) or
            // the face detail mask (type 4) - mutually exclusive by type.
            auto [heightSrv, hasHeight] = resolve(
                (mat.hasHeightMap || mat.hasDetailMask) ? mat.heightTexture : kNone, m_blackTexSRV.Get());

            // t7 likewise: the backlight map, or - for MSN materials without
            // backlight - the external specular map (NifSkope renderer.cpp
            // line 779's exact condition).
            const bool wantsSpecMap = mat.hasModelSpaceNormals && mat.hasSpecularMap && !mat.hasBacklight;
            auto [backlightSrv, hasBacklightTex] = resolve(
                (mat.hasBacklight || wantsSpecMap) ? mat.backlightTexture : kNone, m_blackTexSRV.Get());

            // Slot 2 is dual-purpose (see NifMaterial): the glow map when
            // hasGlowMap, the rim/soft LightMask otherwise. NifSkope's
            // LightMask fallback is default_n - flat normal color - which
            // m_flatNormalSRV reproduces.
            auto [glowSrv, hasGlowTex] = resolve(mat.hasGlowMap ? mat.glowTexture : kNone, m_blackTexSRV.Get());
            auto [lightMaskSrv, lightMaskLoaded] = resolve(!mat.hasGlowMap ? mat.glowTexture : kNone, m_flatNormalSRV.Get());
            (void)lightMaskLoaded; // rim/soft stay enabled with the fallback mask, like NifSkope

            // t8 is the per-family auxiliary slot: effect greyscale palette,
            // multilayer inner map, or face tint mask.
            const std::string& auxPath = mat.isEffectShader ? mat.greyscaleTexture
                : (mat.hasMultiLayerParallax || mat.hasTintMask) ? mat.innerTexture
                : kNone;
            auto [auxSrv, hasAux] = resolve(auxPath, m_whiteTexSRV.Get());

            std::uint32_t flags = 0;
            if (hasDiffuse)                           flags |= kLitHasDiffuse;
            if (hasNormal)                            flags |= kLitHasNormalMap;
            if (mat.hasGlowMap && hasGlowTex)         flags |= kLitHasGlowMap;
            if (mat.hasEmittance)                     flags |= kLitHasEmit;
            if (mat.hasSoftlight)                     flags |= kLitHasSoftlight;
            if (mat.hasRimlight)                      flags |= kLitHasRimlight;
            if (mat.hasBacklight && hasBacklightTex)  flags |= kLitHasBacklight;
            if (mat.hasCubeMap && hasCube)            flags |= kLitHasCubeMap;
            if (mat.useEnvironmentMask && hasEnvMask) flags |= kLitHasEnvMask;
            if (mat.hasHeightMap && hasHeight)        flags |= kLitHasHeightMap;
            if (mat.hasTintColor)                     flags |= kLitHasTintColor;
            if (mat.hasSpecular)                      flags |= kLitHasSpecular;
            if (mat.hasModelSpaceNormals)             flags |= kLitModelSpaceNormals;
            if (wantsSpecMap && hasBacklightTex)      flags |= kLitHasSpecularMap;
            if (mat.hasDetailMask && hasHeight)       flags |= kLitHasDetailMask;
            if (mat.hasTintMask && hasAux)            flags |= kLitHasTintMask;
            if (mat.hasMultiLayerParallax && hasAux)  flags |= kLitMultiLayer;
            if (mat.isEffectShader)
            {
                flags |= kLitIsEffect;
                if (mat.useFalloff)                   flags |= kLitEffectUseFalloff;
                if (mat.greyscaleColor && hasAux)     flags |= kLitEffectGreyscaleColor;
                if (mat.greyscaleAlpha && hasAux)     flags |= kLitEffectGreyscaleAlpha;
                if (mat.hasWeaponBlood)               flags |= kLitEffectWeaponBlood;
            }
            if (mat.hasAlphaTest)
            {
                flags |= kLitAlphaTest;
                cbObj.alphaTest = mat.alphaTestThreshold;
            }
            cbObj.flags = flags;

            ID3D11ShaderResourceView* srvs[9] = {
                diffuseSrv, normalSrv, glowSrv, heightSrv,
                // t4 is a TextureCube; when no cube resolved, bind null with
                // the cube flag cleared (a 2D SRV on a cube slot would be a
                // type mismatch the debug layer flags).
                hasCube ? cubeSrv : nullptr,
                envMaskSrv, lightMaskSrv, backlightSrv, auxSrv,
            };
            m_context->PSSetShaderResources(0, 9, srvs);

            UploadDynamicCB(m_context.Get(), m_cbPerObject.Get(), &cbObj, sizeof(cbObj));
            ID3D11Buffer* objCb = m_cbPerObject.Get();
            m_context->VSSetConstantBuffers(1, 1, &objCb);
            m_context->PSSetConstantBuffers(1, 1, &objCb);

            m_context->OMSetBlendState(mat.hasAlphaBlend ? m_blendAlpha.Get() : m_blendOpaque.Get(), nullptr, 0xFFFFFFFF);
            if (!settings.wireframe)
            {
                if (mat.isDecal)
                    m_context->RSSetState(mat.isDoubleSided ? m_rasterDecalNoCull.Get() : m_rasterDecal.Get());
                else
                    m_context->RSSetState(mat.isDoubleSided ? m_rasterSolidNoCull.Get() : m_rasterSolid.Get());
            }

            UINT stride = sizeof(Vertex), offset = 0;
            ID3D11Buffer* vb = gpu->vertexBuffer.Get();
            m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            m_context->IASetIndexBuffer(gpu->indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->DrawIndexed(gpu->indexCount, 0, 0);
        }
    }
}

} // namespace nsk
