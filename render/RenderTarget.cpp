#include "RenderTarget.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cstdint>

namespace nsk
{

bool RenderTarget::Resize(ID3D11Device* device, UINT width, UINT height, UINT sampleCount)
{
    if (!device || width == 0 || height == 0)
        return false;

    // Clamp the request to what the device actually supports for this format
    // (fall back toward 1x). B8G8R8A8 4x is universal on D3D11 hardware, but
    // check rather than assume.
    constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT samples = (std::max)(1u, sampleCount);
    while (samples > 1)
    {
        UINT quality = 0;
        if (SUCCEEDED(device->CheckMultisampleQualityLevels(kColorFormat, samples, &quality)) && quality > 0)
            break;
        samples /= 2;
    }

    if (width == m_width && height == m_height && samples == m_sampleCount && m_colorTex)
        return true;

    m_renderColorRTV.Reset(); m_colorSRV.Reset(); m_colorTex.Reset();
    m_msaaColorTex.Reset();
    m_depthDSV.Reset(); m_depthTex.Reset();

    // Single-sample resolve/present color: what D2D wraps + screenshots read.
    const D3D11_TEXTURE2D_DESC colorDesc {
        .Width = width, .Height = height,
        .MipLevels = 1, .ArraySize = 1,
        .Format = kColorFormat, // matches D2D's CreateBitmapFromDxgiSurface expectations
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    if (FAILED(device->CreateTexture2D(&colorDesc, nullptr, &m_colorTex)))
        return false;
    device->CreateShaderResourceView(m_colorTex.Get(), nullptr, &m_colorSRV);

    if (samples > 1)
    {
        // Multisampled color the scene renders into; resolved into m_colorTex.
        D3D11_TEXTURE2D_DESC msaaDesc = colorDesc;
        msaaDesc.SampleDesc.Count = samples;
        msaaDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&msaaDesc, nullptr, &m_msaaColorTex)))
            return false;
        if (FAILED(device->CreateRenderTargetView(m_msaaColorTex.Get(), nullptr, &m_renderColorRTV)))
            return false;
    }
    else
    {
        // No MSAA: render straight into the single-sample color.
        if (FAILED(device->CreateRenderTargetView(m_colorTex.Get(), nullptr, &m_renderColorRTV)))
            return false;
    }

    // Depth matches the render color's sample count.
    D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = samples;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, &m_depthTex)))
        return false;
    if (FAILED(device->CreateDepthStencilView(m_depthTex.Get(), nullptr, &m_depthDSV)))
        return false;

    m_width = width;
    m_height = height;
    m_sampleCount = samples;
    return true;
}

void RenderTarget::Resolve(ID3D11DeviceContext* context) const
{
    if (context && m_sampleCount > 1 && m_msaaColorTex && m_colorTex)
        context->ResolveSubresource(m_colorTex.Get(), 0, m_msaaColorTex.Get(), 0,
                                    DXGI_FORMAT_B8G8R8A8_UNORM);
}

bool RenderTarget::SaveColorToPng(ID3D11Device* device, ID3D11DeviceContext* context,
                                  const std::wstring& path, std::string* error) const
{
    if (!device || !context || !m_colorTex)
    {
        if (error) *error = "no rendered frame to save";
        return false;
    }

    DirectX::ScratchImage captured;
    HRESULT hr = DirectX::CaptureTexture(device, context, m_colorTex.Get(), captured);
    if (FAILED(hr) || captured.GetImageCount() == 0)
    {
        if (error) *error = "CaptureTexture failed";
        return false;
    }

    // B8G8R8A8 readback: stamp the alpha byte opaque so the PNG shows what
    // the viewport shows instead of ghosting where blended draws left
    // partial alpha behind.
    const DirectX::Image* img = captured.GetImage(0, 0, 0);
    for (std::size_t y = 0; y < img->height; ++y)
    {
        std::uint8_t* row = img->pixels + y * img->rowPitch;
        for (std::size_t x = 0; x < img->width; ++x)
            row[x * 4 + 3] = 0xFF;
    }

    hr = DirectX::SaveToWICFile(*img, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str());
    if (FAILED(hr))
    {
        if (error) *error = "SaveToWICFile failed";
        return false;
    }
    return true;
}

} // namespace nsk
