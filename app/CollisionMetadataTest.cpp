#include "../core/CollisionMetadata.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace
{

template <typename T>
void Append(
    std::vector<std::uint8_t>& bytes,
    T value)
{
    const std::uint8_t* first =
        reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(
        bytes.end(),
        first,
        first + sizeof(value));
}

} // namespace

int main()
{
    std::vector<std::uint8_t> bytes(54, 0);

    Append<std::uint32_t>(bytes, 1);
    Append<std::uint32_t>(bytes, 0x11111111);
    Append<std::uint32_t>(bytes, 2);
    Append<std::uint32_t>(bytes, 0x2222);
    Append<std::uint32_t>(bytes, 0x3333);
    Append<std::uint32_t>(bytes, 3);
    Append<std::uint32_t>(bytes, 1);
    Append<std::uint32_t>(bytes, 2);
    Append<std::uint32_t>(bytes, 3);

    Append<std::uint32_t>(bytes, 2);
    Append<std::uint32_t>(bytes, 0x10203040);
    Append<std::uint8_t>(bytes, 7);
    Append<std::uint8_t>(bytes, 8);
    Append<std::uint16_t>(bytes, 0x090A);
    Append<std::uint32_t>(bytes, 0xA0B0C0D0);
    Append<std::uint8_t>(bytes, 11);
    Append<std::uint8_t>(bytes, 12);
    Append<std::uint16_t>(bytes, 0x0D0E);

    std::vector<nsk::NifCollisionMaterial> materials;
    if (!nsk::ParseBhkCompressedMeshMaterials(
            bytes,
            materials) ||
        materials.size() != 2 ||
        materials[0].material != 0x10203040 ||
        materials[0].layer != 7 ||
        materials[0].flags != 8 ||
        materials[0].group != 0x090A ||
        materials[1].material != 0xA0B0C0D0 ||
        materials[1].layer != 11 ||
        materials[1].flags != 12 ||
        materials[1].group != 0x0D0E)
    {
        std::cout
            << "FAIL: valid collision material table\n";
        return 1;
    }

    std::vector<std::uint8_t> truncated(54, 0);
    Append<std::uint32_t>(truncated, 0xFFFFFFFF);
    if (nsk::ParseBhkCompressedMeshMaterials(
            truncated,
            materials))
    {
        std::cout
            << "FAIL: truncated collision table accepted\n";
        return 1;
    }

    std::cout
        << "All collision metadata tests passed.\n";
    return 0;
}
