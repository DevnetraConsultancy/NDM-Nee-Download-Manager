// NDMan.cpp -- NDM application entry point
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Command line:
//   -tray | /tray | --tray     start hidden in the notification area
//   -url <address> | <address> open "Download File Info" for the address
// A second instance forwards its URL (or a "show" request) to the running
// instance via WM_COPYDATA and exits.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>

#include "../common/ndm_common.h"
#include "../core/ndm_config.h"
#include "../core/ndm_queue.h"
#include "../ui/ndm_mainwnd.h"
#include "../ui/ndm_optdlg.h"
#include "../ui/ndm_theme.h"
#include "../msghost/ndm_msghost.h"
#include "../torrent/ndm_torrent.h"
#include "../ui/ndm_firstrun.h"
#include "../ui/ndm_power.h"
#include "../ui/ndm_dialogs.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    struct Args
    {
        bool         tray = false;
        bool         firstRun = false;
        std::wstring url;
    };

    bool LooksLikeUrl(const std::wstring& s)
    {
        // http(s) URL, magnet: link, or a .torrent file (file association)
        return (s.find(L"://") != std::wstring::npos && s.size() > 8) ||
               NdmTorrentIsTorrentLink(s);
    }

    Args ParseArgs()
    {
        Args a;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return a;
        for (int i = 1; i < argc; ++i)
        {
            std::wstring s = argv[i];
            std::wstring l = s;
            for (auto& c : l) c = (wchar_t)towlower(c);
            if (l == L"-tray" || l == L"/tray" || l == L"--tray" || l == L"-minimized")
                a.tray = true;
            else if (l == L"-firstrun" || l == L"/firstrun" || l == L"--firstrun")
                a.firstRun = true;
            else if ((l == L"-url" || l == L"/url" || l == L"--url") && i + 1 < argc)
                a.url = argv[++i];
            else if (LooksLikeUrl(s) && a.url.empty())
                a.url = s;
        }
        LocalFree(argv);
        return a;
    }

    void EnableDpiAwareness()
    {
        // The manifest already asks for PerMonitorV2; this is a fallback for
        // hosts that ignore it.
        typedef BOOL (WINAPI* fnCtx)(HANDLE);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (fnCtx f = (fnCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
        {
            if (f((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/)) return;
            if (f((HANDLE)-3 /*PER_MONITOR_AWARE*/))    return;
        }
        SetProcessDPIAware();
    }

    // Forward to the running instance. Returns true if it was reached.
    bool ForwardToRunning(const Args& a)
    {
        HWND h = nullptr;
        for (int i = 0; i < 40 && !h; ++i)    // the first instance may be starting
        {
            h = FindWindowW(NDM_WINDOW_CLASS, nullptr);
            if (!h) Sleep(100);
        }
        if (!h) return false;

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid) AllowSetForegroundWindow(pid);   // we own foreground rights now

        COPYDATASTRUCT cds = {};
        if (!a.url.empty())
        {
            if (a.url.size() > 70000) return true;   // nothing sensible is that long
            cds.dwData = NDM_COPYDATA_CMDLINE;       // may be a local .torrent (file association)
            cds.cbData = (DWORD)((a.url.size() + 1) * sizeof(wchar_t));
            cds.lpData = const_cast<wchar_t*>(a.url.c_str());
        }
        else if (!a.tray)
        {
            cds.dwData = NDM_COPYDATA_SHOW;
        }
        else
            return true;                          // "-tray" while running: nothing to do
        DWORD_PTR res = 0;
        SendMessageTimeoutW(h, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                            SMTO_ABORTIFHUNG, 5000, &res);
        return true;
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // DLLs only from System32 (plus explicitly added dirs): no planting via
    // the application or current folder.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // A known current directory (our install folder), not whatever the
    // launcher had: nothing relative is ever resolved against e.g. Downloads.
    {
        wchar_t exe[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        if (n > 0 && n < sizeof(exe) / sizeof(exe[0]))
        {
            std::wstring d(exe, n);
            size_t sl = d.find_last_of(L"\\/");
            if (sl != std::wstring::npos)
            {
                d.resize(sl);
                if (d.size() == 2 && d[1] == L':') d += L'\\';
                SetCurrentDirectoryW(d.c_str());
            }
        }
    }
    Args args = ParseArgs();

    // -----------------------------------------------------------------------
    // 1. Single-instance guard
    // -----------------------------------------------------------------------
    HANDLE hMutex = CreateMutexA(nullptr, FALSE, NDM_MUTEX_NAME);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ForwardToRunning(args);
        CloseHandle(hMutex);
        return 0;
    }

    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // OLE on top (same STA): drag and drop onto the main window needs it.
    const bool oleOk = SUCCEEDED(OleInitialize(nullptr));

    INITCOMMONCONTROLSEX icc = { sizeof(icc) };
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                ICC_UPDOWN_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    NdmTheme::Init();

    // -----------------------------------------------------------------------
    // 2. Configuration + queue
    // -----------------------------------------------------------------------
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    cfg.Load();
    NdmOptDlg::ApplySettings();

    // IDM-style category folders in the download folder (Compressed,
    // Documents, Music, Programs, Video, Pictures). Existing folders are kept.
    {
        std::wstring base = cfg.GetSavePath();
        if (base.empty()) base = NdmConfigMgr::GetDefaultSavePath();
        if (!base.empty())
            for (int c = CAT_COMPRESSED; c <= CAT_PICTURES; ++c)
                SHCreateDirectoryExW(nullptr, NdmTheme::CategoryFolder(base, c).c_str(), nullptr);
    }

    std::wstring queuePath = cfg.GetAppDataPath() + L"queue.xml";
    NdmQueue::GetInstance().LoadQueue(queuePath);
    NdmQueue::GetInstance().SetSchedule(cfg.GetSchedule());

    // -----------------------------------------------------------------------
    // 3. Main window, then the browser bridge (it needs the hwnd)
    // -----------------------------------------------------------------------
    NdmMainWindow& mw = NdmMainWindow::GetInstance();
    HWND hWnd = mw.Create(hInstance, args.tray);
    if (!hWnd)
    {
        MessageBoxW(nullptr, L"Failed to create the main window.", NDM_PRODUCT_NAME,
                    MB_ICONERROR | MB_OK);
        NdmTheme::Shutdown();
        if (oleOk) OleUninitialize();
        CoUninitialize();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }
    NdmMsgHost::SetMainWindow(hWnd);
    NdmMsgHost::StartListening();
    // On-completion actions (sleep / hibernate / shut down) come back to the
    // main window, which shows the cancellable countdown.
    NdmQueue::GetInstance().SetCompletionActionCallback(NdmPower::OnQueueAction, hWnd);

    // IDM-style first-run questions (first start, or -firstrun from the installer)
    if (args.firstRun || NdmFirstRun::IsNeeded())
        NdmFirstRun::Show(args.tray ? nullptr : hWnd);

    if (!args.url.empty())
    {
        NdmBrowserRequest* req = new NdmBrowserRequest();
        req->url = args.url;
        req->origin = NDM_ORIGIN_CMDLINE;
        NdmPostBrowserRequest(hWnd, req);          // takes ownership
    }

    // -----------------------------------------------------------------------
    // 4. Message loop (modeless dialogs + accelerators)
    // -----------------------------------------------------------------------
    MSG msg = {};
    BOOL r;
    while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0)
    {
        if (r == -1) break;
        if (NdmTheme::PreTranslate(&msg)) continue;
        HWND main = mw.GetHwnd();
        if (main && mw.GetAccel() &&
            (msg.hwnd == main || IsChild(main, msg.hwnd)) &&
            msg.hwnd != mw.GetSearchHwnd() &&        // Del / Ctrl+C / Ctrl+A edit the search text
            TranslateAcceleratorW(main, mw.GetAccel(), &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // -----------------------------------------------------------------------
    // 5. Shutdown: stop the bridge, stop downloads (state kept), persist.
    // -----------------------------------------------------------------------
    NdmMsgHost::SetMainWindow(nullptr);
    NdmMsgHost::StopListening();
    NdmQueue::GetInstance().SetCompletionActionCallback(nullptr, nullptr);
    NdmDialogs::WaitForWorkers(5000);        // a "Refresh download address" check still running
    // Save first: Shutdown() stops the workers, writes the final state itself
    // and then releases every item -- saving after it would write an empty
    // queue.
    NdmQueue::GetInstance().SaveQueue(queuePath);
    // Torrent engine first: saveSession + graceful aria2 shutdown (control
    // files flushed); its running items then stop with the queue below.
    NdmTorrentShutdown();
    NdmQueue::GetInstance().Shutdown();
    cfg.Save();
    NdmOptDlg::ApplyRunKey();        // cfg.Save rewrites the Run key; keep ours (-tray)

    NdmTheme::Shutdown();
    if (oleOk) OleUninitialize();
    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}
