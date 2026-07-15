// RenderTarget.h - one offscreen color+depth framebuffer.
//
// Split out of the old D3D11Renderer, which folded the framebuffer together
// with all the shared device-level resources (shaders/states/IBL) and so
// forced a full renderer per viewport. A RenderTarget is now the only
// per-view GPU-surface object: each NifViewport owns one, and item 12's
// background thumbnail renderer will own additional ones, all driven by the
// single shared RenderDevice. The color texture is B8G8R8A8 with
// RENDER_TARGET|SHADER_RESOURCE bind flags so NifViewport can wrap it as an
// ID2D1Bitmap1 over its DXGI surface with no CPU copy (see NifViewport.cpp).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace nsk
{

class RenderTarget
{
public:
    // (Re)creates the color+depth textures at the given pixel size. Safe to
    // call every frame; a no-op when the size is unchanged. Returns false on
    // a zero dimension or a device failure.
    bool Resize(ID3D11Device* device, UINT width, UINT height);

    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

    ID3D11Texture2D* ColorTexture() const { return m_colorTex.Get(); }
    ID3D11ShaderResourceView* ColorSRV() const { return m_colorSRV.Get(); }
    ID3D11RenderTargetView* ColorRTV() const { return m_colorRTV.Get(); }
    ID3D11DepthStencilView* DepthDSV() const { return m_depthDSV.Get(); }

    // Saves the color target's current contents (the last rendered frame, no
    // UI chrome) as a PNG via WIC - the calling thread must have COM
    // initialized. Alpha is forced opaque: the RT's alpha channel holds
    // blending residue, not coverage.
    bool SaveColorToPng(ID3D11Device* device, ID3D11DeviceContext* context,
                        const std::wstring& path, std::string* error = nullptr) const;

private:
    UINT m_width = 0;
    UINT m_height = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_colorRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_colorSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthDSV;
};

} // namespace nsk
