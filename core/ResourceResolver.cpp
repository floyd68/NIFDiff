#include "ResourceResolver.h"

#include "NifLog.h"
#include "StartupTrace.h"

#include <ArchiveTypes.h>

#include <algorithm>
#include <cctype>
#include <execution>
#include <filesystem>
#include <fstream>
#include <numeric>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{
namespace
{
    // Log-safe UTF-8 narrowing (path::string() can throw on non-ACP chars).
    std::string ToUtf8ForLog(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            s.data(), len, nullptr, nullptr);
        return s;
    }

    std::wstring LowerWide(std::wstring s)
    {
        for (wchar_t& c : s)
            c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    // ResourceBytes::sourceKey for a loose-file hit: lowercased,
    // slash-normalized absolute path, so the same file reached through
    // different casing / separators dedups to one identity.
    std::string MakeFileSourceKey(std::wstring diskPath)
    {
        for (wchar_t& c : diskPath)
            c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));
        return "file:" + ToUtf8ForLog(diskPath);
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring ReadRegInstallPath(const wchar_t* subKey, const wchar_t* valueName)
    {
        HKEY key = nullptr;
        const REGSAM views[] = { KEY_READ | KEY_WOW64_32KEY, KEY_READ | KEY_WOW64_64KEY };
        for (REGSAM sam : views)
        {
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, sam, &key) != ERROR_SUCCESS)
                continue;
            wchar_t buf[MAX_PATH] {};
            DWORD type = 0;
            DWORD size = sizeof(buf);
            const LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0')
                return buf;
        }
        return {};
    }

    void AppendUnique(std::vector<std::wstring>& out, const std::wstring& path)
    {
        if (path.empty())
            return;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            p = path;
        const std::wstring normalized = p.wstring();
        for (const std::wstring& existing : out)
        {
            if (_wcsicmp(existing.c_str(), normalized.c_str()) == 0)
                return;
        }
        const DWORD attr = GetFileAttributesW(normalized.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
            out.push_back(normalized);
    }
}

ResourceResolver::~ResourceResolver()
{
    WaitForArchiveScan();
}

void ResourceResolver::SetGameData(std::wstring dataDir)
{
    m_gameData = std::move(dataDir);
    while (!m_gameData.empty() && (m_gameData.back() == L'\\' || m_gameData.back() == L'/'))
        m_gameData.pop_back();
    ReloadArchives();
}

void ResourceResolver::SetOverrideFolders(std::vector<std::wstring> folders)
{
    m_overrideFolders = std::move(folders);
    for (std::wstring& f : m_overrideFolders)
    {
        while (!f.empty() && (f.back() == L'\\' || f.back() == L'/'))
            f.pop_back();
    }
}

