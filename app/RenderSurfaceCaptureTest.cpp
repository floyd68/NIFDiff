#include <Application.h>
#include <Backplate.h>
#include <ShaderResourcePresenter.h>
#include <Wnd.h>

#include "TestSupport.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{

class UnboundPresenterTest final : public FD2D::Wnd
{
public:
    UnboundPresenterTest()
        : FD2D::Wnd(L"UnboundPresenter")
    {
    }

    void OnRenderD3D(
        ID3D11DeviceContext* context) override
    {
        FD2D::Backplate* backplate =
            BackplateRef();
        if (!context || !backplate)
        {
            return;
        }

        if (!m_srv)
        {
            const std::uint32_t redPixels[4] =
            {
                0xFFFF0000,
                0xFFFF0000,
                0xFFFF0000,
                0xFFFF0000
            };
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = 2;
            desc.Height = 2;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data {};
            data.pSysMem = redPixels;
            data.SysMemPitch = sizeof(std::uint32_t) * 2;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(backplate->D3DDevice()->CreateTexture2D(
                    &desc,
                    &data,
                    &texture)) ||
                FAILED(backplate->D3DDevice()->CreateShaderResourceView(
                    texture.Get(),
                    nullptr,
                    &m_srv)))
            {
                return;
            }
        }

        // Reproduce a child renderer leaving the shared context without an
        // output target and with a private viewport. The presenter must bind
        // Backplate's composed target and full-surface viewport itself.
        ID3D11RenderTargetView* noTarget = nullptr;
        context->OMSetRenderTargets(
            1,
            &noTarget,
            nullptr);
        const D3D11_VIEWPORT privateViewport
        {
            0.0f,
            0.0f,
            2.0f,
            2.0f,
            0.0f,
            1.0f
        };
        context->RSSetViewports(
            1,
            &privateViewport);

        FD2D::ShaderResourceDraw draw;
        draw.layout =
            D2D1::RectF(
                24.0f,
                16.0f,
                72.0f,
                48.0f);
        draw.contentWidth = 2;
        draw.contentHeight = 2;
        draw.sourceAlphaUsage = 1;
        // CHECK, not REQUIRE: this runs inside Backplate::Render, so it must
        // not throw; a failed draw still shows up in the pixel checks below.
        CHECK(SUCCEEDED(FD2D::DrawShaderResource(
            context,
            *backplate,
            m_srv.Get(),
            draw)));
    }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
};

}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);

    auto& app = FD2D::Application::Instance();
    std::shared_ptr<FD2D::Backplate> backplate;
    const int exitCode = nsk::test::Run([&]
    {
        FD2D::InitContext context {};
        context.instance = GetModuleHandleW(nullptr);
        REQUIRE(SUCCEEDED(app.Initialize(context)));

        FD2D::WindowOptions options {};
        options.instance = context.instance;
        options.title = L"RenderSurfaceCaptureTest";
        options.width = 96;
        options.height = 64;
        backplate =
            app.CreateWindowedBackplate(
                L"capture",
                options);
        REQUIRE(backplate);

        backplate->SetClearColor(
            D2D1::ColorF(
                0.20f,
                0.40f,
                0.80f,
                1.0f));
        backplate->AddWnd(
            std::make_shared<UnboundPresenterTest>());
        backplate->Render();

        std::vector<std::uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        UINT stride = 0;
        REQUIRE(SUCCEEDED(backplate->ReadComposedPixels(
            D2D1::RectF(4.0f, 4.0f, 20.0f, 20.0f),
            pixels,
            width,
            height,
            stride)));
        // The pixel indexing below relies on these dimensions.
        REQUIRE(width == 16);
        REQUIRE(height == 16);
        REQUIRE(stride == width * 4);
        REQUIRE(pixels.size() ==
            static_cast<std::size_t>(stride) * height);

        const std::size_t center =
            static_cast<std::size_t>(height / 2) * stride +
            static_cast<std::size_t>(width / 2) * 4;
        CHECK(pixels[center + 0] >= 200 &&
            pixels[center + 0] <= 210);
        CHECK(pixels[center + 1] >= 98 &&
            pixels[center + 1] <= 106);
        CHECK(pixels[center + 2] >= 47 &&
            pixels[center + 2] <= 55);
        CHECK(pixels[center + 3] == 255);

        pixels.clear();
        REQUIRE(SUCCEEDED(backplate->ReadComposedPixels(
            D2D1::RectF(40.0f, 24.0f, 56.0f, 40.0f),
            pixels,
            width,
            height,
            stride)));
        REQUIRE(pixels.size() ==
            static_cast<std::size_t>(stride) * height);
        const std::size_t presenterCenter =
            static_cast<std::size_t>(height / 2) * stride +
            static_cast<std::size_t>(width / 2) * 4;
        CHECK(pixels[presenterCenter + 0] <= 4);
        CHECK(pixels[presenterCenter + 1] <= 4);
        CHECK(pixels[presenterCenter + 2] >= 250);
        CHECK(pixels[presenterCenter + 3] == 255);
    });

    backplate.reset();
    app.Shutdown();
    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return exitCode;
}
