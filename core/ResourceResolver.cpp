#include "ResourceResolver.h"

#include <ArchiveTypes.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{
namespace
{
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

bool ResourceResolver::ArchiveHasTexturesOrMaterials(Floar::IArchiveReader& archive)
{
    for (const Floar::ArchiveEntry& entry : archive.ListEntries())
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
    m_archives.clear();
    if (!m_autoLoadArchives || m_gameData.empty())
        return;

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(m_gameData, ec))
        return;

    std::vector<fs::path> candidates;
    for (const fs::directory_entry& entry : fs::directory_iterator(m_gameData, ec))
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

    std::sort(candidates.begin(), candidates.end(),
        [](const fs::path& a, const fs::path& b)
        {
            return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
        });

    for (const fs::path& path : candidates)
    {
        auto archive = Floar::ArchiveReaderFactory::Open(path);
        if (!archive)
            continue;
        if (ArchiveHasTexturesOrMaterials(*archive))
            m_archives.push_back(std::move(archive));
    }
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

ResourceBytes ResourceResolver::FindNormalized(const std::string& rel,
                                               const std::wstring& nifDirectory) const
{
    ResourceBytes result;

    auto tryLoose = [&](const std::wstring& root) -> bool
    {
        if (!LooseExists(root, rel))
            return false;
        result.diskPath = JoinLoose(root, rel);
        return true;
    };

    for (const std::wstring& folder : m_overrideFolders)
    {
        if (tryLoose(folder))
            return result;
    }

    if (!nifDirectory.empty() && tryLoose(nifDirectory))
        return result;

    if (!m_gameData.empty() && tryLoose(m_gameData))
        return result;

    const std::wstring wideRel = ToWidePath(rel);
    for (const auto& archive : m_archives)
    {
        if (!archive || !archive->HasEntry(wideRel))
            continue;
        // ExtractToMemory is logically const for our purposes (IO into a buffer).
        result.data = const_cast<Floar::IArchiveReader&>(*archive).ExtractToMemory(wideRel);
        if (!result.data.empty())
            return result;
        result.data.clear();
    }

    return {};
}

ResourceBytes ResourceResolver::Find(const std::string& relativePath,
                                     const std::wstring& nifDirectory) const
{
    if (relativePath.empty())
        return {};

    {
        std::wstring abs = ToWidePath(relativePath);
        if (FileExists(abs))
        {
            ResourceBytes r;
            r.diskPath = std::move(abs);
            return r;
        }
    }

    const std::string normalized = NormalizeRelative(relativePath);
    ResourceBytes hit = FindNormalized(normalized, nifDirectory);
    if (hit.ok())
        return hit;

    const std::string fixed = ApplyTexturesFixup(normalized);
    if (fixed != normalized)
        return FindNormalized(fixed, nifDirectory);

    return {};
}

std::vector<std::wstring> ResourceResolver::DetectGameDataFolders()
{
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