bool ResourceResolver::ArchiveHasTexturesOrMaterials(const std::vector<Floar::ArchiveEntry>& entries)
{
    for (const Floar::ArchiveEntry& entry : entries)
    {
        if (entry.isDirectory)
            continue;
        std::wstring name = entry.name;
        for (wchar_t& c : name)
        {
            if (c == L'\\')
                c = L'/';
            else if (c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (name.starts_with(L"textures/") || name.starts_with(L"materials/"))
            return true;
    }
    return false;
}

void ResourceResolver::ReloadArchives()
{
    // Join any in-flight scan first so two workers never fill m_archives
    // concurrently (SetGameData can re-trigger from UI callbacks).
    WaitForArchiveScan();
    m_archives.clear();

    std::shared_future<void> scan;
    if (m_autoLoadArchives && !m_gameData.empty())
    {
        scan = std::async(std::launch::async,
            [this, gameData = m_gameData] { ScanArchives(std::move(gameData)); }).share();
    }

    std::lock_guard<std::mutex> lock(m_scanMutex);
    m_pendingScan = std::move(scan);
}

void ResourceResolver::WaitForArchiveScan() const
{
    std::shared_future<void> pending;
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        pending = m_pendingScan;
    }
    if (!pending.valid())
        return;

    const auto t0 = StartupTrace::Clock::now();
    pending.wait();
    const double waitedMs = std::chrono::duration<double, std::milli>(StartupTrace::Clock::now() - t0).count();
    if (waitedMs > 0.5)
        NIFLOG_INFO("[STARTUP]   blocked {:.2f} ms waiting for the archive scan", waitedMs);
}

bool ResourceResolver::IsArchiveScanReady() const
{
    std::shared_future<void> pending;
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        pending = m_pendingScan;
    }
    return !pending.valid() ||
           pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void ResourceResolver::ScanArchives(std::wstring gameData)
{
    StartupTrace::Phase total("Archive scan (worker thread)");

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(gameData, ec))
        return;

    std::vector<fs::path> candidates;
    {
        StartupTrace::Phase p("  Archive scan: directory scan");
        for (const fs::directory_entry& entry : fs::directory_iterator(gameData, ec))
        {
            if (ec || !entry.is_regular_file(ec))
                continue;
            std::wstring e = entry.path().extension().wstring();
            for (wchar_t& c : e)
                c = static_cast<wchar_t>(towlower(c));
            if (!Floar::ArchiveTypes::IsBethesdaArchiveExt(e))
                continue;
            candidates.push_back(entry.path());
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const fs::path& a, const fs::path& b)
        {
            return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
        });

    // Open + probe every candidate in parallel: the per-archive cost is a
    // flat ~5ms of file-open/TOC-parse (measured; independent of archive
    // size), so a modded Data folder with 150+ archives spent >1.2s here
    // when opened sequentially. Readers are self-contained per instance
    // (own ifstream per operation), so distinct archives can open on
    // distinct threads; the keep-order of m_archives stays the sorted
    // candidate order because results are joined by index afterwards.
    struct ScanResult
    {
        std::unique_ptr<Floar::IArchiveReader> archive;
        std::size_t entryCount = 0;
        bool keep = false;
        double openMs = 0.0;
        double scanMs = 0.0;
    };
    std::vector<ScanResult> scans(candidates.size());
    {
        StartupTrace::Phase p("  Archive scan: parallel open+probe");
        std::vector<std::size_t> indices(candidates.size());
        std::iota(indices.begin(), indices.end(), std::size_t { 0 });
        std::for_each(std::execution::par, indices.begin(), indices.end(),
            [&](std::size_t i)
            {
                using TraceClock = StartupTrace::Clock;
                ScanResult& r = scans[i];
                const auto tOpen = TraceClock::now();
                r.archive = Floar::ArchiveReaderFactory::Open(candidates[i]);
                const auto tList = TraceClock::now();
                r.openMs = std::chrono::duration<double, std::milli>(tList - tOpen).count();
                if (!r.archive)
                    return;
                const std::vector<Floar::ArchiveEntry> entries = r.archive->ListEntries();
                r.entryCount = entries.size();
                r.keep = ArchiveHasTexturesOrMaterials(entries);
                r.scanMs = std::chrono::duration<double, std::milli>(TraceClock::now() - tList).count();
            });
    }

    for (std::size_t i = 0; i < scans.size(); ++i)
    {
        ScanResult& r = scans[i];
        if (!r.archive)
        {
            NIFLOG_INFO("[STARTUP]   archive open FAILED {:>8.2f} ms  {}",
                r.openMs, ToUtf8ForLog(candidates[i].filename().wstring()));
            continue;
        }
        NIFLOG_TRACE("[STARTUP]   archive {} open={:.2f}ms list+scan={:.2f}ms({} entries)  {}",
            r.keep ? "KEEP" : "skip", r.openMs, r.scanMs, r.entryCount,
            ToUtf8ForLog(candidates[i].filename().wstring()));
        if (r.keep)
            m_archives.push_back({ candidates[i].wstring(), std::move(r.archive) });
    }

    NIFLOG_INFO("[STARTUP]   Archive scan: {} candidates -> {} kept", candidates.size(), m_archives.size());
}

