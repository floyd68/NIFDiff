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
    struct Vertex { float pos[3]; float normal[3]; float uv[2]; float color[4]; };
    struct LineVertex { float pos[3]; float color[4]; };

    struct CBPerFrameLit { float viewProj[16]; float lightDir[3]; float ambient; float eyePos[3]; float brightness; };
    struct CBPerObjectLit { float world[16]; float tint[4]; float spec[4]; std::int32_t hasTexture; float pad0[3]; };
    struct CBPerFrameUnlit { float viewProj[16]; };
    struct CBPerObjectUnlit { float world[16]; };

    static_assert(sizeof(CBPerFrameLit) == 96, "CBPerFrameLit must match Shaders.h PerFrame layout");
    static_assert(sizeof(CBPerObjectLit) == 112, "CBPerObjectLit must match Shaders.h PerObject layout");

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

    // 1x1 white texture: fallback SRV for meshes/materials with no resolved
    // diffuse texture (so the lit shader's Sample() call always has a valid
    // resource bound, even before any texture ever loads successfully).
    {
        std::uint32_t whitePixel = 0xFFFFFFFFu;
        const D3D11_TEXTURE2D_DESC td {
            .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { .Count = 1 },
            .Usage = D3D11_USAGE_IMMUTABLE,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };
        const D3D11_SUBRESOURCE_DATA sd { .pSysMem = &whitePixel, .SysMemPitch = sizeof(whitePixel) };
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(m_device->CreateTexture2D(&td, &sd, &tex)))
            m_device->CreateShaderResourceView(tex.Get(), nullptr, &m_whiteTexSRV);
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
    };
    m_device->CreateInputLayout(litLayoutDesc, 4, litVsBlob->GetBufferPointer(), litVsBlob->GetBufferSize(), &m_litLayout);

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
        .DepthClipEnable = TRUE,
    };
    m_device->CreateRasterizerState(&rasterSolid, &m_rasterSolid);

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

    return m_blendOpaque && m_blendAlpha && m_depthDefault && m_rasterSolid && m_rasterWireframe && m_sampler;
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

            CBPerObjectLit cbObj {};
            std::memcpy(cbObj.world, mesh.worldTransform.data(), sizeof(cbObj.world));
            cbObj.tint[0] = mesh.material.emissiveColor[0] * mesh.material.emissiveMultiple;
            cbObj.tint[1] = mesh.material.emissiveColor[1] * mesh.material.emissiveMultiple;
            cbObj.tint[2] = mesh.material.emissiveColor[2] * mesh.material.emissiveMultiple;
            cbObj.tint[3] = mesh.material.alpha;
            cbObj.spec[0] = mesh.material.specularColor[0];
            cbObj.spec[1] = mesh.material.specularColor[1];
            cbObj.spec[2] = mesh.material.specularColor[2];
            cbObj.spec[3] = 0.0f;

            ID3D11ShaderResourceView* srv = m_whiteTexSRV.Get();
            cbObj.hasTexture = 0;
            if (textures && !mesh.material.diffuseTexture.empty())
            {
                if (ID3D11ShaderResourceView* loaded = textures->GetOrLoad(mesh.material.diffuseTexture))
                {
                    srv = loaded;
                    cbObj.hasTexture = 1;
                }
            }
            m_context->PSSetShaderResources(0, 1, &srv);

            UploadDynamicCB(m_context.Get(), m_cbPerObject.Get(), &cbObj, sizeof(cbObj));
            ID3D11Buffer* objCb = m_cbPerObject.Get();
            m_context->VSSetConstantBuffers(1, 1, &objCb);
            m_context->PSSetConstantBuffers(1, 1, &objCb);

            m_context->OMSetBlendState(mesh.material.hasAlphaBlend ? m_blendAlpha.Get() : m_blendOpaque.Get(), nullptr, 0xFFFFFFFF);

            UINT stride = sizeof(Vertex), offset = 0;
            ID3D11Buffer* vb = gpu->vertexBuffer.Get();
            m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            m_context->IASetIndexBuffer(gpu->indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->DrawIndexed(gpu->indexCount, 0, 0);
        }
    }
}

} // namespace nsk
