#include "FileDialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <ArchiveReader.h> // Floar: which archive extensions this build supports

#include <string>
#include <vector>

namespace nsk
{
namespace
{
    bool ShowOpenDialog(void* ownerWindowHwnd, const wchar_t* title,
                        const COMDLG_FILTERSPEC* filters, UINT filterCount,
                        DWORD extraOptions, std::wstring& outPath)
    {
        using Microsoft::WRL::ComPtr;

        ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || !dialog)
            return false;

        if (filters && filterCount > 0)
        {
            dialog->SetFileTypes(filterCount, filters);
            dialog->SetFileTypeIndex(1);
        }
        if (title)
            dialog->SetTitle(title);

        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | extraOptions);

        HWND owner = reinterpret_cast<HWND>(ownerWindowHwnd);
        hr = dialog->Show(owner);
        if (FAILED(hr))
            return false;

        ComPtr<IShellItem> item;
        hr = dialog->GetResult(&item);
        if (FAILED(hr) || !item)
            return false;

        PWSTR path = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(hr) || !path)
            return false;

        outPath = path;
        CoTaskMemFree(path);
        return true;
    }
}

bool ShowOpenNifDialog(void* ownerWindowHwnd, std::wstring& outPath)
{
    // Build the archive pattern (e.g. "*.bsa;*.ba2;*.zip;*.7z;*.rar") from the
    // formats Floar actually supports in this build, so picking an archive to
    // browse into stays in sync with ArchiveReaderFactory. The spec/label
    // strings must outlive dialog->Show(), so keep them on the stack here.
    std::wstring archivePattern;
    std::wstring archiveLabel = L"Archives (";
    for (const std::wstring& ext : Floar::ArchiveReaderFactory::GetSupportedExtensions())
    {
        // GetSupportedExtensions yields dotted extensions (".bsa", ".zip", ...).
        if (!archivePattern.empty())
        {
            archivePattern += L';';
            archiveLabel += L' ';
        }
        archivePattern += L'*';
        archivePattern += ext;
        archiveLabel += L'*';
        archiveLabel += ext;
    }
    archiveLabel += L')';

    std::vector<COMDLG_FILTERSPEC> filters;
    filters.push_back({ L"NetImmerse/Gamebryo files (*.nif)", L"*.nif" });
    if (!archivePattern.empty())
        filters.push_back({ archiveLabel.c_str(), archivePattern.c_str() });
    filters.push_back({ L"All files (*.*)", L"*.*" });

    return ShowOpenDialog(ownerWindowHwnd, L"Open NIF File or Archive", filters.data(),
                          static_cast<UINT>(filters.size()),
                          FOS_FILEMUSTEXIST, outPath);
}

bool ShowPickFolderDialog(void* ownerWindowHwnd, const wchar_t* title, std::wstring& outPath)
{
    return ShowOpenDialog(ownerWindowHwnd,
                          title ? title : L"Select Folder",
                          nullptr, 0,
                          FOS_PICKFOLDERS | FOS_PATHMUSTEXIST, outPath);
}

bool ShowSavePngDialog(void* ownerWindowHwnd, const std::wstring& initialFolder,
                       const std::wstring& initialFileName, std::wstring& outPath)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog)
        return false;

    static constexpr COMDLG_FILTERSPEC kFilters[] =
    {
        { L"PNG image (*.png)", L"*.png" },
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(kFilters)), kFilters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"png");
    dialog->SetTitle(L"Save Screenshot");
    if (!initialFileName.empty())
        dialog->SetFileName(initialFileName.c_str());
    if (!initialFolder.empty())
    {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder)
            dialog->SetFolder(folder.Get());
    }

    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

    hr = dialog->Show(reinterpret_cast<HWND>(ownerWindowHwnd));
    if (FAILED(hr))
        return false;

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item)
        return false;

    PWSTR path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(hr) || !path)
        return false;

    outPath = path;
    CoTaskMemFree(path);
    return true;
}

} // namespace nsk
