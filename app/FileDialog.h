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

// "Save As" dialog filtered to *.png with .png as the default extension.
// initialFolder/initialFileName pre-fill the dialog (either may be empty).
bool ShowSavePngDialog(void* ownerWindowHwnd, const std::wstring& initialFolder,
                       const std::wstring& initialFileName, std::wstring& outPath);

} // namespace nsk
