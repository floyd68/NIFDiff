#pragma once

#include <d3d11.h>
#include <cstdint>

namespace nsk
{

DXGI_FORMAT DefaultTypedTextureSrvFormat(DXGI_FORMAT format);

bool IsTexturePublicationCurrent(
    ID3D11Device* decodedDevice,
    std::uint64_t decodedDeviceGeneration,
    std::uint64_t decodedPublicationGeneration,
    ID3D11Device* currentDevice,
    std::uint64_t currentDeviceGeneration,
    std::uint64_t currentPublicationGeneration);

}
