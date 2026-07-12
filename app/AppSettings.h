// AppSettings.h - Qt-free replacement for QSettings-based state persistence
// (Phase 4's "설정저장(SimpleIniFile 패턴)" item).
//
// Vendored, self-contained variant of FICture2's SimpleIniFile.h pattern
// (in-memory single-pass INI read) plus a small write-back helper built on
// WritePrivateProfileStringW, exactly matching the comment in the original
// file: "Writes ... are left unchanged; they are called only at shutdown /
// save points and do not contribute to startup latency." This copy drops
// FICture2's CommonUtil::ToLower dependency (inlined below) so it has zero
// dependencies beyond the Win32 API, per the plan's "vendoring" note.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cwctype>
#include <cwchar>
#include <cstdio>

namespace nsk
{

class AppSettings
{
public:
    static AppSettings Load(const std::wstring& path)
    {
        AppSettings ini;
        ini.m_path = path;

        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr)
            return ini;

        std::fseek(fp, 0, SEEK_END);
        long flen = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (flen <= 0) { std::fclose(fp); return ini; }

        std::vector<std::uint8_t> raw(static_cast<std::size_t>(flen));
        std::size_t nRead = std::fread(raw.data(), 1, raw.size(), fp);
        std::fclose(fp);
        if (nRead == 0)
            return ini;

        std::wstring content;
        if (nRead >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        {
            std::size_t wchars = (nRead - 2) / sizeof(wchar_t);
            content.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), wchars);
        }
        else
        {
            int wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(raw.data()), static_cast<int>(nRead), nullptr, 0);
            if (wlen > 0)
            {
                content.resize(static_cast<std::size_t>(wlen));
                MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(raw.data()), static_cast<int>(nRead), content.data(), wlen);
            }
        }
        ini.Parse(content);
        ini.m_loaded = true;
        return ini;
    }

    // Read-path lookups take wstring_view: section/key are only lowered into
    // a fresh key string, never stored or passed to a C API needing null
    // termination (unlike SetString below, which keeps const wstring& for
    // WritePrivateProfileStringW's c_str()).
    std::wstring GetString(std::wstring_view section, std::wstring_view key, const std::wstring& defaultVal = L"") const
    {
        auto sIt = m_sections.find(ToLower(section));
        if (sIt == m_sections.end()) return defaultVal;
        auto kIt = sIt->second.find(ToLower(key));
        if (kIt == sIt->second.end()) return defaultVal;
        return kIt->second;
    }

    int GetInt(std::wstring_view section, std::wstring_view key, int defaultVal = 0) const
    {
        std::wstring s = GetString(section, key);
        if (s.empty()) return defaultVal;
        wchar_t* end = nullptr;
        const long v = std::wcstol(s.c_str(), &end, 10);
        return (end == s.c_str()) ? defaultVal : static_cast<int>(v);
    }

    float GetFloat(std::wstring_view section, std::wstring_view key, float defaultVal = 0.0f) const
    {
        std::wstring s = GetString(section, key);
        if (s.empty()) return defaultVal;
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return (end == s.c_str()) ? defaultVal : static_cast<float>(v);
    }

    bool IsLoaded() const { return m_loaded; }

    // Writes (see class doc comment): a handful of calls at shutdown only,
    // so per-call INI reopen/reparse cost is a non-issue here.
    static void SetString(const std::wstring& path, const std::wstring& section, const std::wstring& key, const std::wstring& value)
    {
        WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), path.c_str());
    }
    static void SetInt(const std::wstring& path, const std::wstring& section, const std::wstring& key, int value)
    {
        SetString(path, section, key, std::to_wstring(value));
    }
    static void SetFloat(const std::wstring& path, const std::wstring& section, const std::wstring& key, float value)
    {
        SetString(path, section, key, std::to_wstring(value));
    }

private:
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> m_sections;
    bool m_loaded = false;
    std::wstring m_path;

    static std::wstring ToLower(std::wstring_view s)
    {
        std::wstring out(s);
        for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
        return out;
    }

    static std::wstring Trim(std::wstring_view s)
    {
        std::size_t start = 0;
        while (start < s.size() && std::iswspace(s[start])) ++start;
        std::size_t end = s.size();
        while (end > start && std::iswspace(s[end - 1])) --end;
        return std::wstring(s.substr(start, end - start));
    }

    void Parse(const std::wstring& content)
    {
        std::wstring currentSection;
        std::size_t pos = 0, len = content.size();
        while (pos < len)
        {
            std::size_t eol = pos;
            while (eol < len && content[eol] != L'\n' && content[eol] != L'\r') ++eol;
            std::wstring_view line(content.data() + pos, eol - pos);
            pos = eol;
            if (pos < len && content[pos] == L'\r') ++pos;
            if (pos < len && content[pos] == L'\n') ++pos;

            std::size_t start = 0;
            while (start < line.size() && std::iswspace(line[start])) ++start;
            if (start == line.size()) continue;
            wchar_t first = line[start];
            if (first == L';' || first == L'#') continue;

            if (first == L'[')
            {
                std::size_t close = line.find(L']', start + 1);
                if (close != std::wstring_view::npos)
                    currentSection = ToLower(Trim(line.substr(start + 1, close - start - 1)));
                continue;
            }

            std::size_t eq = line.find(L'=', start);
            if (eq != std::wstring_view::npos)
            {
                std::wstring key = ToLower(Trim(line.substr(start, eq - start)));
                std::wstring val = Trim(line.substr(eq + 1));
                std::size_t semi = val.find(L';');
                if (semi != std::wstring::npos)
                    val = Trim(val.substr(0, semi));
                if (!key.empty())
                    m_sections[currentSection][std::move(key)] = std::move(val);
            }
        }
    }
};

} // namespace nsk
