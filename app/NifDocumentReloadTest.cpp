#include "NifDocument.h"
#include "TestSupport.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

template <class T>
void Append(Bytes& bytes, const T& value)
{
    const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void String(Bytes& bytes, const std::string& text)
{
    Append(bytes, static_cast<std::uint32_t>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

struct Block
{
    std::string type;
    Bytes bytes;
};

Bytes MakeNif(const std::vector<Block>& blocks, std::uint32_t bsVersion = 100)
{
    Bytes bytes;
    const std::string magic = "Gamebryo File Format, Version 20.2.0.7\n";
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    Append(bytes, std::uint32_t { 0x14020007 });
    Append(bytes, std::uint8_t { 1 }); // little endian
    Append(bytes, std::uint32_t { 12 });
    Append(bytes, static_cast<std::uint32_t>(blocks.size()));
    Append(bytes, bsVersion);
    Append(bytes, std::uint8_t { 0 }); // author / process / export
    Append(bytes, std::uint8_t { 0 });
    Append(bytes, std::uint8_t { 0 });
    Append(bytes, static_cast<std::uint16_t>(blocks.size()));
    for (const Block& block : blocks)
        String(bytes, block.type);
    for (std::size_t i = 0; i < blocks.size(); ++i)
        Append(bytes, static_cast<std::uint16_t>(i));
    for (const Block& block : blocks)
        Append(bytes, static_cast<std::uint32_t>(block.bytes.size()));
    Append(bytes, std::uint32_t { 1 }); // string table
    Append(bytes, std::uint32_t { 4 });
    String(bytes, "Node");
    Append(bytes, std::uint32_t { 0 }); // groups
    for (const Block& block : blocks)
        bytes.insert(bytes.end(), block.bytes.begin(), block.bytes.end());
    Append(bytes, std::uint32_t { 0 }); // footer; also keeps a zero-block header non-empty
    return bytes;
}

Bytes ObjectNet()
{
    Bytes bytes;
    Append(bytes, std::int32_t { 0 }); // name
    Append(bytes, std::uint32_t { 0 }); // extra data
    Append(bytes, std::int32_t { -1 }); // controller
    return bytes;
}

Bytes AvObject()
{
    Bytes bytes = ObjectNet();
    Append(bytes, std::uint32_t { 0 }); // flags
    for (int i = 0; i < 3; ++i)
        Append(bytes, 0.0f);
    for (int i = 0; i < 9; ++i)
        Append(bytes, i % 4 == 0 ? 1.0f : 0.0f);
    Append(bytes, 1.0f); // scale
    Append(bytes, std::int32_t { -1 }); // collision
    return bytes;
}

Bytes Node(bool hasChild)
{
    Bytes bytes = AvObject();
    Append(bytes, std::uint32_t { hasChild ? 1u : 0u });
    if (hasChild)
        Append(bytes, std::int32_t { 1 });
    Append(bytes, std::uint32_t { 0 }); // effects
    return bytes;
}

Bytes Shape(std::int32_t shader = -1, std::int32_t alpha = -1)
{
    Bytes bytes = AvObject();
    Append(bytes, std::int32_t { -1 }); // geometry
    Append(bytes, std::int32_t { -1 }); // skin instance
    Append(bytes, std::uint32_t { 0 }); // materials
    Append(bytes, std::int32_t { -1 }); // active material
    Append(bytes, std::uint8_t { 0 });
    Append(bytes, shader);
    Append(bytes, alpha);
    return bytes;
}

Bytes RichNif()
{
    return MakeNif({
        { "NiNode", Node(true) }, { "NiNode", Node(false) },
        { "NiSkinInstance", Bytes(64, 0) }, { "NiSkinData", Bytes(128, 0) },
        { "NiSkinPartition", Bytes(64, 0) }, { "NiTransformData", Bytes(64, 0) },
        { "NiTransformInterpolator", Bytes(64, 0) }, { "NiTransformController", Bytes(64, 0) },
        { "NiControllerSequence", Bytes(128, 0) }, { "NiFloatInterpolator", Bytes(16, 0) },
        { "NiBoolInterpolator", Bytes(16, 0) }, { "NiFloatData", Bytes(16, 0) },
        { "NiBoolData", Bytes(16, 0) }
    });
}

void CheckNoSkinOrAnimation(const nsk::NifDocument& doc)
{
    CHECK(doc.skinInstanceBones().empty());
    CHECK(doc.skinInstanceToDataRef().empty());
    CHECK(doc.skinData().empty());
    CHECK(doc.skinPartitionWeights().empty());
    CHECK(doc.transformData().empty());
    CHECK(doc.transformInterpolators().empty());
    CHECK(doc.timeControllers().empty());
    CHECK(doc.controllerSequences().empty());
    CHECK(doc.floatInterpolators().empty());
    CHECK(doc.boolInterpolators().empty());
    CHECK(doc.floatData().empty());
    CHECK(doc.boolData().empty());
}

void CheckCleared(const nsk::NifDocument& doc, const std::string& error)
{
    CHECK(!doc.isValid());
    CHECK(doc.filePath().empty());
    CHECK(doc.versionString().empty());
    CHECK(doc.version() == 0);
    CHECK(doc.bsVersion() == 0);
    CHECK(doc.blockCount() == 0);
    CHECK(doc.blockTree() == nullptr);
    CHECK(doc.nodes().empty());
    CHECK(doc.roots().empty());
    CHECK(doc.geometries().empty());
    CHECK(doc.materials().empty());
    CHECK(doc.textureSets().empty());
    CHECK(doc.collisionMaterials().empty());
    CHECK(doc.nodeIndexForBlock(0) == nsk::kNoRef);
    CHECK(!doc.lastError().empty());
    CHECK(error == doc.lastError());
    CheckNoSkinOrAnimation(doc);
}

void TestMemoryReload()
{
    nsk::NifDocument doc;
    std::string error;
    REQUIRE(doc.loadFromMemory(RichNif(), &error, L"first.nif"));
    REQUIRE(doc.skinInstanceBones().size() == 1);
    REQUIRE(doc.skinInstanceToDataRef().size() == 1);
    REQUIRE(doc.skinData().size() == 1);
    REQUIRE(doc.skinPartitionWeights().size() == 1);
    REQUIRE(doc.transformData().size() == 1);
    REQUIRE(doc.transformInterpolators().size() == 1);
    REQUIRE(doc.timeControllers().size() == 1);
    REQUIRE(doc.controllerSequences().size() == 1);
    REQUIRE(doc.floatInterpolators().size() == 1);
    REQUIRE(doc.boolInterpolators().size() == 1);
    REQUIRE(doc.floatData().size() == 1);
    REQUIRE(doc.boolData().size() == 1);
    CHECK(doc.nodes()[1].parentIndex == 0);

    // Reuse the old parent/child block IDs, but neither new shape owns children.
    REQUIRE(doc.loadFromMemory(MakeNif({ { "NiTriShape", Shape() }, { "NiTriShape", Shape() } }), &error));
    CHECK(doc.isValid());
    CHECK(doc.filePath().empty());
    REQUIRE(doc.nodes().size() == 2);
    CHECK(doc.roots().size() == 2);
    CHECK(doc.nodes()[1].parentIndex == nsk::kNoRef);
    CheckNoSkinOrAnimation(doc);
}

void TestPendingMaterialAndAlpha()
{
    nsk::NifDocument doc;
    // A's lighting property refers to texture block 1. B's legacy material at
    // the same index must not inherit this reference to B's unrelated textures.
    Bytes lighting(512, 0);
    const std::int32_t textureRef = 1;
    std::memcpy(lighting.data() + 40, &textureRef, sizeof(textureRef));
    REQUIRE(doc.loadFromMemory(MakeNif({ { "BSLightingShaderProperty", lighting } })));
    Bytes textureSet;
    Append(textureSet, std::uint32_t { 1 });
    String(textureSet, "textures/unrelated.dds");
    REQUIRE(doc.loadFromMemory(MakeNif({ { "NiMaterialProperty", Bytes(128, 0) }, { "BSShaderTextureSet", textureSet } })));
    CHECK(doc.materials().at(0).diffuseTexture.empty());

    Bytes alpha = ObjectNet();
    Append(alpha, std::uint16_t { 1 }); // blend enabled
    Append(alpha, std::uint8_t { 128 });
    REQUIRE(doc.loadFromMemory(MakeNif({ { "NiAlphaProperty", alpha } })));
    REQUIRE(doc.loadFromMemory(MakeNif({
        { "Unknown", Bytes(1, 0) }, { "NiTriShape", Shape(2, 0) }, { "NiMaterialProperty", Bytes(128, 0) }
    })));
    CHECK(!doc.materials().at(2).hasAlphaBlend);
}

void TestMemoryFailureAndRecovery()
{
    nsk::NifDocument doc;
    std::string error;
    REQUIRE(doc.loadFromMemory(RichNif(), &error, L"previous.nif"));
    CHECK(!doc.loadFromMemory(MakeNif({}, 77), &error, L"invalid.nif"));
    CheckCleared(doc, error); // header already wrote version fields before failing
    REQUIRE(doc.loadFromMemory(MakeNif({}), &error, L"recovered.nif"));
    CHECK(doc.filePath() == L"recovered.nif");
    CHECK(doc.lastError().empty());
    CHECK(error.empty());
    REQUIRE(doc.loadFromMemory(MakeNif({}), &error, doc.filePath()));
    CHECK(doc.filePath() == L"recovered.nif"); // preserve an aliased sourcePath
    CHECK(!doc.loadFromMemory({}, &error));
    CheckCleared(doc, error);
}

struct FixtureDirectory
{
    std::filesystem::path path;
    FixtureDirectory()
    {
        const auto base = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int i = 0; i < 100; ++i)
        {
            const auto candidate = base / (L"nifdiff-reload-" + std::to_wstring(random()) + L"-" + std::to_wstring(random()));
            if (std::filesystem::create_directory(candidate))
            {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("Could not create a unique test directory");
    }
    ~FixtureDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // only the exclusively created fixture
    }
    void Write(const std::filesystem::path& name, const Bytes& bytes) const
    {
        std::ofstream file(path / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            throw std::runtime_error("Could not write fixture");
    }
};

void TestFileReloadAndFailures()
{
    FixtureDirectory fixture;
    fixture.Write(L"rich.nif", RichNif());
    fixture.Write(L"reload-\uD55C.nif", MakeNif({ { "NiNode", Node(false) } }));
    fixture.Write(L"invalid.nif", Bytes { 1, 2, 3 });
    nsk::NifDocument doc;
    std::string error;
    const auto goodPath = (fixture.path / L"reload-\uD55C.nif").wstring();
    REQUIRE(doc.loadFromFile((fixture.path / L"rich.nif").wstring(), &error));
    REQUIRE(doc.loadFromFile(goodPath, &error)); // internal owned-buffer alias
    REQUIRE(doc.nodes().size() == 1);
    CHECK(doc.nodes()[0].name == "Node");
    CHECK(doc.filePath() == goodPath);
    CheckNoSkinOrAnimation(doc);
    REQUIRE(doc.loadFromFile(doc.filePath(), &error)); // source path can alias this document

    CHECK(!doc.loadFromFile((fixture.path / L"missing.nif").wstring(), &error));
    CheckCleared(doc, error);
    REQUIRE(doc.loadFromFile(goodPath, &error));
    CHECK(doc.lastError().empty());
    CHECK(error.empty());
    CHECK(!doc.loadFromFile((fixture.path / L"invalid.nif").wstring(), &error));
    CheckCleared(doc, error);

    REQUIRE(doc.loadFromFile(goodPath, &error));
    const HANDLE locked = CreateFileW(goodPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    const bool loaded = doc.loadFromFile(goodPath, &error);
    CloseHandle(locked);
    CHECK(!loaded);
    CheckCleared(doc, error);
    REQUIRE(doc.loadFromFile(goodPath, &error));
}
} // namespace

int main()
{
    return nsk::test::Run([]
    {
        TestMemoryReload();
        TestPendingMaterialAndAlpha();
        TestMemoryFailureAndRecovery();
        TestFileReloadAndFailures();
    });
}
