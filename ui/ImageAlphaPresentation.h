#pragma once

#include "ImageCore/DecodedImage.h"

#include <cstdint>
#include <vector>

namespace nsk
{

// Decoder-reported alpha facts retained alongside presentation resources. The
// encoding describes storage; usage is resolved separately by app policy.
struct ImageAlphaInfo
{
    ImageCore::AlphaEncoding encoding { ImageCore::AlphaEncoding::Unknown };
    ImageCore::AlphaUsage usageHint { ImageCore::AlphaUsage::Auto };
    bool sourceWasBlockCompressed { false };
};

ImageAlphaInfo AlphaInfoFromDecodedImage(const ImageCore::DecodedImage& image);

// Bethesda-friendly Auto policy. Explicit overrides win, followed by decoder
// hints; block-compressed straight/unknown alpha defaults to data, while loose
// and uncompressed images default to transparency coverage.
ImageCore::AlphaUsage ResolveAlphaUsage(
    const ImageAlphaInfo& alpha,
    ImageCore::AlphaUsage overrideUsage = ImageCore::AlphaUsage::Auto);

// Build a D2D-ready PREMULTIPLIED BGRA8 copy without modifying the decoded
// source. Coverage premultiplies straight RGB; Data restores premultiplied RGB
// to straight where possible and forces display alpha opaque.
std::vector<std::uint8_t> BuildBgra8Presentation(
    const ImageCore::DecodedImage& image,
    ImageCore::AlphaUsage usage);

} // namespace nsk
