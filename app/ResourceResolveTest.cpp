// ResourceResolveTest - smoke-test Bethesda search order without a GPU.
#include "ResourceResolver.h"

#include <ArchiveReader.h>
#include <ArchiveTypes.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

static void WriteDummyFile(const fs::path& p, const char* bytes, std::size_t n)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(bytes, static_cast<std::streamsize>(n));
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "niflite_resource_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "GameData" / "textures" / "test");
    fs::create_directories(root / "Override" / "textures" / "test");
    fs::create_directories(root / "NifDir");

    const char markerGame[] = "GAME";
    const char markerOvr[] = "OVR!";
    const char markerNif[] = "NIF!";
    WriteDummyFile(root / "GameData" / "textures" / "test" / "a.dds", markerGame, 4);
    WriteDummyFile(root / "Override" / "textures" / "test" / "a.dds", markerOvr, 4);
    WriteDummyFile(root / "NifDir" / "textures" / "test" / "a.dds", markerNif, 4);
    WriteDummyFile(root / "GameData" / "textures" / "test" / "only_game.dds", markerGame, 4);

    nsk::ResourceResolver resolver;
    resolver.SetAutoLoadArchives(false);
    resolver.SetGameData((root / "GameData").wstring());
    resolver.SetOverrideFolders({ (root / "Override").wstring() });

    auto readMarker = [](const nsk::ResourceBytes& b) -> std::string
    {
        if (b.diskPath.empty()) return {};
        std::ifstream in(b.diskPath, std::ios::binary);
        char buf[5] {};
        in.read(buf, 4);
        return buf;
    };

    int failures = 0;
    {
        auto hit = resolver.Find("textures/test/a.dds", (root / "NifDir").wstring());
        const std::string m = readMarker(hit);
        if (m != "OVR!")
        {
            std::cout << "FAIL: override should win, got '" << m << "'\n";
            ++failures;
        }
        else
            std::cout << "OK: override beats nif dir and game data\n";
    }

    {
        resolver.SetOverrideFolders({});
        auto hit = resolver.Find("textures/test/a.dds", (root / "NifDir").wstring());
        const std::string m = readMarker(hit);
        if (m != "NIF!")
        {
            std::cout << "FAIL: nif dir should win over game data, got '" << m << "'\n";
            ++failures;
        }
        else
            std::cout << "OK: nif dir beats game data\n";
    }

    {
        auto hit = resolver.Find("textures/test/only_game.dds", (root / "NifDir").wstring());
        const std::string m = readMarker(hit);
        if (m != "GAME")
        {
            std::cout << "FAIL: game data loose miss, got '" << m << "'\n";
            ++failures;
        }
        else
            std::cout << "OK: game data loose resolves\n";
    }

    {
        auto hit = resolver.Find("test/only_game.dds");
        const std::string m = readMarker(hit);
        if (m != "GAME")
        {
            std::cout << "FAIL: textures/ fixup, got '" << m << "'\n";
            ++failures;
        }
        else
            std::cout << "OK: textures/ prefix fixup\n";
    }

    {
        const auto detected = nsk::ResourceResolver::DetectGameDataFolders();
        std::cout << "DetectGameDataFolders: " << detected.size() << " candidate(s)\n";
        for (const auto& d : detected)
            std::wcout << L"  " << d << L"\n";
    }

    {
        const auto detected = nsk::ResourceResolver::DetectGameDataFolders();
        if (!detected.empty())
        {
            nsk::ResourceResolver live;
            live.SetAutoLoadArchives(true);
            live.SetGameData(detected.front());
            std::wcout << L"Live GameData=" << live.GameData()
                       << L" archives=" << live.ArchiveCount() << L"\n";

            bool archiveOk = false;
            for (const auto& entry : fs::directory_iterator(detected.front()))
            {
                auto ext = entry.path().extension().wstring();
                for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (!Floar::ArchiveTypes::IsBethesdaArchiveExt(ext))
                    continue;
                auto probe = Floar::ArchiveReaderFactory::Open(entry.path());
                if (!probe)
                    continue;

                // Prefer known keys; otherwise pick the first textures/*.dds entry.
                std::wstring key;
                const wchar_t* preferred[] = {
                    L"textures/landscape/dirt01.dds",
                    L"textures/effects/fxglowflat.dds"
                };
                for (const wchar_t* k : preferred)
                {
                    if (probe->HasEntry(k))
                    {
                        key = k;
                        break;
                    }
                }
                if (key.empty())
                {
                    for (const auto& e : probe->ListEntries())
                    {
                        if (e.isDirectory) continue;
                        std::wstring n = e.name;
                        for (wchar_t& c : n)
                        {
                            if (c == L'\\') c = L'/';
                            else if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                        }
                        if (n.starts_with(L"textures/") && n.ends_with(L".dds"))
                        {
                            key = e.name;
                            break;
                        }
                    }
                }
                if (key.empty())
                    continue;

                auto data = probe->ExtractToMemory(key);
                if (data.size() >= 4 && data[0] == 'D' && data[1] == 'D' && data[2] == 'S')
                {
                    std::wcout << L"OK: Floar BSA/BA2 extract from " << entry.path().filename().wstring()
                               << L" (" << data.size() << L" bytes)\n";
                    archiveOk = true;
                    break;
                }
            }
            auto hit = live.Find("textures/landscape/dirt01.dds");
            if (hit.ok())
                std::cout << "OK: live Find hit (" << (hit.diskPath.empty() ? "archive" : "loose") << ")\n";
            if (!archiveOk)
                std::cout << "NOTE: could not verify BSA extract on this install\n";
        }
    }

    fs::remove_all(root, ec);
    if (failures == 0)
    {
        std::cout << "All ResourceResolver tests passed.\n";
        return 0;
    }
    std::cout << failures << " failure(s)\n";
    return 1;
}
