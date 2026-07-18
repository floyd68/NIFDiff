#include <Application.h>
#include <Backplate.h>

#include <cassert>
#include <cstdint>
#include <vector>

int main()
{
    const HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);

    auto& app = FD2D::Application::Instance();
    FD2D::InitContext context {};
    context.instance = GetModuleHandleW(nullptr);
    assert(SUCCEEDED(app.Initialize(context)));

    FD2D::WindowOptions options {};
    options.instance = context.instance;
    options.title = L"RenderSurfaceCaptureTest";
    options.width = 96;
    options.height = 64;
    auto backplate =
        app.CreateWindowedBackplate(
            L"capture",
            options);
    assert(backplate);

    backplate->SetClearColor(
        D2D1::ColorF(
            0.20f,
            0.40f,
            0.80f,
            1.0f));
    backplate->Render();

    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    assert(SUCCEEDED(backplate->ReadComposedPixels(
        D2D1::RectF(4.0f, 4.0f, 20.0f, 20.0f),
        pixels,
        width,
        height,
        stride)));
    assert(width == 16);
    assert(height == 16);
    assert(stride == width * 4);
    assert(pixels.size() ==
        static_cast<std::size_t>(stride) * height);

    const std::size_t center =
        static_cast<std::size_t>(height / 2) * stride +
        static_cast<std::size_t>(width / 2) * 4;
    assert(pixels[center + 0] >= 200 &&
        pixels[center + 0] <= 210);
    assert(pixels[center + 1] >= 98 &&
        pixels[center + 1] <= 106);
    assert(pixels[center + 2] >= 47 &&
        pixels[center + 2] <= 55);
    assert(pixels[center + 3] == 255);

    backplate.reset();
    app.Shutdown();
    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return 0;
}
