// ImageViewTest.cpp - P1 validation harness for the FICture2 texture-view port.
//
// Opens a window and displays one image decoded by ImageCore through
// FD2D::Image. It proves the ported decode + display stack builds and runs
// inside NIFDiff's toolchain BEFORE the larger pane-abstraction work (P2+):
// ImageCore (WIC + DirectXTex) decodes to CPU BGRA8, and FD2D::Image (already
// present in NIFDiff's FD2D) renders it aspect-fit. Not shipped.
//
// Usage: ImageViewTest <path-to-image>   (.dds/.png/.tga/.jpg/...)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#include <wrl/client.h>
#include <d2d1.h>

#include <Application.h>
#include <Backplate.h>
#include <Image.h>

#include "ImageCore/ImageCore.h"       // RegisterBuiltInDecoders
#include "ImageCore/ImageLoader.h"     // ImageLoader::Instance().RequestDecoded
#include "ImageCore/ImageRequest.h"
#include "ImageCore/DecodedImage.h"

#include <future>
#include <memory>
#include <string>
#include <utility>

namespace
{
    // FD2D::Image wants a ready ID2D1Bitmap, but a bitmap can only be created
    // once we hold a render target (OnRender time). Stage the decoded CPU BGRA8
    // payload and upload it on the first render that has the target.
    class DecodedImageView : public FD2D::Image
    {
    public:
        explicit DecodedImageView(const std::wstring& name) : FD2D::Image(name) {}

        void SetPayload(ImageCore::DecodedImage img)
        {
            m_payload = std::move(img);
            m_pending = m_payload.blocks && !m_payload.blocks->empty();
            Invalidate();
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            if (m_pending && target)
            {
                const uint32_t pitch = m_payload.rowPitchBytes ? m_payload.rowPitchBytes
                                                               : m_payload.width * 4;
                const D2D1_SIZE_U size { m_payload.width, m_payload.height };
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
                if (SUCCEEDED(target->CreateBitmap(size, m_payload.blocks->data(), pitch, props, &bmp)))
                    SetBitmap(bmp);
                m_pending = false;
            }
            FD2D::Image::OnRender(target);
        }

    private:
        ImageCore::DecodedImage m_payload {};
        bool m_pending { false };
    };

    // Decode `path` to CPU BGRA8, synchronously (block on the async loader).
    HRESULT DecodeToBgra8(const std::wstring& path, ImageCore::DecodedImage& out)
    {
        ImageCore::ImageRequest req(path, ImageCore::ImagePurpose::FullResolution);
        req.allowGpuCompressedDDS = false; // force CPU-displayable BGRA8
        req.srgb = true;

        std::promise<std::pair<HRESULT, ImageCore::DecodedImage>> prom;
        auto fut = prom.get_future();
        ImageCore::ImageLoader::Instance().RequestDecoded(
            req, [&prom](HRESULT hr, ImageCore::DecodedImage img)
            { prom.set_value({ hr, std::move(img) }); });

        auto result = fut.get();
        out = std::move(result.second);
        return result.first;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring path = (argv && argc > 1) ? argv[1] : std::wstring();
    if (argv) LocalFree(argv);
    if (path.empty())
    {
        MessageBoxW(nullptr, L"Usage: ImageViewTest <path-to-image>", L"ImageViewTest", MB_OK);
        CoUninitialize();
        return 2;
    }

    auto& app = FD2D::Application::Instance();
    FD2D::InitContext ic {};
    ic.instance = hInstance;
    if (FAILED(app.Initialize(ic)))
    {
        CoUninitialize();
        return -1;
    }

    ImageCore::RegisterBuiltInDecoders();

    ImageCore::DecodedImage decoded;
    const HRESULT hr = DecodeToBgra8(path, decoded);

    FD2D::WindowOptions opts {};
    opts.instance = hInstance;
    opts.title = L"ImageViewTest";
    opts.width = 1200;
    opts.height = 800;
    auto backplate = app.CreateWindowedBackplate(L"main", opts);
    if (!backplate)
    {
        ImageCore::ImageLoader::Instance().Shutdown();
        app.Shutdown();
        CoUninitialize();
        return -1;
    }

    auto view = std::make_shared<DecodedImageView>(L"img");
    if (SUCCEEDED(hr) && decoded.blocks && !decoded.blocks->empty())
    {
        wchar_t title[512];
        swprintf_s(title, L"ImageViewTest - %ux%u  %s", decoded.width, decoded.height, path.c_str());
        SetWindowTextW(backplate->Window(), title);
        view->SetPayload(std::move(decoded));
    }
    else
    {
        wchar_t msg[600];
        swprintf_s(msg, L"Decode FAILED (hr=0x%08lX)\n%s", static_cast<unsigned long>(hr), path.c_str());
        MessageBoxW(nullptr, msg, L"ImageViewTest", MB_OK | MB_ICONERROR);
    }
    backplate->AddWnd(view);
    backplate->Show(nCmdShow);

    const int rc = app.RunMessageLoop();

    ImageCore::ImageLoader::Instance().Shutdown();
    app.Shutdown();
    CoUninitialize();
    return rc;
}
