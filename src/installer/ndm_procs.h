#pragma once
// ---------------------------------------------------------------------------
// Process helpers shared by the installer and the uninstaller: stop NDM and
// the bundled tools (yt-dlp, ffmpeg, ...) that run from the install folder so
// their files can be replaced or deleted.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

#define NDM_MAIN_WINDOW_CLASS L"NDMMainClass"
// Registered message handled by NDMan.exe: save the queue and exit for real
// (WM_CLOSE only hides the window when "close to tray" is on).
#define NDM_EXIT_NOW_MESSAGE  L"NDM_ExitNow_v1"

// Ask every running NDMan.exe main window to exit and wait (up to waitMs) for
// those processes to end. Older builds without the exit message get WM_CLOSE
// half-way through. Whatever is still running afterwards is terminated by
// the caller (NdmTerminateProcesses).
static inline void NdmAskMainWindowToClose(int waitMs = 8000)
{
    const UINT exitMsg = RegisterWindowMessageW(NDM_EXIT_NOW_MESSAGE);
    std::vector<HANDLE> procs;
    std::vector<HWND> wnds;
    for (HWND h = FindWindowExW(nullptr, nullptr, NDM_MAIN_WINDOW_CLASS, nullptr); h && wnds.size() < 16;
         h = FindWindowExW(nullptr, h, NDM_MAIN_WINDOW_CLASS, nullptr))
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!p) continue;
        wnds.push_back(h);
        procs.push_back(p);
        PostMessageW(h, exitMsg ? exitMsg : WM_CLOSE, 0, 0);
    }
    if (procs.empty()) return;
    const DWORD start = GetTickCount();
    bool legacyClose = false;
    for (;;) {
        bool alive = false;
        for (HANDLE p : procs)
            if (WaitForSingleObject(p, 0) == WAIT_TIMEOUT) { alive = true; break; }
        DWORD el = GetTickCount() - start;
        if (!alive || el >= (DWORD)waitMs) break;
        if (!legacyClose && exitMsg && el >= (DWORD)waitMs / 2) {
            legacyClose = true;
            for (HWND h : wnds) if (IsWindow(h)) PostMessageW(h, WM_CLOSE, 0, 0);
        }
        WaitForMultipleObjects((DWORD)procs.size(), procs.data(), TRUE, 100);
    }
    for (HANDLE p : procs) CloseHandle(p);
}

static inline bool NdmPathIsUnder(const std::wstring& path, std::wstring dir)
{
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty() || path.size() <= dir.size() + 1) return false;
    return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0 &&
           (path[dir.size()] == L'\\' || path[dir.size()] == L'/');
}

// Terminate every process whose image lives under `dir` (except this process),
// plus any process whose image name is in `names` regardless of location.
// Returns the number of processes terminated.
static inline int NdmTerminateProcesses(const std::wstring& dir, const std::vector<std::wstring>& names)
{
    int killed = 0;
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::vector<HANDLE> waits;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        bool byName = false;
        for (const auto& n : names)
            if (_wcsicmp(pe.szExeFile, n.c_str()) == 0) { byName = true; break; }

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                               FALSE, pe.th32ProcessID);
        if (!h) continue;
        bool match = byName;
        if (!match && !dir.empty()) {
            wchar_t img[MAX_PATH * 2] = {};
            DWORD n = (DWORD)(sizeof(img) / sizeof(img[0]));
            if (QueryFullProcessImageNameW(h, 0, img, &n))
                match = NdmPathIsUnder(img, dir);
        }
        if (match && TerminateProcess(h, 0)) { killed++; waits.push_back(h); }
        else CloseHandle(h);
    }
    CloseHandle(snap);
    for (HANDLE h : waits) { WaitForSingleObject(h, 3000); CloseHandle(h); }
    return killed;
}
