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

enum class BethesdaGame : std::uint8_t
{
    Unknown = 0,
    SkyrimLE,
    SkyrimSE,
    Fallout4
};

struct GameDataRoot
{
    BethesdaGame game { BethesdaGame::Unknown };
    std::wstring path;
};

BethesdaGame BethesdaGameFromBsVersion(std::uint32_t bsVersion);
const wchar_t* BethesdaGameName(BethesdaGame game);

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

// The identity of a resolved source WITHOUT its bytes - the cheap half of a
// lookup (loose-file existence / archive HasEntry), split out so the expensive
// archive byte-extraction can move off the UI thread. Build one with Locate on
// the UI thread (for a dedup key), then read the bytes with Extract on a worker.
struct ResourceLocation
{
    std::string sourceKey;       // identity, empty on a miss (see ResourceBytes::sourceKey)
    std::wstring diskPath;       // loose-file hit: the decoder reads this directly
    int archiveIndex = -1;       // archive hit: index into the resolver's archive list
    std::wstring archivePath;    // archive hit: host path for VFS/display consumers
    std::wstring archiveEntry;   // archive hit: normalized entry name to extract

    bool ok() const { return !sourceKey.empty(); }
    bool isLoose() const { return archiveIndex < 0; }
    std::wstring displayPath() const
    {
        if (isLoose())
        {
            return diskPath;
        }
        if (archivePath.empty() || archiveEntry.empty())
        {
            return {};
        }
        return archivePath + L"\\" + archiveEntry;
    }
};

class ResourceResolver
{
public:
    ~ResourceResolver();

    // Legacy single-root entry point. An untyped root remains available as a
    // fallback for all games; new UI/settings should use SetGameDataRoots.
    void SetGameData(std::wstring dataDir);
    void SetGameDataRoots(std::vector<GameDataRoot> roots);
    void SetOverrideFolders(std::vector<std::wstring> folders); // [0]=highest
    void SetAutoLoadArchives(bool enabled) { m_autoLoadArchives = enabled; }
    bool AutoLoadArchives() const { return m_autoLoadArchives; }

    const std::wstring& GameData() const { return m_legacyGameData; }
    const std::vector<GameDataRoot>& GameDataRoots() const { return m_gameDataRoots; }
    const std::vector<std::wstring>& OverrideFolders() const { return m_overrideFolders; }
    std::size_t ArchiveCount() const;

    // Prefer the registered Data root containing the NIF source path (including
    // virtual paths inside BSA/BA2 files), then fall back to its BS Version.
    // This handles LE-format meshes shipped or installed in Skyrim SE Data.
    BethesdaGame GameForNifPath(
        const std::wstring& nifPath,
        std::uint32_t bsVersion) const;

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
                                     const std::wstring& nifDirectory = {},
                                     BethesdaGame game = BethesdaGame::Unknown) const;

    // Cheap identity lookup: which loose file / archive entry `relativePath`
    // resolves to, WITHOUT reading archive bytes. Thread-safe; safe to call on
    // the UI thread to obtain a dedup key. (Loose-file hits never wait; an
    // archive hit waits for the background scan, usually long done.)
    [[nodiscard]] ResourceLocation Locate(const std::string& relativePath,
                                          const std::wstring& nifDirectory = {},
                                          BethesdaGame game = BethesdaGame::Unknown) const;

    // Read a located source's bytes. A loose file returns empty (the decoder
    // opens diskPath itself); an archive entry does the ExtractToMemory. Safe
    // on a worker thread: Floar readers open a fresh stream per extract and
    // only read immutable metadata, so concurrent extraction is thread-safe
    // (the IoGate already bounds how many run at once).
    [[nodiscard]] std::vector<std::uint8_t> Extract(const ResourceLocation& loc) const;

    // Windows registry scan for Bethesda Softworks install paths → Data dirs.
    static std::vector<std::wstring> DetectGameDataFolders();
    static std::vector<GameDataRoot> DetectGameDataRoots();

private:
    static std::string NormalizeRelative(std::string path);
    static std::string ApplyTexturesFixup(const std::string& path);
    // Bethesda assets are Data-rooted: a NIF at <root>\meshes\... references
    // "textures\..." relative to <root>, not to the NIF's own folder. Given the
    // NIF's directory, return everything before its "\meshes" segment as the
    // implicit Data root (empty when there is none), so a loose mod folder's own
    // textures resolve without the mod being deployed to the game's Data dir.
    static std::wstring DeriveDataRoot(const std::wstring& nifDirectory);
    static bool LooseExists(const std::wstring& root, const std::string& rel);
    static std::wstring JoinLoose(const std::wstring& root, const std::string& rel);
    static std::wstring ToWidePath(const std::string& rel);
    static bool ArchiveHasTexturesOrMaterials(const std::vector<Floar::ArchiveEntry>& entries);

    ResourceLocation LocateNormalized(const std::string& rel,
                                      const std::wstring& nifDirectory,
                                      BethesdaGame game) const;

    // Worker body of ReloadArchives: enumerates gameData (by-value copy so
    // the member stays untouched off-thread) and fills m_archives. Nobody
    // reads m_archives until WaitForArchiveScan has joined this.
    void ScanArchives(std::vector<GameDataRoot> roots);

    std::wstring m_legacyGameData;
    std::vector<GameDataRoot> m_gameDataRoots;
    std::vector<std::wstring> m_overrideFolders;
    bool m_autoLoadArchives = true;

    // The source path rides along with each reader so archive hits can
    // report a stable ResourceBytes::sourceKey.
    struct LoadedArchive
    {
        BethesdaGame game { BethesdaGame::Unknown };
        std::wstring path;
        std::unique_ptr<Floar::IArchiveReader> reader;
    };
    std::vector<LoadedArchive> m_archives;
    mutable std::mutex m_archiveMutex;

    // Declared after m_archives on purpose: members are destroyed in
    // reverse order, so the future (whose destruction joins a still-running
    // std::async worker) goes first, before the m_archives the worker
    // writes to is torn down. ~ResourceResolver also waits explicitly.
    mutable std::mutex m_scanMutex;                 // guards the m_pendingScan handle
    mutable std::shared_future<void> m_pendingScan; // valid while a scan is queued/running
};

} // namespace nsk
