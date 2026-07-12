#include "FileDialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

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
    static constexpr COMDLG_FILTERSPEC kFilters[] =
    {
        { L"NetImmerse/Gamebryo files (*.nif)", L"*.nif" },
        { L"All files (*.*)", L"*.*" },
    };
    return ShowOpenDialog(ownerWindowHwnd, L"Open NIF File", kFilters,
                          static_cast<UINT>(std::size(kFilters)),
                          FOS_FILEMUSTEXIST, outPath);
}

bool ShowPickFolderDialog(void* ownerWindowHwnd, const wchar_t* title, std::wstring& outPath)
{
    return ShowOpenDialog(ownerWindowHwnd,
                          title ? title : L"Select Folder",
                          nullptr, 0,
                          FOS_PICKFOLDERS | FOS_PATHMUSTEXIST, outPath);
}

} // namespace nsk
