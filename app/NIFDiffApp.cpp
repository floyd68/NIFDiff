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
    constexpr wchar_t kSingleInstanceMutex[] = L"Local\\NIFDiff_SingleInstance";

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

    // Ensures `view` has at least `count` panes (growing via AddPane, capped
    // at NifCompareView::kMaxPanes) before loading files into them by index.
    void EnsurePaneCount(NifCompareView& view, std::size_t count)
    {
        while (view.PaneCount() < count && view.PaneCount() < NifCompareView::kMaxPanes)
        {
            if (!view.AddPane())
                break;
        }
    }

    void LoadFilesIntoPanes(NifCompareView& view, const std::vector<std::wstring>& paths)
    {
        if (paths.empty())
            return;
        EnsurePaneCount(view, paths.size());
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

        // ---------------------------------------------------------------
        // Start the IPC server as soon as the HWND exists (FICture2 starts
        // it pre-AddWnd for the same reason: the client's connect budget is
        // 500ms, so the pipe must be listening before any further startup
        // work). The callback runs on the IPC thread; it marshals the
        // request onto the UI thread via WM_FD2D_BROADCAST (handled by
        // NifCompareView::OnCommandEvent) and waits up to 800ms.
        // ---------------------------------------------------------------
        if (!hadExistingInstance)
        {
            AppIpc::StartServer([weakBackplate](const std::wstring& path) -> AppIpc::Decision
            {
                auto bp = weakBackplate.lock();
                if (!bp || bp->Window() == nullptr)
                {
                    NIFLOG_WARN("[IPC] Server: backplate expired or window gone - ignoring.");
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

                if (!PostMessageW(bp->Window(), FD2D::Backplate::WM_FD2D_BROADCAST, 0, reinterpret_cast<LPARAM>(bm)))
                {
                    NIFLOG_ERROR("[IPC] Server: PostMessageW failed (err={}) - ignoring.", GetLastError());
                    CloseHandle(doneEvent);
                    delete req;
                    delete bm;
                    return AppIpc::Decision::Ignore;
                }

                const DWORD wait = WaitForSingleObject(doneEvent, 800);
                if (wait != WAIT_OBJECT_0)
                {
                    // Deliberate deviation from FICture2's original, which
                    // deletes req and closes the event here: the posted
                    // message still references them, so a UI thread that
                    // processes it *after* this timeout would touch freed
                    // memory / a recycled handle. Abandon them instead - a
                    // one-allocation leak on an exceptional path.
                    NIFLOG_WARN("[IPC] Server: UI thread did not respond within 800ms - "
                        "returning Ignore (request intentionally abandoned, not freed).");
                    return AppIpc::Decision::Ignore;
                }

                AppIpc::Decision decision = AppIpc::Decision::Ignore;
                if (req->opened)
                {
                    decision = AppIpc::Decision::OpenedInPane;
                    // Bring the compare window forward; the sending process
                    // granted us foreground rights via
                    // AllowSetForegroundWindow before connecting.
                    if (IsIconic(bp->Window()))
                        ShowWindowAsync(bp->Window(), SW_RESTORE);
                    SetForegroundWindow(bp->Window());
                }

                CloseHandle(doneEvent);
                delete req;
                return decision;
            });
        }

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

        backplate->SetOnBeforeDestroy([iniPath, weakView, weakResolver](HWND hwnd)
        {
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
