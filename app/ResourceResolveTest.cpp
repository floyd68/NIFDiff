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

int main(int argc, char** argv)
{
    // Live query mode: ResourceResolveTest <relativePath>... resolves each
    // argument against the detected Game Data (loose + archives) and reports
    // where it was found - a quick way to diagnose "texture shows white in
    // the viewer" cases without attaching a debugger to the app.
    if (argc > 1)
    {
        const auto detected = nsk::ResourceResolver::DetectGameDataFolders();
        if (detected.empty())
        {
            std::cout << "no Game Data folder detected\n";
            return 1;
        }
        nsk::ResourceResolver live;
        live.SetAutoLoadArchives(true);
        live.SetGameData(detected.front());
        std::wcout << L"GameData=" << live.GameData() << L" archives=" << live.ArchiveCount() << L"\n";
        int misses = 0;
        for (int i = 1; i < argc; ++i)
        {
            auto hit = live.Find(argv[i]);
            if (!hit.ok())
            {
                std::cout << "MISS: " << argv[i] << "\n";
                ++misses;
            }
            else if (!hit.diskPath.empty())
                std::wcout << L"LOOSE: " << hit.diskPath << L"\n";
            else
                std::cout << "ARCHIVE: " << argv[i] << " (" << hit.data.size() << " bytes)\n";
        }
        return misses == 0 ? 0 : 1;
    }

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

    // A loose mod folder: the NIF lives under <Mod>\meshes\..., its texture at
    // the Data-rooted <Mod>\textures\... (NOT next to the NIF). This must resolve
    // via DeriveDataRoot even though neither GameData nor an override has it.
    const char markerRoot[] = "ROOT";
    fs::create_directories(root / "Mod" / "meshes" / "clutter");
    WriteDummyFile(root / "Mod" / "textures" / "elsopa" / "barrel.dds", markerRoot, 4);

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
        // NIF at <Mod>\meshes\clutter references "textures\elsopa\barrel.dds"
        // relative to <Mod>. It must resolve via the derived Data root, not the
        // NIF's own folder (which would be <Mod>\meshes\clutter\textures\...).
        const auto nifDir = (root / "Mod" / "meshes" / "clutter").wstring();
        auto hit = resolver.Find("textures/elsopa/barrel.dds", nifDir);
        const std::string m = readMarker(hit);
        if (m != "ROOT")
        {
            std::cout << "FAIL: data-root derivation should resolve mod texture, got '" << m << "'\n";
            ++failures;
        }
        else
            std::cout << "OK: data root derived from \\meshes resolves mod texture\n";
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

    {
        nsk::ResourceLocation loose;
        loose.sourceKey = "file:test";
        loose.diskPath = L"D:\\Data\\textures\\test.dds";
        if (loose.displayPath() != loose.diskPath)
        {
            std::cout
                << "FAIL: loose display path mismatch\n";
            ++failures;
        }

        nsk::ResourceLocation archived;
        archived.sourceKey = "bsa:test";
        archived.archiveIndex = 0;
        archived.archivePath = L"D:\\Data\\Textures.ba2";
        archived.archiveEntry =
            L"textures\\test.dds";
        if (archived.displayPath() !=
            L"D:\\Data\\Textures.ba2\\textures\\test.dds")
        {
            std::cout
                << "FAIL: archive display path mismatch\n";
            ++failures;
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