std::string ResourceResolver::NormalizeRelative(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    return path;
}

std::string ResourceResolver::ApplyTexturesFixup(const std::string& path)
{
    if (path.starts_with("textures") || path.starts_with("shaders")
        || path.starts_with("materials"))
        return path;

    const auto texPos = path.find("textures/");
    if (texPos != std::string::npos && texPos > 0)
        return path.substr(texPos);

    const auto matPos = path.find("materials/");
    if (matPos != std::string::npos && matPos > 0)
        return path.substr(matPos);

    return "textures/" + path;
}

std::wstring ResourceResolver::DeriveDataRoot(const std::wstring& nifDirectory)
{
    if (nifDirectory.empty())
        return {};

    // Search a lowercased, backslash-normalized copy for a path segment that is
    // exactly "meshes" (bounded by separators / ends), but cut the ORIGINAL
    // string so the returned root keeps its real casing. Scan left-to-right so a
    // path with nested meshes/ folders yields the shallowest (true Data) root.
    std::wstring lower = nifDirectory;
    for (wchar_t& c : lower)
        c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));

    for (std::size_t from = 0;;)
    {
        const std::size_t p = lower.find(L"\\meshes", from);
        if (p == std::wstring::npos)
            break;
        const std::size_t after = p + 7; // length of "\meshes"
        if (after == lower.size() || lower[after] == L'\\')
            return nifDirectory.substr(0, p); // everything before "\meshes"
        from = p + 1;
    }
    return {};
}

std::wstring ResourceResolver::ToWidePath(const std::string& rel)
{
    std::wstring w(rel.size(), L'\0');
    for (std::size_t i = 0; i < rel.size(); ++i)
        w[i] = static_cast<wchar_t>(static_cast<unsigned char>(rel[i]));
    return w;
}

std::wstring ResourceResolver::JoinLoose(const std::wstring& root, const std::string& rel)
{
    std::wstring w = ToWidePath(rel);
    for (wchar_t& c : w)
    {
        if (c == L'/')
            c = L'\\';
    }
    return root + L"\\" + w;
}

bool ResourceResolver::LooseExists(const std::wstring& root, const std::string& rel)
{
    if (root.empty() || rel.empty())
        return false;
    return FileExists(JoinLoose(root, rel));
}

ResourceLocation ResourceResolver::LocateNormalized(const std::string& rel,
                                                    const std::wstring& nifDirectory) const
{
    ResourceLocation result;

    auto tryLoose = [&](const std::wstring& root) -> bool
    {
        if (!LooseExists(root, rel))
            return false;
        result.diskPath = JoinLoose(root, rel);
        result.sourceKey = MakeFileSourceKey(result.diskPath);
        return true;
    };

    for (const std::wstring& folder : m_overrideFolders)
    {
        if (tryLoose(folder))
            return result;
    }

    if (!nifDirectory.empty() && tryLoose(nifDirectory))
        return result;

    // Bethesda Data-rooted fallback: a NIF under <root>\meshes\ references
    // "textures\..." relative to <root>. Try that derived root (a loose mod
    // folder's own textures) before the game's Data - a compare/preview tool
    // wants THIS mod's textures, and vanilla still resolves via m_gameData below.
    if (const std::wstring dataRoot = DeriveDataRoot(nifDirectory);
        !dataRoot.empty() && tryLoose(dataRoot))
        return result;

    if (!m_gameData.empty() && tryLoose(m_gameData))
        return result;

    // Loose-file hits above never wait. The archive fallback needs the
    // background scan to have finished; rather than BLOCK on it (which froze
    // startup - the whole reason the pump-wait existed), a lookup made while
    // the scan is still running just reports "not resolved yet". The startup
    // re-resolves every loaded pane's textures once the scan lands (see
    // NifCompareView::RefreshTexturesAfterScan), so archive textures pop in
    // then instead of stalling the UI now.
    if (!IsArchiveScanReady())
        return {};
    WaitForArchiveScan(); // ready (or none pending): returns immediately

    // Cheap HasEntry probe only (read-only map lookup) - the byte extraction
    // is deferred to Extract, which a worker runs off the UI thread. First
    // archive with the entry wins (the list is priority-sorted).
    const std::wstring wideRel = ToWidePath(rel);
    for (std::size_t i = 0; i < m_archives.size(); ++i)
    {
        const LoadedArchive& archive = m_archives[i];
        if (!archive.reader || !archive.reader->HasEntry(wideRel))
            continue;
        result.archiveIndex = static_cast<int>(i);
        result.archivePath = archive.path;
        result.archiveEntry = wideRel;
        result.sourceKey = "bsa:" + ToUtf8ForLog(LowerWide(archive.path)) + "|" + rel;
        return result;
    }

    return {};
}

