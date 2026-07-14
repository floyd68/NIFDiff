#include "NIFDiffApp.h"

#include "AppIpc.h"
#include "AppSettings.h"
#include "AppSetup.h"
#include "FileDialog.h"
#include "../ui/IpcOpenRequest.h"
#include "../ui/NifCompareView.h"
#include "../core/NifLog.h"
#include "../core/ResourceResolver.h"

#include <Application.h>
#include <Backplate.h>
#include <Core.h>

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

    // IPC server response budget: how long a second instance's request may
    // wait for this instance's window to exist (startup renderer init +
    // game-archive scan take seconds) plus the UI thread processing the
    // open. The client is parked on AppIpc's Waiting ack for the duration;
    // past the budget it is told to proceed as its own instance.
    constexpr DWORD kIpcResponseBudgetMs = 10000;

    // Published through the ipcUiWindow atomic once shutdown begins, so
    // pending/future IPC requests answer Ignore immediately instead of
    // making their client sit out the full response budget.
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

    // `pane` is the compare pane under the right-click (nullptr when the
    // click landed outside every pane) - the Open/Close items that used to
    // be a per-pane button row act on exactly that pane.
    void ShowAppContextMenu(HWND hwnd, POINT clientPt, const std::wstring& iniPath,
                            NifCompareView* view, NifComparePane* pane)
    {
        POINT screenPt = clientPt;
        ClientToScreen(hwnd, &screenPt);

        // The Register/Unregister item reflects the current HKCU state, so
        // the same slot always offers the action that makes sense right now.
        const bool registered = AppSetup::AreFileAssociationsRegistered();

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
            return;
        if (view != nullptr && pane != nullptr)
        {
            AppendMenuW(menu, MF_STRING, kMenuIdOpenPane, L"&Open .nif in This Pane...");
            // Closing the last remaining pane is never allowed (see
            // NifCompareView::QueueClosePane) - gray the item out instead of
            // offering a click that silently does nothing.
            const UINT closeFlags = MF_STRING |
                (view->PaneCount() > NifCompareView::kMinPanes ? MF_ENABLED : MF_GRAYED);
            AppendMenuW(menu, closeFlags, kMenuIdClosePane, L"&Close This Pane");
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
        DestroyMenu(menu);

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
            {
                if (const NifDocument* doc = view.Pane(i).Document())
                    path = doc->filePath();
            }
            AppSettings::SetString(iniPath, kSectionSession, key, path);
        }
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
    const std::vector<std::wstring> cmdFiles = GetInitialFilesFromCommandLine();

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
        // Server unavailable, or all 4 panes occupied (Ignore): continue as
        // a new instance.
    }

    // ---------------------------------------------------------------------
    // Primary instance: start the IPC server IMMEDIATELY. A second
    // instance's pipe-connect budget is 500ms from *its* launch, while this
    // instance still has renderer init and game-archive scanning ahead of
    // it (measured in seconds) - starting the server after window creation
    // (FICture2's shape, this file's previous shape) made back-to-back
    // Explorer opens spawn extra windows instead of extra panes.
    //
    // No window exists yet, so the callback (a per-client AppIpc worker
    // thread; the client is parked on the Waiting ack for the duration)
    // first waits for `ipcUiWindow` to be published, then marshals the
    // request onto the UI thread via WM_FD2D_BROADCAST, answering Ignore
    // ("proceed as your own instance") if the whole exchange exceeds
    // kIpcResponseBudgetMs.
    // ---------------------------------------------------------------------
    auto ipcUiWindow = std::make_shared<std::atomic<HWND>>(nullptr);
    if (!hadExistingInstance)
    {
        AppIpc::StartServer([ipcUiWindow](const std::wstring& path) -> AppIpc::Decision
        {
            const ULONGLONG deadline = GetTickCount64() + kIpcResponseBudgetMs;

            HWND hwnd = ipcUiWindow->load();
            while (hwnd == nullptr && GetTickCount64() < deadline)
            {
                Sleep(25);
                hwnd = ipcUiWindow->load();
            }
            if (hwnd == nullptr || hwnd == kIpcUiWindowGone)
            {
                NIFLOG_WARN("[IPC] Server: UI window not available within {}ms "
                    "(still starting or shutting down) - client proceeds alone.", kIpcResponseBudgetMs);
                return AppIpc::Decision::Ignore;
            }

            HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (doneEvent == nullptr)
                return AppIpc::Decision::Ignore;

            auto* req = new IpcOpenRequest();
            req->path = path;
            req->doneEvent = doneEvent;
            req->opened = false;

            auto* bm = new FD2D::Backplate::BroadcastMessage();
            bm->message = CMD_NIFDIFF_IPC_OPEN;
            bm->wParam = 0;
            bm->lParam = reinterpret_cast<LPARAM>(req);

            if (!PostMessageW(hwnd, FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
            {
                NIFLOG_ERROR("[IPC] Server: PostMessageW failed (err={}) - ignoring.", GetLastError());
                CloseHandle(doneEvent);
                delete req;
                delete bm;
                return AppIpc::Decision::Ignore;
            }

            const ULONGLONG now = GetTickCount64();
            const DWORD waitMs = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(doneEvent, waitMs) != WAIT_OBJECT_0)
            {
                // Deliberate deviation from FICture2's original, which
                // deletes req and closes the event here: the posted message
                // still references them, so a UI thread that processes it
                // *after* this timeout would touch freed memory / a recycled
                // handle. Abandon them instead - a one-allocation leak on an
                // exceptional path.
                NIFLOG_WARN("[IPC] Server: UI thread did not respond within budget - "
                    "returning Ignore (request intentionally abandoned, not freed).");
                return AppIpc::Decision::Ignore;
            }

            AppIpc::Decision decision = AppIpc::Decision::Ignore;
            if (req->opened)
            {
                decision = AppIpc::Decision::OpenedInPane;
                // Bring the compare window forward; the sending process
                // granted us foreground rights via AllowSetForegroundWindow
                // before connecting.
                if (IsIconic(hwnd))
                    ShowWindowAsync(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }

            CloseHandle(doneEvent);
            delete req;
            return decision;
        });
    }

    const bool oleOwned = SUCCEEDED(OleInitialize(nullptr));

    auto& app = FD2D::Application::Instance();

    FD2D::InitContext initContext {};
    initContext.instance = hInstance;
    if (FAILED(app.Initialize(initContext)))
    {
        if (instanceMutex) CloseHandle(instanceMutex);
        if (oleOwned) OleUninitialize();
        return -1;
    }

    const std::wstring iniPath = GetIniFilePath();

    // First-run: offer to register the per-user .nif association (asked
    // once, recorded in the INI - see AppSetup.h).
    AppSetup::RunFirstRunAssociationPromptIfNeeded(iniPath);

    const AppSettings settings = AppSettings::Load(iniPath);

    auto resolver = std::make_shared<ResourceResolver>();
    ConfigureResolverFromSettings(*resolver, settings);

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
        auto backplate = app.CreateWindowedBackplate(L"main", opts);
        if (!backplate)
        {
            app.Shutdown();
            if (oleOwned) OleUninitialize();
            return -1;
        }

        auto compareView = std::make_shared<NifCompareView>(L"CompareView");
        compareView->SetResourceResolver(resolver.get());
        ApplyResourcesToUi(*compareView, *resolver);

        std::weak_ptr<NifCompareView> weakView = compareView;
        std::weak_ptr<FD2D::Backplate> weakBackplate = backplate;
        std::weak_ptr<ResourceResolver> weakResolver = resolver;

        compareView->SetOnContextMenuRequested([weakView, weakBackplate, iniPath](POINT clientPt, NifComparePane* pane)
        {
            auto view = weakView.lock();
            auto bp = weakBackplate.lock();
            if (!view || !bp || bp->Window() == nullptr)
                return;
            ShowAppContextMenu(bp->Window(), clientPt, iniPath, view.get(), pane);
            bp->Render(); // deferred pane close / file open may have changed the layout
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

        backplate->AddWnd(compareView);

        backplate->SetOnBeforeDestroy([iniPath, weakView, weakResolver, ipcUiWindow](HWND hwnd)
        {
            // Stop routing IPC opens at a window that is about to die;
            // pending/future requests answer Ignore right away.
            ipcUiWindow->store(kIpcUiWindowGone);
            SaveWindowPlacement(iniPath, hwnd);
            if (auto view = weakView.lock())
                SaveSession(iniPath, *view);
            if (auto res = weakResolver.lock())
                SaveResources(iniPath, *res);
        });
        backplate->SetOnWindowPlacementChanged([iniPath](HWND hwnd)
        {
            SaveWindowPlacement(iniPath, hwnd);
        });

        if (!cmdFiles.empty())
        {
            LoadFilesIntoPanes(*compareView, cmdFiles);
        }
        else
        {
            LoadAndOpenInitialSession(*compareView, settings);
        }

        // UI is fully wired (view attached, initial files loaded) - publish
        // the window so parked IPC requests can be posted at it. Broadcasts
        // posted from here on are processed once RunMessageLoop starts.
        ipcUiWindow->store(backplate->Window());

        const int effectiveShowCmd = hasSavedPlacement ? savedShowCmd : nCmdShow;
        backplate->Show(effectiveShowCmd);

        result = app.RunMessageLoop();
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
