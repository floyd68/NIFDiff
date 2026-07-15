#include "NIFDiffApp.h"

#include "AppIpc.h"
#include "AppSettings.h"
#include "AppSetup.h"
#include "FileDialog.h"
#include "../ui/IpcOpenRequest.h"
#include "../ui/NifCompareView.h"
#include "../ui/ThumbnailStrip.h"
#include "../core/NifLog.h"
#include "../core/ResourceManager.h"
#include "../core/ResourceResolver.h"
#include "../core/StartupTrace.h"
#include "../render/TextureRepository.h"
#include "../render/RenderDevice.h"

#include <Application.h>
#include <Backplate.h>
#include <Core.h>
#include <DockPanel.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string_view>
#include <shellapi.h>
#include <objbase.h>

namespace nsk
{
namespace
{
    constexpr wchar_t kIniFileName[] = L"NIFDiff.ini";
    constexpr wchar_t kSectionWindow[] = L"Window";
    constexpr wchar_t kSectionSession[] = L"Session";
    constexpr wchar_t kSectionResources[] = L"Resources";
    constexpr wchar_t kSectionGeneral[] = L"General";
    constexpr wchar_t kSingleInstanceMutex[] = L"Local\\NIFDiff_SingleInstance";

    // Published through the ipcUiWindow atomic once shutdown begins, so
    // future IPC accepts stop posting drain broadcasts at a dying window
    // (the accept itself is already declined via IpcOpenQueue's
    // shuttingDown flag).
    const HWND kIpcUiWindowGone = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));

