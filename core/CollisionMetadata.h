#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace nsk
{

// One bhkCompressedMeshShapeData::Chunk Materials entry. The material is the
// SkyrimHavokMaterial value; the remaining fields are the packed HavokFilter.
struct NifCollisionMaterial
{
    std::uint32_t material = 0;
    std::uint8_t layer = 0;
    std::uint8_t flags = 0;
    std::uint16_t group = 0;
};

// Reads only the metadata prefix and Chunk Materials table. Collision
// geometry/chunks intentionally remain out of scope.
inline bool ParseBhkCompressedMeshMaterials(
    std::span<const std::uint8_t> bytes,
    std::vector<NifCollisionMaterial>& out)
{
    out.clear();
    std::size_t pos = 0;
    const auto skip =
        [&](std::size_t count)
        {
            if (count > bytes.size() - pos)
            {
                return false;
            }
            pos += count;
            return true;
        };
    const auto readU32 =
        [&](std::uint32_t& value)
        {
            if (sizeof(value) > bytes.size() - pos)
            {
                return false;
            }
            std::memcpy(
                &value,
                bytes.data() + pos,
                sizeof(value));
            pos += sizeof(value);
            return true;
        };
    const auto skipCounted =
        [&](std::size_t elementSize)
        {
            std::uint32_t count = 0;
            if (!readU32(count) ||
                (elementSize != 0 &&
                 count > (bytes.size() - pos) / elementSize))
            {
                return false;
            }
            return skip(
                static_cast<std::size_t>(count) *
                elementSize);
        };

    // bhkRefObject contributes no bytes. Prefix:
    // four index/mask uints, Error, hkAabb(min/max Vector4),
    // Welding Type and Material Type.
    constexpr std::size_t kPrefixBytes =
        sizeof(std::uint32_t) * 4 +
        sizeof(float) +
        sizeof(float) * 8 +
        sizeof(std::uint8_t) * 2;
    if (!skip(kPrefixBytes) ||
        !skipCounted(sizeof(std::uint32_t)) ||
        !skipCounted(sizeof(std::uint32_t)) ||
        !skipCounted(sizeof(std::uint32_t)))
    {
        return false;
    }

    std::uint32_t materialCount = 0;
    constexpr std::size_t kMaterialBytes = 8;
    if (!readU32(materialCount) ||
        materialCount >
            (bytes.size() - pos) / kMaterialBytes)
    {
        return false;
    }

    out.reserve(materialCount);
    for (std::uint32_t i = 0;
         i < materialCount;
         ++i)
    {
        NifCollisionMaterial material;
        std::memcpy(
            &material.material,
            bytes.data() + pos,
            sizeof(material.material));
        pos += sizeof(material.material);
        material.layer = bytes[pos++];
        material.flags = bytes[pos++];
        std::memcpy(
            &material.group,
            bytes.data() + pos,
            sizeof(material.group));
        pos += sizeof(material.group);
        out.push_back(material);
    }
    return true;
}

} // namespace nsk
