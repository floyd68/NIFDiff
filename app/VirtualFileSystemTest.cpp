#include <VirtualFileSystem.h>
#include <VirtualPath.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

int main()
{
    namespace fs = std::filesystem;

    constexpr std::array<unsigned char, 131> zipBytes {
        0x50,0x4B,0x03,0x04,0x14,0x00,0x00,0x00,0x00,0x00,0x15,0xBE,0xF2,0x5C,0x31,0x69,
        0x41,0x3B,0x03,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,0x6D,0x65,
        0x73,0x68,0x65,0x73,0x2F,0x74,0x65,0x73,0x74,0x2E,0x6E,0x69,0x66,0x4E,0x49,0x46,
        0x50,0x4B,0x01,0x02,0x14,0x00,0x14,0x00,0x00,0x00,0x00,0x00,0x15,0xBE,0xF2,0x5C,
        0x31,0x69,0x41,0x3B,0x03,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x00,0x00,0x00,0x00,0x6D,0x65,
        0x73,0x68,0x65,0x73,0x2F,0x74,0x65,0x73,0x74,0x2E,0x6E,0x69,0x66,0x50,0x4B,0x05,
        0x06,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x3D,0x00,0x00,0x00,0x30,0x00,0x00,
        0x00,0x00,0x00
    };

    const fs::path root =
        fs::temp_directory_path() /
        "nifdiff_vfs_exists_test";
    const fs::path archive = root / "session.zip";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec)
    {
        return 1;
    }

    {
        std::ofstream output(
            archive,
            std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(zipBytes.data()),
            static_cast<std::streamsize>(zipBytes.size()));
        if (!output)
        {
            return 2;
        }
    }

    const auto member = Floar::VirtualPath::Parse(
        archive.wstring() +
        L"\\meshes\\test.nif");
    const auto directory = Floar::VirtualPath::Parse(
        archive.wstring() +
        L"\\meshes");
    const auto missing = Floar::VirtualPath::Parse(
        archive.wstring() +
        L"\\meshes\\missing.nif");
    if (!member ||
        !directory ||
        !missing)
    {
        return 3;
    }
    if (!Floar::VirtualFileSystem::Exists(*member))
    {
        return 4;
    }
    if (!Floar::VirtualFileSystem::Exists(*directory))
    {
        return 5;
    }
    if (Floar::VirtualFileSystem::Exists(*missing))
    {
        return 6;
    }

    const auto archiveRoot = Floar::VirtualPath::Parse(archive.wstring());
    if (!archiveRoot)
    {
        return 7;
    }

    const std::vector<Floar::VirtualFileEntry> rootEntries =
        Floar::VirtualFileSystem::ListDirectory(*archiveRoot);
    if (rootEntries.size() != 1 ||
        !rootEntries.front().isDirectory ||
        rootEntries.front().path.GetFilename() != L"meshes")
    {
        return 8;
    }

    const std::vector<Floar::VirtualFileEntry> meshEntries =
        Floar::VirtualFileSystem::ListDirectory(*directory);
    if (meshEntries.size() != 1 ||
        meshEntries.front().isDirectory ||
        meshEntries.front().path.GetFilename() != L"test.nif")
    {
        return 9;
    }

    fs::remove_all(root, ec);
    return 0;
}