#ifndef NIFDIFF_VERSION_WSTR
#define NIFDIFF_VERSION_WSTR L"(dev)"
#endif

    // App context menu (right-click anywhere that is not a camera drag).
    constexpr UINT kMenuIdAbout = 1;
    constexpr UINT kMenuIdFileAssociation = 2;
    constexpr UINT kMenuIdExit = 3;
    constexpr UINT kMenuIdOpenPane = 4;
    constexpr UINT kMenuIdClosePane = 5;
    constexpr UINT kMenuIdOpenFolder = 6;
    constexpr UINT kMenuIdSaveScreenshot = 7;
    constexpr UINT kMenuIdClearRecent = 8;
    constexpr UINT kMenuIdToggleThumbnailStrip = 11;
    constexpr UINT kMenuIdThumbSizeSmall = 12;
    constexpr UINT kMenuIdThumbSizeMedium = 13;
    constexpr UINT kMenuIdThumbSizeLarge = 14;
    // Recent-files (MRU) submenu entries occupy a reserved id range above
    // the fixed items: entry i uses kMenuIdRecentBase + i.
    constexpr UINT kMenuIdRecentBase = 100;
    constexpr std::size_t kMaxRecentFiles = 12;

    // MRU helpers (defined below, after the pipe-list utilities they use);
    // forward-declared here because ShowAppContextMenu references them.
    std::vector<std::wstring> LoadRecentFiles(const std::wstring& iniPath);
    void AddRecentFile(const std::wstring& iniPath, const std::wstring& path);
    void ClearRecentFiles(const std::wstring& iniPath);

    // Saves the pane's last rendered 3D frame (the offscreen color target -
    // clean render, no UI chrome) as a PNG. Defaults to the loaded NIF's
    // own folder with the first unused <stem>_screenshotN.png name; the
    // Save dialog lets the user redirect anywhere.
    void SavePaneScreenshot(HWND hwnd, NifComparePane& pane)
    {
        const NifDocument* doc = pane.Document();
        std::filesystem::path nifPath = doc ? std::filesystem::path(doc->filePath()) : std::filesystem::path();
        const std::wstring folder = nifPath.has_parent_path() ? nifPath.parent_path().wstring() : std::wstring();
        const std::wstring stem = nifPath.has_stem() ? nifPath.stem().wstring() : L"nifdiff";

        std::wstring name = stem + L"_screenshot1.png";
        for (int n = 1; !folder.empty() && n < 1000; ++n)
        {
            name = stem + L"_screenshot" + std::to_wstring(n) + L".png";
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path(folder) / name, ec))
                break;
        }

        std::wstring outPath;
        if (!ShowSavePngDialog(hwnd, folder, name, outPath))
            return;

        std::string error;
        if (!pane.Viewport().SaveScreenshot(outPath, &error))
        {
            NIFLOG_ERROR("Screenshot save failed ({}).", error);
            MessageBoxW(hwnd, (L"Failed to save screenshot:\n" + outPath).c_str(),
                        L"NIFDiff", MB_OK | MB_ICONERROR);
        }
    }

    // Opens Explorer with the pane's loaded .nif pre-selected. explorer's
    // /select verb needs no COM apartment (unlike SHOpenFolderAndSelectItems),
    // which this UI thread never initializes.
    void OpenContainingFolder(const std::wstring& filePath)
    {
        if (filePath.empty())
            return;
        const std::wstring args = L"/select,\"" + filePath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    // Menu display string for a recent path: keep the tail (filename lives
    // there) within a budget with a leading ellipsis, and double any '&' so
    // it is not eaten as a menu mnemonic.
    std::wstring MenuLabelForPath(const std::wstring& path)
    {
        constexpr std::size_t kMax = 64;
        std::wstring label = path.size() <= kMax
            ? path
            : L"..." + path.substr(path.size() - (kMax - 3));
        std::wstring escaped;
        escaped.reserve(label.size() + 4);
        for (const wchar_t c : label)
        {
            escaped += c;
            if (c == L'&') escaped += L'&';
        }
        return escaped;
    }

    // `pane` is the compare pane under the right-click (nullptr when the
    // click landed outside every pane) - the Open/Close items that used to
    // be a per-pane button row act on exactly that pane.
    void ShowAppContextMenu(HWND hwnd, POINT clientPt, const std::wstring& iniPath,
                            NifCompareView* view, NifComparePane* pane,
                            const std::function<void()>& onToggleThumbnailStrip = {},
                            bool thumbnailStripEnabled = true,
                            const std::function<void(float)>& onSetThumbnailSize = {},
                            float currentThumbnailSize = ThumbnailStrip::kSizeMedium)
    {
        POINT screenPt = clientPt;
        ClientToScreen(hwnd, &screenPt);

        // The Register/Unregister item reflects the current HKCU state, so
        // the same slot always offers the action that makes sense right now.
        const bool registered = AppSetup::AreFileAssociationsRegistered();

        // Loaded once here so the switch below can map a chosen entry id
        // back to its path (entry i -> recentFiles[i]).
        const std::vector<std::wstring> recentFiles = LoadRecentFiles(iniPath);

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
            return;
        HMENU recentMenu = nullptr; // owned by `menu` once attached; freed with it
        if (view != nullptr && pane != nullptr)
        {
            AppendMenuW(menu, MF_STRING, kMenuIdOpenPane, L"&Open .nif in This Pane...");

            // Recent-files (MRU) submenu: open a previously loaded .nif into
            // this pane. Grayed out with no history yet.
            recentMenu = CreatePopupMenu();
            if (recentMenu != nullptr && !recentFiles.empty())
            {
                for (std::size_t i = 0; i < recentFiles.size(); ++i)
                    AppendMenuW(recentMenu, MF_STRING, kMenuIdRecentBase + static_cast<UINT>(i),
                                MenuLabelForPath(recentFiles[i]).c_str());
                AppendMenuW(recentMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(recentMenu, MF_STRING, kMenuIdClearRecent, L"&Clear Recent Files");
            }
            const UINT recentFlags = MF_POPUP | (recentFiles.empty() ? MF_GRAYED : MF_ENABLED);
            AppendMenuW(menu, recentFlags, reinterpret_cast<UINT_PTR>(recentMenu), L"Open &Recent");

            // Closing the last remaining pane is never allowed (see
            // NifCompareView::QueueClosePane) - gray the item out instead of
            // offering a click that silently does nothing.
            const UINT closeFlags = MF_STRING |
                (view->PaneCount() > NifCompareView::kMinPanes ? MF_ENABLED : MF_GRAYED);
            AppendMenuW(menu, closeFlags, kMenuIdClosePane, L"&Close This Pane");
            // Only meaningful once a file is loaded in this pane.
            const NifDocument* doc = pane->Document();
            const UINT folderFlags = MF_STRING |
                (doc != nullptr && !doc->filePath().empty() ? MF_ENABLED : MF_GRAYED);
            AppendMenuW(menu, folderFlags, kMenuIdOpenFolder, L"Open Containing &Folder");
            AppendMenuW(menu, MF_STRING, kMenuIdSaveScreenshot, L"Save Pane &Screenshot...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        if (onToggleThumbnailStrip)
        {
            AppendMenuW(menu, MF_STRING, kMenuIdToggleThumbnailStrip,
                        thumbnailStripEnabled ? L"&Hide Thumbnail Strips"
                                              : L"&Show Thumbnail Strips");
            if (onSetThumbnailSize)
            {
                // "Thumbnail Size" submenu with a radio check on the current size.
                HMENU sizeMenu = CreatePopupMenu();
                auto sizeItem = [&](UINT id, const wchar_t* label, float extent)
                {
                    const bool on = currentThumbnailSize > extent - 0.5f &&
                                    currentThumbnailSize < extent + 0.5f;
                    AppendMenuW(sizeMenu, MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED), id, label);
                };
                sizeItem(kMenuIdThumbSizeSmall,  L"&Small",  ThumbnailStrip::kSizeSmall);
                sizeItem(kMenuIdThumbSizeMedium, L"&Medium", ThumbnailStrip::kSizeMedium);
                sizeItem(kMenuIdThumbSizeLarge,  L"&Large",  ThumbnailStrip::kSizeLarge);
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), L"Thumbnail Si&ze");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, kMenuIdAbout, L"&About NIFDiff...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuIdFileAssociation,
                    registered ? L"&Unregister File Association" : L"&Register File Association");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuIdExit, L"E&xit");

        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screenPt.x, screenPt.y, hwnd, nullptr));
        DestroyMenu(menu); // also destroys the attached recent submenu

        // Recent-files entries share a contiguous id range, so they are
        // handled here rather than in the fixed-id switch: open the chosen
        // path into the clicked pane (same "replace this pane" semantics as
        // "Open .nif in This Pane").
        if (cmd >= kMenuIdRecentBase &&
            cmd < kMenuIdRecentBase + static_cast<UINT>(recentFiles.size()))
        {
            if (view != nullptr && pane != nullptr)
            {
                const std::wstring& path = recentFiles[cmd - kMenuIdRecentBase];
                std::string error;
                if (!pane->Load(path, &error))
                    MessageBoxW(hwnd, (L"Could not open the recent file:\n" + path).c_str(),
                                L"NIFDiff", MB_OK | MB_ICONWARNING);
            }
            return;
        }

        switch (cmd)
        {
        case kMenuIdOpenPane:
            if (view != nullptr && pane != nullptr)
                view->RequestOpenPane(*pane);
            break;

        case kMenuIdClosePane:
            if (view != nullptr && pane != nullptr)
                view->RequestClosePane(*pane);
            break;

        case kMenuIdOpenFolder:
            if (pane != nullptr && pane->Document() != nullptr)
                OpenContainingFolder(pane->Document()->filePath());
            break;

        case kMenuIdSaveScreenshot:
            if (pane != nullptr)
                SavePaneScreenshot(hwnd, *pane);
            break;

        case kMenuIdClearRecent:
            ClearRecentFiles(iniPath);
            break;

        case kMenuIdToggleThumbnailStrip:
            if (onToggleThumbnailStrip)
                onToggleThumbnailStrip();
            break;

        case kMenuIdThumbSizeSmall:
            if (onSetThumbnailSize) onSetThumbnailSize(ThumbnailStrip::kSizeSmall);
            break;
        case kMenuIdThumbSizeMedium:
            if (onSetThumbnailSize) onSetThumbnailSize(ThumbnailStrip::kSizeMedium);
            break;
        case kMenuIdThumbSizeLarge:
            if (onSetThumbnailSize) onSetThumbnailSize(ThumbnailStrip::kSizeLarge);
            break;

        case kMenuIdAbout:
            MessageBoxW(hwnd,
                L"NIFDiff - NIF Model Compare\n"
                L"Version " NIFDIFF_VERSION_WSTR,
                L"About NIFDiff",
                MB_ICONINFORMATION | MB_OK);
            break;

        case kMenuIdFileAssociation:
            if (registered)
            {
                if (AppSetup::UnregisterFileAssociations(hwnd))
                    AppSettings::SetInt(iniPath, kSectionGeneral, L"AssociationsEnabled", 0);
            }
            else
            {
                if (AppSetup::RegisterFileAssociations(hwnd))
                    AppSettings::SetInt(iniPath, kSectionGeneral, L"AssociationsEnabled", 1);
            }
            break;

        case kMenuIdExit:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        default: // dismissed
            break;
        }
    }

    std::wstring GetIniFilePath()
    {
        wchar_t exePath[MAX_PATH] {};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        return (std::filesystem::path(exePath).parent_path() / kIniFileName).wstring();
    }

    std::vector<std::wstring> SplitPipeList(std::wstring_view s)
    {
        std::vector<std::wstring> out;
        std::size_t start = 0;
        while (start < s.size())
        {
            std::size_t bar = s.find(L'|', start);
            if (bar == std::wstring_view::npos)
                bar = s.size();
            std::wstring_view part = s.substr(start, bar - start);
            while (!part.empty() && iswspace(part.front())) part.remove_prefix(1);
            while (!part.empty() && iswspace(part.back())) part.remove_suffix(1);
            if (!part.empty())
                out.emplace_back(part);
            start = bar + 1;
        }
        return out;
    }

    std::wstring JoinPipeList(const std::vector<std::wstring>& parts)
    {
        std::wstring out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0) out += L'|';
            out += parts[i];
        }
        return out;
    }

    // Recent-files (MRU) list, persisted pipe-joined under [Session].
    // Read fresh from the INI each time: the list changes at runtime (every
    // open appends to it), so the startup AppSettings snapshot goes stale.
    std::vector<std::wstring> LoadRecentFiles(const std::wstring& iniPath)
    {
        const AppSettings s = AppSettings::Load(iniPath);
        std::vector<std::wstring> out = SplitPipeList(s.GetString(kSectionSession, L"RecentFiles"));
        if (out.size() > kMaxRecentFiles)
            out.resize(kMaxRecentFiles);
        return out;
    }

    void AddRecentFile(const std::wstring& iniPath, const std::wstring& path)
    {
        if (path.empty())
            return;
        std::vector<std::wstring> files = LoadRecentFiles(iniPath);
        // Case-insensitive de-dupe, then push to the front so the just-opened
        // file is always most-recent (bounded to kMaxRecentFiles on save).
        files.erase(std::remove_if(files.begin(), files.end(),
            [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
            files.end());
        files.insert(files.begin(), path);
        if (files.size() > kMaxRecentFiles)
            files.resize(kMaxRecentFiles);
        AppSettings::SetString(iniPath, kSectionSession, L"RecentFiles", JoinPipeList(files));
    }

    void ClearRecentFiles(const std::wstring& iniPath)
    {
        AppSettings::SetString(iniPath, kSectionSession, L"RecentFiles", L"");
    }

    void ApplyResourcesToUi(NifCompareView& view, const ResourceResolver& resolver)
    {
        view.SetGameDataLabel(resolver.GameData());
        view.SetOverrideCountLabel(resolver.OverrideFolders().size());
    }

    void SaveResources(const std::wstring& iniPath, const ResourceResolver& resolver)
    {
        AppSettings::SetString(iniPath, kSectionResources, L"GameData", resolver.GameData());
        AppSettings::SetString(iniPath, kSectionResources, L"OverrideFolders",
                               JoinPipeList(resolver.OverrideFolders()));
        AppSettings::SetInt(iniPath, kSectionResources, L"AutoLoadArchives",
                            resolver.AutoLoadArchives() ? 1 : 0);
    }

    void ConfigureResolverFromSettings(ResourceResolver& resolver, const AppSettings& settings)
    {
        resolver.SetAutoLoadArchives(settings.GetInt(kSectionResources, L"AutoLoadArchives", 1) != 0);

        std::wstring gameData = settings.GetString(kSectionResources, L"GameData");
        if (gameData.empty())
        {
            const auto detected = ResourceResolver::DetectGameDataFolders();
            if (!detected.empty())
                gameData = detected.front();
        }
        resolver.SetOverrideFolders(SplitPipeList(settings.GetString(kSectionResources, L"OverrideFolders")));
        resolver.SetGameData(gameData); // triggers ReloadArchives
    }

    // Command-line launch (including the Explorer file-association case):
    // size the view to exactly the given files - a single double-clicked
    // .nif gets a single pane, not the constructor's two-pane default with
    // an empty right pane. Safe here: this runs at startup before any load
    // (see SetPaneCount's comment about synchronous removal).
    void LoadFilesIntoPanes(NifCompareView& view, const std::vector<std::wstring>& paths)
    {
        if (paths.empty())
            return;
        view.SetPaneCount(paths.size());
        for (std::size_t i = 0; i < paths.size() && i < view.PaneCount(); ++i)
        {
            if (!paths[i].empty())
                view.Pane(i).Load(paths[i]);
        }
    }

    // Parameterless launch: restore the last-opened session, with the pane
    // count sized to exactly the files that still exist. First run (no
    // session keys yet) or every remembered file having since vanished both
    // degrade to a single empty pane rather than the two-pane default.
    void LoadAndOpenInitialSession(NifCompareView& view, const AppSettings& settings)
    {
        std::vector<std::wstring> paths;
        for (std::size_t i = 0; i < NifCompareView::kMaxPanes; ++i)
        {
            const std::wstring key = L"File" + std::to_wstring(i);
            std::wstring path = settings.GetString(kSectionSession, key);
            if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                paths.push_back(std::move(path));
        }

        if (paths.empty())
        {
            view.SetPaneCount(1);
            return;
        }

        view.SetPaneCount(paths.size());
        for (std::size_t i = 0; i < paths.size() && i < view.PaneCount(); ++i)
            view.Pane(i).Load(paths[i]);

        // Restore the dragged splitter positions saved with the session.
        const std::wstring ratioStr = settings.GetString(kSectionSession, L"SplitRatios");
        if (!ratioStr.empty())
        {
            std::vector<float> ratios;
            const wchar_t* p = ratioStr.c_str();
            wchar_t* end = nullptr;
            while (*p != L'\0')
            {
                const float v = std::wcstof(p, &end);
                if (end == p)
                    break;
                ratios.push_back(v);
                p = (*end == L',') ? end + 1 : end;
            }
            view.ApplySplitRatios(ratios);
        }
    }

    bool ReadWindowPlacement(const AppSettings& settings, RECT& outRect, int& outShowCmd)
    {
        if (!settings.IsLoaded())
            return false;

        const int width = settings.GetInt(kSectionWindow, L"Width", 0);
        const int height = settings.GetInt(kSectionWindow, L"Height", 0);
        if (width <= 0 || height <= 0)
            return false;

        outRect.left = settings.GetInt(kSectionWindow, L"X", 100);
        outRect.top = settings.GetInt(kSectionWindow, L"Y", 100);
        outRect.right = outRect.left + width;
        outRect.bottom = outRect.top + height;
        outShowCmd = settings.GetInt(kSectionWindow, L"ShowCmd", SW_SHOWNORMAL);
        return true;
    }

    void SaveWindowPlacement(const std::wstring& iniPath, HWND hwnd)
    {
        if (!hwnd)
            return;

        WINDOWPLACEMENT wp {};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hwnd, &wp))
            return;

        const RECT& r = wp.rcNormalPosition;
        AppSettings::SetInt(iniPath, kSectionWindow, L"X", r.left);
        AppSettings::SetInt(iniPath, kSectionWindow, L"Y", r.top);
        AppSettings::SetInt(iniPath, kSectionWindow, L"Width", r.right - r.left);
        AppSettings::SetInt(iniPath, kSectionWindow, L"Height", r.bottom - r.top);
        AppSettings::SetInt(iniPath, kSectionWindow, L"ShowCmd",
            wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }

    void SaveSession(const std::wstring& iniPath, NifCompareView& view)
    {
        for (std::size_t i = 0; i < NifCompareView::kMaxPanes; ++i)
        {
            const std::wstring key = L"File" + std::to_wstring(i);
            std::wstring path;
            if (i < view.PaneCount())
                path = view.Pane(i).CurrentPath(); // pending path too, if still loading
            AppSettings::SetString(iniPath, kSectionSession, key, path);
        }

        // Dragged splitter positions, comma-joined in the view's pre-order
        // walk (see NifCompareView::CaptureSplitRatios).
        std::wstring ratios;
        for (const float r : view.CaptureSplitRatios())
        {
            if (!ratios.empty())
                ratios += L",";
            ratios += std::format(L"{:.4f}", r);
        }
        AppSettings::SetString(iniPath, kSectionSession, L"SplitRatios", ratios);
    }

    // Up to NifCompareView::kMaxPanes positional .nif paths from the command line.
    std::vector<std::wstring> GetInitialFilesFromCommandLine()
    {
        std::vector<std::wstring> out;
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return out;

        for (int i = 1; i < argc && out.size() < NifCompareView::kMaxPanes; ++i)
        {
            const std::wstring arg = argv[i] ? argv[i] : L"";
            if (arg.empty() || arg.front() == L'-')
                continue;
            const DWORD attr = GetFileAttributesW(arg.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                out.push_back(arg);
        }
        LocalFree(argv);
        return out;
    }
}

int RunNIFDiffApp(HINSTANCE hInstance, LPWSTR /*cmdLine*/, int nCmdShow)
{
    StartupTrace::Mark("RunNIFDiffApp enter");

    std::vector<std::wstring> cmdFiles;
    {
        StartupTrace::Phase p("Parse command line");
        cmdFiles = GetInitialFilesFromCommandLine();
    }

    // Single-instance detection (named mutex), same shape as FICture2's:
    // when another instance is already running and this launch carries
    // exactly one .nif path (the Explorer file-association case), forward it
    // over IPC so the running compare window gains a pane instead of a
    // second window appearing. Multi-file launches (someone explicitly
    // requesting a particular side-by-side set) and empty launches keep
    // starting their own instance.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    const bool hadExistingInstance = (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);

    if (hadExistingInstance && cmdFiles.size() == 1)
    {
        StartupTrace::Phase p("IPC client forward attempt");
        // Let the (background) running instance take foreground when it
        // accepts the file - this fresh process currently holds that right.
        AllowSetForegroundWindow(ASFW_ANY);

        AppIpc::Decision decision = AppIpc::Decision::Ignore;
        const bool sent = AppIpc::TrySendPath(cmdFiles.front(), decision);
        if (sent && decision == AppIpc::Decision::OpenedInPane)
        {
            NIFLOG_INFO("[IPC] Client: file opened in existing instance - exiting.");
            CloseHandle(instanceMutex);
            return 0;
        }
        // Ignore covers: server unavailable, every pane occupied, or the
        // file name matching nothing already open (the compare-forwarding
        // only exists for same-named files from different folders - see
        // NifCompareView::AcceptsIpcOpen). Continue as a new instance.
    }

    // ---------------------------------------------------------------------
    // Primary instance: start the IPC server IMMEDIATELY. A second
    // instance's pipe-connect budget is 500ms from *its* launch, while this
    // instance still has renderer init and game-archive scanning ahead of
    // it (measured in seconds) - starting the server after window creation
    // (FICture2's shape, this file's previous shape) made back-to-back
    // Explorer opens spawn extra windows instead of extra panes.
    //
    // The callback (a per-client AppIpc worker thread) decides on the spot
    // against the shared IpcOpenQueue - same-file-name gate + pane
    // capacity - queues the accepted path, and answers immediately. The UI
    // thread drains the queue when the CMD_NIFDIFF_IPC_OPEN broadcast
    // arrives (or right after startup, for paths accepted before the
    // window existed). It never waits for the UI to load anything, so
    // back-to-back opens can't pile clients up behind multi-second loads.
    // ---------------------------------------------------------------------
    auto ipcUiWindow = std::make_shared<std::atomic<HWND>>(nullptr);
    auto ipcQueue = std::make_shared<IpcOpenQueue>();
    if (!hadExistingInstance)
    {
        StartupTrace::Phase p("IPC server start");
        // Simultaneous multi-launch (Explorer multi-select): the losers'
        // forwards must match against what THIS instance is about to load.
        ipcQueue->SeedExpected(cmdFiles);
        AppIpc::StartServer([ipcUiWindow, ipcQueue](const std::wstring& path) -> AppIpc::Decision
        {
            if (!ipcQueue->TryEnqueue(path, NifCompareView::kMaxPanes))
                return AppIpc::Decision::Ignore;

            HWND hwnd = ipcUiWindow->load();
            if (hwnd != nullptr && hwnd != kIpcUiWindowGone)
            {
                // Wake the UI thread to drain; a null hwnd means the app is
                // still booting and will drain explicitly once the view is
                // up (see the post-initial-load drain below).
                auto* bm = new FD2D::Backplate::BroadcastMessage();
                bm->message = CMD_NIFDIFF_IPC_OPEN;
                bm->wParam = 0;
                bm->lParam = 0;
                if (!PostMessageW(hwnd, FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
                    delete bm; // queue entry stays; the startup/next drain picks it up
                // Bring the compare window forward; the sending process
                // granted us foreground rights via AllowSetForegroundWindow
                // before connecting.
                if (IsIconic(hwnd))
                    ShowWindowAsync(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }
            return AppIpc::Decision::OpenedInPane;
        });
    }

    bool oleOwned = false;
    {
        StartupTrace::Phase p("OleInitialize");
        oleOwned = SUCCEEDED(OleInitialize(nullptr));
    }

    auto& app = FD2D::Application::Instance();

    {
        StartupTrace::Phase p("FD2D Application::Initialize");
        FD2D::InitContext initContext {};
        initContext.instance = hInstance;
        if (FAILED(app.Initialize(initContext)))
        {
            if (instanceMutex) CloseHandle(instanceMutex);
            if (oleOwned) OleUninitialize();
            return -1;
        }
    }

    const std::wstring iniPath = GetIniFilePath();

    // First-run: offer to register the per-user .nif association (asked
    // once, recorded in the INI - see AppSetup.h). May block on the user,
    // so it gets its own phase to keep that wait attributable.
    {
        StartupTrace::Phase p("First-run association prompt");
        AppSetup::RunFirstRunAssociationPromptIfNeeded(iniPath);
    }

    AppSettings settings;
    {
        StartupTrace::Phase p("AppSettings::Load (INI)");
        settings = AppSettings::Load(iniPath);
    }

    auto resolver = std::make_shared<ResourceResolver>();
    {
        // The BSA/BA2 scan this kicks off runs on a background thread and
        // overlaps window/D3D init; see ResourceResolver::ReloadArchives.
        StartupTrace::Phase p("Configure resolver (launch archive scan)");
        ConfigureResolverFromSettings(*resolver, settings);
    }

    // Cross-pane texture pool. Declared after `resolver` (whose raw pointer
    // it holds) and before the backplate scope, so it outlives the view and
    // viewports that keep pointers into it.
    auto textureRepository = std::make_shared<TextureRepository>(resolver.get());

    // Single shared render core (shaders/states/IBL): built once when the
    // first viewport attaches, reused by every pane - and later by item 12's
    // thumbnail renderer - instead of one full renderer per pane. Same
    // lifetime rationale as the texture pool above.
    auto renderDevice = std::make_shared<RenderDevice>();

    // Shared async load pool (design: docs/resource-manager-design.md, phase 1;
    // currently drives thumbnail parsing). Declared before the backplate scope
    // so it outlives the panes/strips that submit to it (their destructors call
    // ResourceManager::Cancel).
    auto resourceManager = std::make_shared<ResourceManager>();
    resourceManager->Start();
    // Route texture decode+upload through the shared pool (async prefetch).
    // The repo (declared above) outlives the manager, so its completions never
    // reference a torn-down repo.
    textureRepository->SetResourceManager(resourceManager.get());

    RECT savedRect {};
    int savedShowCmd = SW_SHOWNORMAL;
    const bool hasSavedPlacement = ReadWindowPlacement(settings, savedRect, savedShowCmd);

    FD2D::WindowOptions opts {};
    opts.title = L"NIFDiff - NIF Model Compare";
    opts.instance = hInstance;
    opts.chrome = FD2D::ChromeStyle::Standard;
    if (hasSavedPlacement)
    {
        opts.x = savedRect.left;
        opts.y = savedRect.top;
        opts.width = static_cast<UINT>(savedRect.right - savedRect.left);
        opts.height = static_cast<UINT>(savedRect.bottom - savedRect.top);
    }
    else
    {
        opts.width = 1600;
        opts.height = 950;
    }

    int result = -1;
    {
        std::shared_ptr<FD2D::Backplate> backplate;
        {
            StartupTrace::Phase p("CreateWindowedBackplate (window+D3D)");
            backplate = app.CreateWindowedBackplate(L"main", opts);
        }
        if (!backplate)
        {
            app.Shutdown();
            if (oleOwned) OleUninitialize();
            return -1;
        }

        std::shared_ptr<NifCompareView> compareView;
        {
            StartupTrace::Phase p("NifCompareView construct");
            compareView = std::make_shared<NifCompareView>(L"CompareView");
        }
        compareView->SetResourceResolver(resolver.get());
        compareView->SetTextureRepository(textureRepository.get());
        compareView->SetRenderDevice(renderDevice.get());
        compareView->SetResourceManager(resourceManager.get());
        resourceManager->SetRedrawToken(backplate->GetAsyncRedrawToken());
        compareView->SetIpcOpenQueue(ipcQueue);
        ApplyResourcesToUi(*compareView, *resolver);

        // Keep the recent-files (MRU) list current: every successful open
        // (file dialog, drag&drop, command line, session restore, IPC)
        // funnels through NifComparePane::Load. Wired before the initial
        // load so command-line / restored files are recorded too.
        compareView->SetOnFileOpened([iniPath](const std::wstring& path)
        {
            AddRecentFile(iniPath, path);
        });

        std::weak_ptr<NifCompareView> weakView = compareView;
        std::weak_ptr<FD2D::Backplate> weakBackplate = backplate;
        std::weak_ptr<ResourceResolver> weakResolver = resolver;

        // Per-pane folder thumbnail strips (FICture2's ThumbnailPane model, one
        // per pane): each NifComparePane owns a strip along its bottom that
        // lists ITS open .nif's folder - sibling .nif thumbnails + subfolders +
        // an ".." tile, current file highlighted. Clicking a sibling loads it
        // into that pane; folder/".." tiles navigate that pane's strip. The
        // panes create and drive their own strips; the app only persists the
        // global on/off toggle here.
        compareView->SetOnThumbnailStripEnabledChanged([iniPath](bool on)
        {
            AppSettings::SetString(iniPath, kSectionSession, L"ThumbnailStripEnabled",
                                   on ? L"1" : L"0");
        });
        // Persist the final size after a drag-resize (or a size-menu pick).
        compareView->SetOnThumbnailStripSizeChanged([iniPath](float extent)
        {
            AppSettings::SetString(iniPath, kSectionSession, L"ThumbnailSize",
                                   std::to_wstring(static_cast<int>(extent)));
        });

        compareView->SetOnContextMenuRequested(
            [weakView, weakBackplate, iniPath](POINT clientPt, NifComparePane* pane)
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            if (!view || !bp || bp->Window() == nullptr)
                return;
            // Global show/hide for every pane's thumbnail strip (flips the same
            // VIEW-group checkbox, which broadcasts + persists).
            const bool stripEnabled = view->IsThumbnailStripEnabled();
            std::function<void()> onToggleStrip = [weakView, weakBackplate]()
            {
                auto v = weakView.lock();
                auto bp2 = weakBackplate.lock();
                if (!v || !bp2) return;
                v->ToggleThumbnailStrip();
                bp2->Render();
            };
            // "Thumbnail Size" submenu: apply to every pane + persist.
            const float currentSize = view->ThumbnailStripSize();
            std::function<void(float)> onSetSize = [weakView, weakBackplate, iniPath](float ext)
            {
                auto v = weakView.lock();
                auto bp2 = weakBackplate.lock();
                if (!v || !bp2) return;
                v->SetThumbnailStripSize(ext);
                AppSettings::SetString(iniPath, kSectionSession, L"ThumbnailSize",
                                       std::to_wstring(static_cast<int>(ext)));
                bp2->Render();
            };
            ShowAppContextMenu(bp->Window(), clientPt, iniPath, view.get(), pane,
                               onToggleStrip, stripEnabled, onSetSize, currentSize);
            bp->Render(); // deferred pane close / file open may have changed the layout
        });

        // F12 shortcut: same Save-dialog flow as the context menu item.
        compareView->SetOnScreenshotRequested([weakBackplate](NifComparePane& pane)
        {
            auto bp = weakBackplate.lock();
            if (bp && bp->Window() != nullptr)
                SavePaneScreenshot(bp->Window(), pane);
        });

        compareView->SetOnPaneOpenRequested([weakBackplate](NifComparePane& pane)
        {
            auto bp = weakBackplate.lock();
            if (!bp) return;
            std::wstring path;
            if (ShowOpenNifDialog(bp->Window(), path))
            {
                std::string error;
                pane.Load(path, &error);
                bp->Render();
            }
        });

        compareView->SetOnBrowseGameData([weakView, weakBackplate, weakResolver, iniPath]()
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            auto res = weakResolver.lock();
            if (!view || !bp || !res) return;
            std::wstring folder;
            if (!ShowPickFolderDialog(bp->Window(), L"Select Game Data Folder", folder))
                return;
            res->SetGameData(folder);
            SaveResources(iniPath, *res);
            ApplyResourcesToUi(*view, *res);
            view->InvalidateTextureCaches();
            bp->Render();
        });

        compareView->SetOnDetectGameData([weakView, weakBackplate, weakResolver, iniPath]()
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            auto res = weakResolver.lock();
            if (!view || !bp || !res) return;
            const auto detected = ResourceResolver::DetectGameDataFolders();
            if (detected.empty())
                return;
            std::wstring chosen = detected.front();
            for (const std::wstring& cand : detected)
            {
                if (_wcsicmp(cand.c_str(), res->GameData().c_str()) != 0)
                {
                    chosen = cand;
                    break;
                }
            }
            if (detected.size() == 1)
                chosen = detected.front();
            res->SetGameData(chosen);
            SaveResources(iniPath, *res);
            ApplyResourcesToUi(*view, *res);
            view->InvalidateTextureCaches();
            bp->Render();
        });

        compareView->SetOnAddOverrideFolder([weakView, weakBackplate, weakResolver, iniPath]()
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            auto res = weakResolver.lock();
            if (!view || !bp || !res) return;
            std::wstring folder;
            if (!ShowPickFolderDialog(bp->Window(), L"Add Override Folder (highest priority)", folder))
                return;
            auto folders = res->OverrideFolders();
            folders.insert(folders.begin(), folder);
            res->SetOverrideFolders(std::move(folders));
            SaveResources(iniPath, *res);
            ApplyResourcesToUi(*view, *res);
            view->InvalidateTextureCaches();
            bp->Render();
        });

        compareView->SetOnClearOverrides([weakView, weakBackplate, weakResolver, iniPath]()
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            auto res = weakResolver.lock();
            if (!view || !bp || !res) return;
            res->SetOverrideFolders({});
            SaveResources(iniPath, *res);
            ApplyResourcesToUi(*view, *res);
            view->InvalidateTextureCaches();
            bp->Render();
        });

        {
            StartupTrace::Phase p("Backplate AddWnd (view attach)");
            backplate->AddWnd(compareView);
        }

        // Explorer drag&drop -> NifCompareView::OnFileDrag/OnFileDropPaths.
        // Needs the HWND and OLE (initialized above), so this is the
        // earliest safe point.
        if (!backplate->EnsureDropTargetRegistered())
            NIFLOG_ERROR("RegisterDragDrop failed - Explorer drag&drop disabled for this run.");

        // The window is now shown BEFORE the initial pane load (below), so it
        // can be closed while the panes are still empty; don't let that
        // overwrite the saved session with blanks. Only true once loaded.
        auto initialLoadDone = std::make_shared<std::atomic<bool>>(false);
        backplate->SetOnBeforeDestroy([iniPath, weakView, weakResolver, ipcUiWindow, ipcQueue, initialLoadDone](HWND hwnd)
        {
            // Stop routing IPC opens at a window that is about to die;
            // future requests answer Ignore right away.
            ipcQueue->MarkShuttingDown();
            ipcUiWindow->store(kIpcUiWindowGone);
            SaveWindowPlacement(iniPath, hwnd);
            if (auto view = weakView.lock(); view && initialLoadDone->load())
                SaveSession(iniPath, *view);
            if (auto res = weakResolver.lock())
                SaveResources(iniPath, *res);
        });
        backplate->SetOnWindowPlacementChanged([iniPath](HWND hwnd)
        {
            SaveWindowPlacement(iniPath, hwnd);
        });

        // Show the window BEFORE the several-second initial load so it appears
        // right away instead of only after every pane + its BSA textures are
        // ready. The load stalls on the background archive scan, so keep the
        // (empty) window painting/responsive until the scan lands - then the
        // pane loads below resolve textures without a long freeze.
        const int effectiveShowCmd = hasSavedPlacement ? savedShowCmd : nCmdShow;
        {
            StartupTrace::Phase p("Backplate Show");
            backplate->Show(effectiveShowCmd);
        }
        bool windowAlive = true;
        {
            StartupTrace::Phase p("Archive scan wait (window responsive)");
            const HWND hwnd = backplate->Window();
            MSG msg;
            while (resolver && !resolver->IsArchiveScanReady())
            {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!IsWindow(hwnd)) { windowAlive = false; break; } // closed while waiting
                if (resolver && !resolver->IsArchiveScanReady())
                    Sleep(6); // brief; keeps CPU free for the scan
            }
        }

        if (windowAlive)
        {
            {
                StartupTrace::Phase p("Initial NIF load (panes)");
                if (!cmdFiles.empty())
                    LoadFilesIntoPanes(*compareView, cmdFiles);
                else
                    LoadAndOpenInitialSession(*compareView, settings);
            }
            initialLoadDone->store(true); // the session is now safe to persist

            // Restore the global thumbnail-strip on/off. Each pane's strip
            // follows its own .nif folder, which the initial load above already
            // pointed it at; nothing folder-specific is persisted. Default: on.
            {
                // Enabled unless explicitly turned off last session. Reflect it
                // in the VIEW-group checkbox and apply to every pane without
                // notifying (avoids a redundant INI write).
                const bool stripOn =
                    settings.GetString(kSectionSession, L"ThumbnailStripEnabled") != L"0";
                compareView->SetThumbnailStripEnabled(stripOn, /*notify=*/false);

                // Card size (default Medium); any saved value is accepted - the
                // strip clamps it to its valid resize range.
                const int savedSize = settings.GetInt(kSectionSession, L"ThumbnailSize",
                                                      static_cast<int>(ThumbnailStrip::kSizeMedium));
                compareView->SetThumbnailStripSize(static_cast<float>(savedSize));
            }

            // UI is fully wired (view attached, initial files loaded) - publish
            // the window so IPC accepts can post their drain broadcast at it,
            // then drain whatever was accepted while the window did not exist
            // yet. Order matters: publish FIRST, so an accept racing this point
            // either lands in the explicit drain below or posts its own
            // broadcast (processed once RunMessageLoop starts) - never neither.
            ipcUiWindow->store(backplate->Window());
            ipcQueue->MarkUiReady();
            compareView->DrainIpcOpenQueue();

            StartupTrace::Mark("Entering message loop");
            result = app.RunMessageLoop();
        }
        // Stop the pool while the panes/strips are still alive, so no worker
        // posts a completion into a half-torn-down UI.
        resourceManager->Shutdown();
    }

    app.Shutdown();

    if (oleOwned)
        OleUninitialize();

    // Held (not just created) for the entire lifetime so a second instance's
    // ERROR_ALREADY_EXISTS check stays accurate; release last.
    if (instanceMutex)
        CloseHandle(instanceMutex);

    return result;
}

} // namespace nsk
