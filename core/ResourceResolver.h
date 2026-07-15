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
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nsk
{

struct ResourceBytes
{
    std::wstring diskPath;              // set on loose-file hit
    std::vector<std::uint8_t> data;     // set on archive hit

    // Identity of the resolved bytes, stable across panes: two panes whose
    // same-named requests land on the SAME loose file / archive entry get
    // the same key, while requests that resolve differently (override vs
    // vanilla - the whole point of a compare tool) get different keys.
    // "file:<lowercased absolute path>" or "bsa:<archive path>|<entry>",
    // UTF-8. Empty when the lookup missed. TextureRepository dedups on it.
    std::string sourceKey;

    bool ok() const { return !diskPath.empty() || !data.empty(); }
};

class ResourceResolver
{
public:
    ~ResourceResolver();

    void SetGameData(std::wstring dataDir);
    void SetOverrideFolders(std::vector<std::wstring> folders); // [0]=highest
    void SetAutoLoadArchives(bool enabled) { m_autoLoadArchives = enabled; }
    bool AutoLoadArchives() const { return m_autoLoadArchives; }

    const std::wstring& GameData() const { return m_gameData; }
    const std::vector<std::wstring>& OverrideFolders() const { return m_overrideFolders; }
    std::size_t ArchiveCount() const { WaitForArchiveScan(); return m_archives.size(); }

    // Scan GameData for *.bsa/*.ba2 that contain a textures/ or materials/
    // root entry. No-op when AutoLoadArchives is false or GameData is empty.
    // The scan itself runs on a background thread (a modded Data folder
    // takes ~200ms even opened in parallel - see StartupTrace numbers), so
    // this returns immediately; anything that consults the archive list
    // (Find's archive fallback, ArchiveCount) blocks in WaitForArchiveScan
    // until it lands. Loose-file lookups never wait.
    void ReloadArchives();

    // Blocks until the scan launched by the last ReloadArchives finished.
    void WaitForArchiveScan() const;

    // Non-blocking: true when no scan is pending or the pending one is done.
    // Lets startup keep the window responsive (pump messages) until the scan
    // lands, so the initial pane loads don't stall on it before the window
    // is even shown.
    bool IsArchiveScanReady() const;

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
    static bool ArchiveHasTexturesOrMaterials(const std::vector<Floar::ArchiveEntry>& entries);

    ResourceBytes FindNormalized(const std::string& rel,
                                 const std::wstring& nifDirectory) const;

    // Worker body of ReloadArchives: enumerates gameData (by-value copy so
    // the member stays untouched off-thread) and fills m_archives. Nobody
    // reads m_archives until WaitForArchiveScan has joined this.
    void ScanArchives(std::wstring gameData);

    std::wstring m_gameData;
    std::vector<std::wstring> m_overrideFolders;
    bool m_autoLoadArchives = true;

    // The source path rides along with each reader so archive hits can
    // report a stable ResourceBytes::sourceKey.
    struct LoadedArchive
    {
        std::wstring path;
        std::unique_ptr<Floar::IArchiveReader> reader;
    };
    std::vector<LoadedArchive> m_archives;

    // Declared after m_archives on purpose: members are destroyed in
    // reverse order, so the future (whose destruction joins a still-running
    // std::async worker) goes first, before the m_archives the worker
    // writes to is torn down. ~ResourceResolver also waits explicitly.
    mutable std::mutex m_scanMutex;                 // guards the m_pendingScan handle
    mutable std::shared_future<void> m_pendingScan; // valid while a scan is queued/running
};

} // namespace nsk
