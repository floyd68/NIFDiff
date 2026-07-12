// AppSetup.cpp - ported from FICture2's AppSetup.cpp standalone-flavor path
// (NIFDiff has no Store/Winget flavor, so the build-flavor branches
// collapse): HKCU-only ProgID + modern Capabilities model (for the Windows
// Default Apps UI) + legacy Applications/OpenWithProgids metadata + a
// best-effort direct extension mapping, followed by SHChangeNotify.
#include "AppSetup.h"
#include "AppSettings.h"
#include "../core/NifLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winreg.h>

#include <filesystem>
#include <vector>

namespace
{
    constexpr wchar_t kAppName[] = L"NIFDiff";
    constexpr wchar_t kProgId[] = L"NIFDiff.Model";
    constexpr wchar_t kProgIdDescription[] = L"NIF Model";
    constexpr wchar_t kSectionGeneral[] = L"General";

    // Extensions this viewer can actually open (NifDocument's parser scope).
    // NifSkope also registers .btr/.bto terrain shapes and .kf animations,
    // but this parser does not handle those block sets, so don't claim them.
    const std::vector<std::wstring>& SupportedExtensions()
    {
        static const std::vector<std::wstring> exts { L".nif" };
        return exts;
    }

    bool SetRegSzValue(HKEY root, const std::wstring& subKey, const wchar_t* valueNameOrNull, const std::wstring& value)
    {
        HKEY key = nullptr;
        DWORD disp = 0;
        const LONG rc = RegCreateKeyExW(
            root, subKey.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, &disp);
        if (rc != ERROR_SUCCESS)
            return false;

        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        const LONG rc2 = RegSetValueExW(
            key, valueNameOrNull /* nullptr => (Default) */, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), bytes);
        RegCloseKey(key);
        return rc2 == ERROR_SUCCESS;
    }

    bool RegisterPerUserFileAssociations(const std::wstring& exePath)
    {
        const std::vector<std::wstring>& extensions = SupportedExtensions();
        if (exePath.empty() || extensions.empty())
            return false;

        const std::wstring exeName = std::filesystem::path(exePath).filename().wstring();
        const std::wstring cmd = L"\"" + exePath + L"\" \"%1\"";
        const std::wstring icon = L"\"" + exePath + L"\",0";

        bool ok = true;

        // ProgID (HKCU only).
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId, nullptr, kProgIdDescription);
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId + L"\\DefaultIcon", nullptr, icon);
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command", nullptr, cmd);

        // Modern Capabilities model (HKCU) - required for the Windows
        // Settings > Default Apps UI to list the app.
        const std::wstring capabilitiesKey = std::wstring(L"Software\\") + kAppName + L"\\Capabilities";
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, capabilitiesKey, L"ApplicationName", kAppName);
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, capabilitiesKey, L"ApplicationDescription", L"NIFDiff - NIF Model Compare Viewer");
        ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName, capabilitiesKey);

        for (const std::wstring& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
                continue;
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, capabilitiesKey + L"\\FileAssociations", ext.c_str(), kProgId);
        }

        // Legacy Application + OpenWith metadata for broader shell
        // compatibility ("Open with" menu).
        if (!exeName.empty())
        {
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\shell\\open\\command", nullptr, cmd);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\DefaultIcon", nullptr, icon);
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName, L"FriendlyAppName", kAppName);

            for (const std::wstring& ext : extensions)
            {
                if (ext.empty() || ext[0] != L'.')
                    continue;
                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\" + exeName + L"\\SupportedTypes\\" + ext, nullptr, L"");
                (void)SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext + L"\\OpenWithProgids\\" + kProgId, nullptr, L"");
            }
        }

        // Best-effort direct mapping (may be ignored when a UserChoice policy
        // is active; the Capabilities registration above is the reliable path).
        for (const std::wstring& ext : extensions)
        {
            if (ext.empty() || ext[0] != L'.')
                continue;
            ok = ok && SetRegSzValue(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, nullptr, kProgId);
        }

        // Notify Explorer that associations changed.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        NIFLOG_INFO("[Setup] Per-user .nif association registration {}.", ok ? "succeeded" : "partially failed");
        return ok;
    }

    std::wstring GetExePath()
    {
        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        return exePath;
    }
}

namespace nsk::AppSetup
{
    void RunFirstRunAssociationPromptIfNeeded(const std::wstring& iniPath)
    {
        if (iniPath.empty())
            return;

        const AppSettings settings = ::nsk::AppSettings::Load(iniPath);
        if (settings.GetInt(kSectionGeneral, L"AskedAssociations", 0) != 0)
            return;

        const bool enabled = RegisterFileAssociations(nullptr);

        ::nsk::AppSettings::SetInt(iniPath, kSectionGeneral, L"AskedAssociations", 1);
        ::nsk::AppSettings::SetInt(iniPath, kSectionGeneral, L"AssociationsEnabled", enabled ? 1 : 0);
    }

    bool RegisterFileAssociations(HWND owner)
    {
        const int choice = MessageBoxW(
            owner,
            L"Register NIFDiff as the default viewer for .nif model files?\n"
            L"(This configures a per-user association - no administrator rights needed.)\n\n"
            L"Do you want to apply this now?",
            L"NIFDiff - File Associations",
            MB_ICONQUESTION | MB_YESNO);
        if (choice != IDYES)
            return false;

        return RegisterPerUserFileAssociations(GetExePath());
    }
}
