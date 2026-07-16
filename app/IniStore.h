// IniStore.h — shared single-pass INI read + Win32 write helpers
//
// Canonical copy: FICture2/IniStore.h
// Mirrored copy:  NIFDiff/app/IniStore.h  (keep identical via scripts/sync-ini-store.ps1)
//
// Reads the entire file once into memory (avoids GetPrivateProfile* reopen cost).
// Writes go through WritePrivateProfileStringW at save points only.
// No project-specific dependencies — safe to copy between FICture2 and NIFDiff.
//
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class IniStore
{
public:
    using KeyMap = std::unordered_map<std::wstring, std::wstring>;
    using SectionMap = std::unordered_map<std::wstring, KeyMap>;

    static IniStore Load(const std::wstring& path)
    {
        IniStore ini;

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr)
        {
            return ini;
        }

        (void)std::fseek(fp, 0, SEEK_END);
        const long flen = std::ftell(fp);
        (void)std::fseek(fp, 0, SEEK_SET);

        if (flen <= 0)
        {
            std::fclose(fp);
            return ini;
        }

        std::vector<std::uint8_t> raw(static_cast<std::size_t>(flen));
        const std::size_t nRead = std::fread(raw.data(), 1, raw.size(), fp);
        std::fclose(fp);

        if (nRead == 0)
        {
            return ini;
        }

        std::wstring content;
        if (nRead >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        {
            const std::size_t wchars = (nRead - 2) / sizeof(wchar_t);
            content.assign(
                reinterpret_cast<const wchar_t*>(raw.data() + 2),
                wchars);
        }
        else
        {
            const int wlen = MultiByteToWideChar(
                CP_ACP,
                0,
                reinterpret_cast<const char*>(raw.data()),
                static_cast<int>(nRead),
                nullptr,
                0);
            if (wlen > 0)
            {
                content.resize(static_cast<std::size_t>(wlen));
                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    reinterpret_cast<const char*>(raw.data()),
                    static_cast<int>(nRead),
                    content.data(),
                    wlen);
            }
        }

        ini.Parse(content);
        ini.m_loaded = true;
        return ini;
    }

    std::wstring GetString(
        std::wstring_view section,
        std::wstring_view key,
        const std::wstring& defaultVal = L"") const
    {
        const auto sIt = m_sections.find(ToLower(section));
        if (sIt == m_sections.end())
        {
            return defaultVal;
        }
        const auto kIt = sIt->second.find(ToLower(key));
        if (kIt == sIt->second.end())
        {
            return defaultVal;
        }
        return kIt->second;
    }

    int GetInt(std::wstring_view section, std::wstring_view key, int defaultVal = 0) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        wchar_t* end = nullptr;
        const long v = std::wcstol(s.c_str(), &end, 10);
        return (end == s.c_str()) ? defaultVal : static_cast<int>(v);
    }

    float GetFloat(std::wstring_view section, std::wstring_view key, float defaultVal = 0.0f) const
    {
        const std::wstring s = GetString(section, key);
        if (s.empty())
        {
            return defaultVal;
        }
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return (end == s.c_str()) ? defaultVal : static_cast<float>(v);
    }

    bool IsLoaded() const
    {
        return m_loaded;
    }

    static void SetString(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        const std::wstring& value)
    {
        (void)WritePrivateProfileStringW(
            section.c_str(),
            key.c_str(),
            value.c_str(),
            path.c_str());
    }

    static void SetInt(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        int value)
    {
        SetString(path, section, key, std::to_wstring(value));
    }

    static void SetFloat(
        const std::wstring& path,
        const std::wstring& section,
        const std::wstring& key,
        float value)
    {
        SetString(path, section, key, std::to_wstring(value));
    }

    // Pipe-joined multi-value helpers (RecentFiles, OverrideFolders, …).
    static std::vector<std::wstring> SplitPipeList(std::wstring_view s)
    {
        std::vector<std::wstring> out;
        std::size_t start = 0;
        while (start < s.size())
        {
            std::size_t bar = s.find(L'|', start);
            if (bar == std::wstring_view::npos)
            {
                bar = s.size();
            }
            std::wstring_view part = s.substr(start, bar - start);
            while (!part.empty() && std::iswspace(part.front()))
            {
                part.remove_prefix(1);
            }
            while (!part.empty() && std::iswspace(part.back()))
            {
                part.remove_suffix(1);
            }
            if (!part.empty())
            {
                out.emplace_back(part);
            }
            start = bar + 1;
        }
        return out;
    }

    static std::wstring JoinPipeList(const std::vector<std::wstring>& parts)
    {
        std::wstring out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out += L'|';
            }
            out += parts[i];
        }
        return out;
    }

private:
    SectionMap m_sections;
    bool m_loaded = false;

    static std::wstring ToLower(std::wstring_view s)
    {
        std::wstring out(s);
        for (wchar_t& c : out)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return out;
    }

    static std::wstring Trim(std::wstring_view s)
    {
        std::size_t start = 0;
        while (start < s.size() && std::iswspace(s[start]))
        {
            ++start;
        }
        std::size_t end = s.size();
        while (end > start && std::iswspace(s[end - 1]))
        {
            --end;
        }
        return std::wstring(s.substr(start, end - start));
    }

    void Parse(const std::wstring& content)
    {
        std::wstring currentSection;
        std::size_t pos = 0;
        const std::size_t len = content.size();

        while (pos < len)
        {
            std::size_t eol = pos;
            while (eol < len && content[eol] != L'\n' && content[eol] != L'\r')
            {
                ++eol;
            }

            const std::wstring_view line(content.data() + pos, eol - pos);

            pos = eol;
            if (pos < len && content[pos] == L'\r')
            {
                ++pos;
            }
            if (pos < len && content[pos] == L'\n')
            {
                ++pos;
            }

            std::size_t start = 0;
            while (start < line.size() && std::iswspace(line[start]))
            {
                ++start;
            }
            if (start == line.size())
            {
                continue;
            }

            const wchar_t first = line[start];
            if (first == L';' || first == L'#')
            {
                continue;
            }

            if (first == L'[')
            {
                const std::size_t close = line.find(L']', start + 1);
                if (close != std::wstring_view::npos)
                {
                    currentSection = ToLower(Trim(line.substr(start + 1, close - start - 1)));
                }
                continue;
            }

            const std::size_t eq = line.find(L'=', start);
            if (eq != std::wstring_view::npos)
            {
                std::wstring key = ToLower(Trim(line.substr(start, eq - start)));
                std::wstring val = Trim(line.substr(eq + 1));
                const std::size_t semi = val.find(L';');
                if (semi != std::wstring::npos)
                {
                    val = Trim(val.substr(0, semi));
                }
                if (!key.empty())
                {
                    m_sections[currentSection][std::move(key)] = std::move(val);
                }
            }
        }
    }
};
