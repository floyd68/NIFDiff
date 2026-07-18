#include "ImageAlphaPresentation.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace nsk
{

ImageAlphaInfo AlphaInfoFromDecodedImage(const ImageCore::DecodedImage& image)
{
    return
    {
        image.alphaEncoding,
        image.alphaUsageHint,
        image.sourceWasBlockCompressed,
    };
}

ImageCore::AlphaUsage ResolveAlphaUsage(
    const ImageAlphaInfo& alpha,
    ImageCore::AlphaUsage overrideUsage)
{
    using Encoding = ImageCore::AlphaEncoding;
    using Usage = ImageCore::AlphaUsage;

    if (overrideUsage != Usage::Auto)
    {
        return overrideUsage;
    }
    if (alpha.usageHint != Usage::Auto)
    {
        return alpha.usageHint;
    }
    if (alpha.encoding == Encoding::Opaque)
    {
        return Usage::Data;
    }
    if (alpha.sourceWasBlockCompressed &&
        (alpha.encoding == Encoding::Straight || alpha.encoding == Encoding::Unknown))
    {
        return Usage::Data;
    }
    return Usage::Coverage;
}

std::vector<std::uint8_t> BuildBgra8Presentation(
    const ImageCore::DecodedImage& image,
    ImageCore::AlphaUsage usage)
{
    if (image.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        image.width == 0 || image.height == 0 ||
        !image.blocks || image.blocks->empty())
    {
        return {};
    }

    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4u;
    const std::size_t pitch = image.rowPitchBytes != 0
        ? static_cast<std::size_t>(image.rowPitchBytes)
        : rowBytes;
    if (pitch < rowBytes ||
        image.height > (std::numeric_limits<std::size_t>::max)() / pitch)
    {
        return {};
    }

    const std::size_t requiredBytes = pitch * static_cast<std::size_t>(image.height);
    if (image.blocks->size() < requiredBytes)
    {
        return {};
    }

    std::vector<std::uint8_t> out(
        image.blocks->begin(),
        image.blocks->begin() + requiredBytes);

    const bool data = usage == ImageCore::AlphaUsage::Data;
    const bool premultiplied =
        image.alphaEncoding == ImageCore::AlphaEncoding::Premultiplied;

    for (std::size_t y = 0; y < image.height; ++y)
    {
        std::uint8_t* row = out.data() + y * pitch;
        for (std::size_t x = 0; x < image.width; ++x)
        {
            std::uint8_t& b = row[x * 4u];
            std::uint8_t& g = row[x * 4u + 1u];
            std::uint8_t& r = row[x * 4u + 2u];
            std::uint8_t& a = row[x * 4u + 3u];

            if (data)
            {
                if (premultiplied && a != 0)
                {
                    const auto unpremultiply = [a](std::uint8_t color)
                    {
                        const unsigned straight =
                            (static_cast<unsigned>(color) * 255u + a / 2u) / a;
                        return static_cast<std::uint8_t>((std::min)(straight, 255u));
                    };
                    b = unpremultiply(b);
                    g = unpremultiply(g);
                    r = unpremultiply(r);
                }
                a = 255;
            }
            else if (!premultiplied)
            {
                b = static_cast<std::uint8_t>((static_cast<unsigned>(b) * a + 127u) / 255u);
                g = static_cast<std::uint8_t>((static_cast<unsigned>(g) * a + 127u) / 255u);
                r = static_cast<std::uint8_t>((static_cast<unsigned>(r) * a + 127u) / 255u);
            }
        }
    }

    return out;
}

} // namespace nsk
