// FileDialog.h - Qt-free replacements for QFileDialog open-file / pick-folder.
#pragma once

#include <string>

namespace nsk
{

// Shows the standard Windows "Open" common item dialog filtered to *.nif.
// Returns true and fills outPath on success; false on cancel or failure.
bool ShowOpenNifDialog(void* ownerWindowHwnd, std::wstring& outPath);

// Folder picker (IFileOpenDialog + FOS_PICKFOLDERS). title may be null.
bool ShowPickFolderDialog(void* ownerWindowHwnd, const wchar_t* title, std::wstring& outPath);

} // namespace nsk
