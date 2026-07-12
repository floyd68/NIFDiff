// AppSetup.h - per-user (HKCU) .nif file association registration, ported
// from FICture2's AppSetup. Window placement persistence is NOT here - the
// app shell already owns that via AppSettings (see NIFDiffApp.cpp) - and
// FICture2's Explorer thumbnail-provider registration has no NIF
// counterpart, so only the association half is ported.
#pragma once

#include <string>

// Forward declare Win32 types without pulling in <windows.h> from this header.
struct HWND__;
using HWND = HWND__*;

namespace nsk::AppSetup
{
    // First-run is defined as "General/AskedAssociations is 0 in the INI"
    // (NIFDiff.ini already exists for current users, so INI existence -
    // FICture2's first-run signal - would never fire here). If first-run,
    // prompts the user and (optionally) registers the per-user association,
    // then records the answer so the prompt is shown only once.
    void RunFirstRunAssociationPromptIfNeeded(const std::wstring& iniPath);

    // Manual action: prompt and register the per-user (HKCU) association.
    bool RegisterFileAssociations(HWND owner);
}
