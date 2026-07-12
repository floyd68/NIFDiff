#pragma once

// Vendored copy of FICture2's CommonUtil.h (D:\Works\Ficture2\CommonUtil.h),
// placed here because FD2D's Backplate.cpp, Image.cpp, ThumbImage.cpp,
// AsyncImagePipeline.cpp, ScrollView.cpp, Splitter.cpp, Application.cpp and
// Spinner.cpp all `#include "../CommonUtil.h"` relative to their own
// directory. Since FD2D is checked out here as third_party/FD2D, that
// resolves to this file. Header-only/inline, so this only needs to be
// refreshed if FICture2's original changes in a way FD2D starts relying on -
// see https://github.com/floyd68/FICture2/blob/master/CommonUtil.h.

#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <string>

namespace CommonUtil
{
    inline unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    inline float Clamp01(float v)
    {
        if (v < 0.0f)
        {
            return 0.0f;
        }
        if (v > 1.0f)
        {
            return 1.0f;
        }
        return v;
    }

    inline unsigned ToByte255(float v)
    {
        return static_cast<unsigned>(std::floor(Clamp01(v) * 255.0f + 0.5f));
    }

    inline std::wstring ToLower(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return value;
    }

    inline std::wstring ToUpper(std::wstring value)
    {
        for (auto& ch : value)
        {
            ch = static_cast<wchar_t>(towupper(ch));
        }
        return value;
    }

    inline std::wstring NormalizePath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::wstring abs;
        DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (needed > 0)
        {
            abs.resize(static_cast<size_t>(needed));
            DWORD written = GetFullPathNameW(path.c_str(), needed, &abs[0], nullptr);
            if (written > 0 && written < needed)
            {
                abs.resize(static_cast<size_t>(written));
            }
            else if (written == 0)
            {
                abs = path;
            }
        }
        else
        {
            abs = path;
        }

        std::wstring out;
        try
        {
            std::filesystem::path fp(abs);
            fp = fp.lexically_normal();
            fp.make_preferred();
            out = fp.wstring();
        }
        catch (...)
        {
            out = abs;
        }

        for (auto& ch : out)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
            ch = static_cast<wchar_t>(towlower(ch));
        }

        return out;
    }

    inline uint64_t Fnv1a64(const std::wstring& s)
    {
        uint64_t h = 14695981039346656037ull;
        for (wchar_t c : s)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    inline std::wstring Hex64(uint64_t v)
    {
        wchar_t buf[32] {};
        (void)swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(v));
        return buf;
    }

    inline std::wstring NormalizePathLowerForCompare(const std::wstring& path)
    {
        std::wstring s = ToLower(path);
        for (auto& c : s)
        {
            if (c == L'\\')
            {
                c = L'/';
            }
        }
        while (!s.empty() && s.back() == L'/')
        {
            s.pop_back();
        }
        return s;
    }

    inline std::wstring NormalizePathLowerForCompare(const std::filesystem::path& path)
    {
        return NormalizePathLowerForCompare(path.wstring());
    }

    inline bool PathEqualsInsensitive(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return NormalizePathLowerForCompare(a) == NormalizePathLowerForCompare(b);
    }

    inline bool PathEqualsInsensitive(
        const std::filesystem::path& hostA,
        const std::wstring& innerA,
        const std::filesystem::path& hostB,
        const std::wstring& innerB)
    {
        return NormalizePathLowerForCompare(hostA) == NormalizePathLowerForCompare(hostB) &&
            NormalizePathLowerForCompare(innerA) == NormalizePathLowerForCompare(innerB);
    }
}
