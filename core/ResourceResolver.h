// ResourceResolver.h - Bethesda-style loose + BSA/BA2 path resolution for
// textures/materials (and any future relative Data-rooted assets).
//
// Search order (first hit wins), matching NifSkope's TexCache::find spirit
// and the liteviewer "override folders beat Game Data" plan:
//   1. Override folders (list order; [0] = highest priority)
//   2. NIF parent directory (per-call / per-viewport)
//   3. Game Data loose files
//   4. Game Data *.bsa / *.ba2 archives via Floar (filename-sorted)
#pragma once

#include <ArchiveReader.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nsk
{

struct ResourceBytes
{
    std::wstring diskPath;              // set on loose-file hit
    std::vector<std::uint8_t> data;     // set on archive hit
    bool ok() const { return !diskPath.empty() || !data.empty(); }
};

class ResourceResolver
{
public:
    void SetGameData(std::wstring dataDir);
    void SetOverrideFolders(std::vector<std::wstring> folders); // [0]=highest
    void SetAutoLoadArchives(bool enabled) { m_autoLoadArchives = enabled; }
    bool AutoLoadArchives() const { return m_autoLoadArchives; }

    const std::wstring& GameData() const { return m_gameData; }
    const std::vector<std::wstring>& OverrideFolders() const { return m_overrideFolders; }
    std::size_t ArchiveCount() const { return m_archives.size(); }

    // Scan GameData for *.bsa/*.ba2 that contain a textures/ or materials/
    // root entry. No-op when AutoLoadArchives is false or GameData is empty.
    void ReloadArchives();

    // relativePath uses either / or \ (normalized internally). nifDirectory
    // is the folder containing the NIF currently being rendered (may be empty).
    [[nodiscard]] ResourceBytes Find(const std::string& relativePath,
                                     const std::wstring& nifDirectory = {}) const;

    // Windows registry scan for Bethesda Softworks install paths → Data dirs.
    static std::vector<std::wstring> DetectGameDataFolders();

private:
    static std::string NormalizeRelative(std::string path);
    static std::string ApplyTexturesFixup(const std::string& path);
    static bool LooseExists(const std::wstring& root, const std::string& rel);
    static std::wstring JoinLoose(const std::wstring& root, const std::string& rel);
    static std::wstring ToWidePath(const std::string& rel);
    static bool ArchiveHasTexturesOrMaterials(Floar::IArchiveReader& archive);

    ResourceBytes FindNormalized(const std::string& rel,
                                 const std::wstring& nifDirectory) const;

    std::wstring m_gameData;
    std::vector<std::wstring> m_overrideFolders;
    bool m_autoLoadArchives = true;
    std::vector<std::unique_ptr<Floar::IArchiveReader>> m_archives;
};

} // namespace nsk
