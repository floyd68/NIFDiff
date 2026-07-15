#include "RenderTarget.h"

#include <DirectXTex.h>
#include <cstdint>

namespace nsk
{

bool RenderTarget::Resize(ID3D11Device* device, UINT width, UINT height)
{
    if (!device || width == 0 || height == 0)
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
    if (FAILED(device->CreateTexture2D(&colorDesc, nullptr, &m_colorTex)))
        return false;
    device->CreateRenderTargetView(m_colorTex.Get(), nullptr, &m_colorRTV);
    device->CreateShaderResourceView(m_colorTex.Get(), nullptr, &m_colorSRV);

    D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, &m_depthTex)))
        return false;
    device->CreateDepthStencilView(m_depthTex.Get(), nullptr, &m_depthDSV);

    m_width = width;
    m_height = height;
    return true;
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
