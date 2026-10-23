#include "../render/TexturePipelineState.h"

#include "TestSupport.h"

int main()
{
    return nsk::test::Run([]
    {
        using namespace nsk;

        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC1_TYPELESS) ==
              DXGI_FORMAT_BC1_UNORM);
        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC5_TYPELESS) ==
              DXGI_FORMAT_BC5_UNORM);
        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC6H_TYPELESS) ==
              DXGI_FORMAT_BC6H_UF16);
        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC7_TYPELESS) ==
              DXGI_FORMAT_BC7_UNORM);
        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
              DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC3_UNORM_SRGB) ==
              DXGI_FORMAT_BC3_UNORM_SRGB);

        auto* deviceA = reinterpret_cast<ID3D11Device*>(0x1000);
        auto* deviceB = reinterpret_cast<ID3D11Device*>(0x2000);
        CHECK(IsTexturePublicationCurrent(deviceA, 4, 9, deviceA, 4, 9));
        CHECK(!IsTexturePublicationCurrent(deviceA, 3, 9, deviceA, 4, 9));
        CHECK(!IsTexturePublicationCurrent(deviceA, 4, 8, deviceA, 4, 9));
        CHECK(!IsTexturePublicationCurrent(deviceA, 4, 9, deviceB, 4, 9));
        CHECK(!IsTexturePublicationCurrent(nullptr, 4, 9, nullptr, 4, 9));
    });
}
