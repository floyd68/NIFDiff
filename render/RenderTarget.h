// RenderTarget.h - one offscreen color+depth framebuffer, optionally MSAA.
//
// Split out of the old D3D11Renderer, which folded the framebuffer together
// with all the shared device-level resources (shaders/states/IBL) and so
// forced a full renderer per viewport. A RenderTarget is now the only
// per-view GPU-surface object: each NifViewport owns one, and item 12's
// background thumbnail renderer will own additional ones, all driven by the
// single shared RenderDevice.
//
// MSAA: when a sample count > 1 is requested (and the device supports it),
// the scene renders into multisampled color+depth and is resolved into a
// single-sample color texture. That single-sample texture is always the one
// exposed by ColorTexture()/ColorSRV(): it is B8G8R8A8 with
// RENDER_TARGET|SHADER_RESOURCE bind flags so NifViewport can wrap it as an
// ID2D1Bitmap1 over its DXGI surface with no CPU copy (see NifViewport.cpp).
// ColorRTV()/DepthDSV() return the render (multisampled) views; the owner
// must call Resolve() after drawing so the single-sample texture is current.
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
    // (Re)creates the color+depth textures at the given pixel size and sample
    // count (clamped to what the device supports; 1 = no MSAA). Safe to call
    // every frame; a no-op when size and sample count are unchanged. Returns
    // false on a zero dimension or a device failure.
    bool Resize(ID3D11Device* device, UINT width, UINT height, UINT sampleCount = 1);

    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    UINT SampleCount() const { return m_sampleCount; }

    // Single-sample, resolved color: what D2D wraps and screenshots read.
    ID3D11Texture2D* ColorTexture() const { return m_colorTex.Get(); }
    ID3D11ShaderResourceView* ColorSRV() const { return m_colorSRV.Get(); }

    // Render targets to bind (multisampled when SampleCount() > 1).
    ID3D11RenderTargetView* ColorRTV() const { return m_renderColorRTV.Get(); }
    ID3D11DepthStencilView* DepthDSV() const { return m_depthDSV.Get(); }

    // Resolves the multisampled color into the single-sample ColorTexture().
    // No-op when SampleCount() == 1 (rendering went straight to it). Call
    // after all draws, before the color is consumed by D2D / a screenshot.
    void Resolve(ID3D11DeviceContext* context) const;

    // Saves the (resolved) color target's current contents as a PNG via WIC -
    // the calling thread must have COM initialized. Alpha is forced opaque:
    // the RT's alpha channel holds blending residue, not coverage.
    bool SaveColorToPng(ID3D11Device* device, ID3D11DeviceContext* context,
                        const std::wstring& path, std::string* error = nullptr) const;

private:
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_sampleCount = 1;

    // Single-sample resolve/present target (always present).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_colorSRV;
    // Multisampled color (only when m_sampleCount > 1); null otherwise.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_msaaColorTex;
    // The RTV the scene renders into: the MSAA color when multisampling, else
    // the single-sample color directly.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderColorRTV;
    // Depth matches the render color's sample count.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthDSV;
};

} // namespace nsk
