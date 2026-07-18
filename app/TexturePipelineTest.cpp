#include "../render/TexturePipelineState.h"

#include <cassert>

int main()
{
    using namespace nsk;

    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC1_TYPELESS) ==
           DXGI_FORMAT_BC1_UNORM);
    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC5_TYPELESS) ==
           DXGI_FORMAT_BC5_UNORM);
    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC6H_TYPELESS) ==
           DXGI_FORMAT_BC6H_UF16);
    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC7_TYPELESS) ==
           DXGI_FORMAT_BC7_UNORM);
    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
           DXGI_FORMAT_R8G8B8A8_UNORM);
    assert(DefaultTypedTextureSrvFormat(DXGI_FORMAT_BC3_UNORM_SRGB) ==
           DXGI_FORMAT_BC3_UNORM_SRGB);

    auto* deviceA = reinterpret_cast<ID3D11Device*>(0x1000);
    auto* deviceB = reinterpret_cast<ID3D11Device*>(0x2000);
    assert(IsTexturePublicationCurrent(deviceA, 4, 9, deviceA, 4, 9));
    assert(!IsTexturePublicationCurrent(deviceA, 3, 9, deviceA, 4, 9));
    assert(!IsTexturePublicationCurrent(deviceA, 4, 8, deviceA, 4, 9));
    assert(!IsTexturePublicationCurrent(deviceA, 4, 9, deviceB, 4, 9));
    assert(!IsTexturePublicationCurrent(nullptr, 4, 9, nullptr, 4, 9));

    return 0;
}
