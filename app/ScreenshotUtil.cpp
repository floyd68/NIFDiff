#include "ScreenshotUtil.h"

#include <Backplate.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace nsk::ScreenshotUtil
{

bool SaveRenderSurfaceRectPng(
    FD2D::Backplate& backplate,
    const D2D1_RECT_F& logicalRect,
    const std::wstring& pngPath)
{
    if (pngPath.empty())
    {
        return false;
    }

    // Refresh the application-owned offscreen frame, then read it back. Unlike
    // desktop/window DC capture this cannot include pixels from occluding apps.
    backplate.Render();

    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    if (FAILED(backplate.ReadComposedPixels(
            logicalRect,
            pixels,
            width,
            height,
            stride)) ||
        pixels.empty() ||
        width == 0 ||
        height == 0)
    {
        return false;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(result) || !factory)
    {
        return false;
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (FAILED(result) || !stream)
    {
        return false;
    }
    result = stream->InitializeFromFilename(
        pngPath.c_str(),
        GENERIC_WRITE);
    if (FAILED(result))
    {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(
        GUID_ContainerFormatPng,
        nullptr,
        &encoder);
    if (FAILED(result) || !encoder)
    {
        return false;
    }
    result = encoder->Initialize(
        stream.Get(),
        WICBitmapEncoderNoCache);
    if (FAILED(result))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = encoder->CreateNewFrame(
        &frame,
        &properties);
    if (FAILED(result) || !frame)
    {
        return false;
    }
    result = frame->Initialize(properties.Get());
    if (FAILED(result))
    {
        return false;
    }
    result = frame->SetSize(width, height);
    if (FAILED(result))
    {
        return false;
    }

    WICPixelFormatGUID format =
        GUID_WICPixelFormat32bppBGRA;
    result = frame->SetPixelFormat(&format);
    if (FAILED(result) ||
        !IsEqualGUID(
            format,
            GUID_WICPixelFormat32bppBGRA))
    {
        return false;
    }

    result = frame->WritePixels(
        height,
        stride,
        static_cast<UINT>(pixels.size()),
        pixels.data());
    if (FAILED(result))
    {
        return false;
    }
    result = frame->Commit();
    if (FAILED(result))
    {
        return false;
    }
    return SUCCEEDED(encoder->Commit());
}

}