ResourceLocation ResourceResolver::Locate(const std::string& relativePath,
                                          const std::wstring& nifDirectory) const
{
    if (relativePath.empty())
        return {};

    {
        std::wstring abs = ToWidePath(relativePath);
        if (FileExists(abs))
        {
            ResourceLocation r;
            r.sourceKey = MakeFileSourceKey(abs);
            r.diskPath = std::move(abs);
            return r;
        }
    }

    const std::string normalized = NormalizeRelative(relativePath);
    ResourceLocation hit = LocateNormalized(normalized, nifDirectory);
    if (hit.ok())
        return hit;

    const std::string fixed = ApplyTexturesFixup(normalized);
    if (fixed != normalized)
        return LocateNormalized(fixed, nifDirectory);

    return {};
}

std::vector<std::uint8_t> ResourceResolver::Extract(const ResourceLocation& loc) const
{
    if (loc.archiveIndex < 0)
        return {}; // loose file: the decoder opens loc.diskPath itself

    WaitForArchiveScan(); // m_archives must be populated (Locate already waited)
    if (loc.archiveIndex >= static_cast<int>(m_archives.size()))
        return {};
    const LoadedArchive& archive = m_archives[static_cast<std::size_t>(loc.archiveIndex)];
    if (!archive.reader)
        return {};
    // ExtractToMemory is logically const (IO into a fresh buffer via a
    // per-call stream), so this is safe to run concurrently across workers.
    return const_cast<Floar::IArchiveReader&>(*archive.reader).ExtractToMemory(loc.archiveEntry);
}

ResourceBytes ResourceResolver::Find(const std::string& relativePath,
                                     const std::wstring& nifDirectory) const
{
    // Full synchronous lookup for the lazy/thumbnail/CM paths: locate + read.
    const ResourceLocation loc = Locate(relativePath, nifDirectory);
    if (!loc.ok())
        return {};
    ResourceBytes r;
    r.sourceKey = loc.sourceKey;
    if (loc.isLoose())
        r.diskPath = loc.diskPath;
    else
        r.data = Extract(loc);
    return r;
}

std::vector<std::wstring> ResourceResolver::DetectGameDataFolders()
{
    StartupTrace::Phase p("DetectGameDataFolders (registry)");
    std::vector<std::wstring> out;

    struct Candidate
    {
        const wchar_t* subKey;
        const wchar_t* valueName;
        const wchar_t* dataSuffix;
    };

    const Candidate games[] = {
        { L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\Fallout4", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\Skyrim", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\FalloutNV", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\Fallout3", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\Oblivion", L"Installed Path", L"Data" },
        { L"SOFTWARE\\Bethesda Softworks\\Morrowind", L"Installed Path", L"Data Files" },
    };

    for (const Candidate& g : games)
    {
        std::wstring install = ReadRegInstallPath(g.subKey, g.valueName);
        if (install.empty())
            continue;
        while (!install.empty() && (install.back() == L'\\' || install.back() == L'/'))
            install.pop_back();
        AppendUnique(out, install + L"\\" + g.dataSuffix);
    }
    return out;
}

} // namespace nsk
