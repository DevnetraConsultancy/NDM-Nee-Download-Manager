// ===========================================================================
// NDM Uninstaller (x64)
//
//   Uninstall.exe                 interactive: confirm, optionally remove the
//                                 user's NDM data, final note
//   Uninstall.exe /S [/removedata]   silent (QuietUninstallString): no
//                                 message boxes at all; /quiet = /S
//
// Stage 1 (Uninstall.exe in the install folder): confirm, stop NDM and the
// bundled tools, copy itself into a fresh admin-only temp folder and relaunch
// the copy with
//   /stage2 "<install dir>" <pid> [/S] [/removedata]
// then exit, so the install folder (including this exe) can be deleted.
// Stage 2 (temp copy): wait for stage 1 to exit, remove files (incl. tools\),
// registry entries and shortcuts, then schedule deletion of itself.
// If the temp copy cannot be started, everything runs in-process and the
// locked leftovers are deleted by a delayed cmd / on reboot.
// (Silent mode: stage 1 returns as soon as stage 2 is started; the removal
// itself finishes a few seconds later.)
//
// Per-user state: the uninstaller runs elevated, so its HKCU / profile may be
// an administrator's (over-the-shoulder UAC) rather than the user's who uses
// NDM. Setup records that user's SID (HKLM\Software\Devnetra\NDM
// InstallUserSid, see ndm_userinfo.h); their Run value, desktop shortcut and
// - on request - NDM data are removed through HKEY_USERS\<SID> and their
// profile folder. What cannot be reached (hive not loaded) is reported.
// Downloaded files are never deleted.
//
// Hardening: static CRT, only KnownDLLs are imported statically (the rest is
// delay-loaded) and SetDefaultDllDirectories(System32) runs first, so the
// stage-2 copy cannot pick up DLLs planted next to it. The stage-2 folder is
// created with a protected SY/BA-only DACL under a random name and must not
// exist yet. Deletion never follows junctions / symbolic links (files) or
// registry symbolic links, and inside a user profile it never schedules a
// delete-at-reboot.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include "../installer/ndm_procs.h"
#include "../installer/ndm_fsutil.h"
#include "../installer/ndm_integration.h"
#include "../installer/ndm_userinfo.h"
#include "ndm_uninst_logic.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define NDM_PRODUCT_NAME_W  L"Nee Download Manager"
#define NDM_EXE_NAME        L"NDMan.exe"
#define NDM_SHORTCUT_NAME   L"Nee Download Manager.lnk"
#define NDM_RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define NDM_RUN_VALUE       L"Nee Download Manager"

#define NDM_REG_UNINSTALL   \
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Nee Download Manager"
#define NDM_REG_APP         L"Software\\Devnetra\\NDM"
// Native messaging host keys written by the installer (both registry views)
static const wchar_t* const kNmHostKeys[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.devnetra.ndm",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.devnetra.ndm",
    L"Software\\Chromium\\NativeMessagingHosts\\com.devnetra.ndm",
    L"Software\\Mozilla\\NativeMessagingHosts\\com.devnetra.ndm",
};

static bool g_rebootNeeded = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring SelfPath()
{
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
    p.resize(n);
    return p;
}

static std::wstring DirOf(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

static std::wstring StripSlash(std::wstring d)
{
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

static std::wstring KnownFolderPath(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) s = p;
    if (p) CoTaskMemFree(p);
    return StripSlash(s);
}

// REG_SZ of any length from HKLM\Software\Devnetra\NDM (native 64-bit view).
static std::wstring ReadRegStr(const wchar_t* name)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NDM_REG_APP, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return L"";
    std::wstring s;
    DWORD sz = 0;
    if (RegGetValueW(h, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &sz) == ERROR_SUCCESS &&
        sz >= sizeof(wchar_t) && sz <= 64 * 1024)
    {
        s.resize(sz / sizeof(wchar_t));
        if (RegGetValueW(h, nullptr, name, RRF_RT_REG_SZ, nullptr, &s[0], &sz) == ERROR_SUCCESS)
            s.resize(wcsnlen(s.c_str(), s.size()));
        else
            s.clear();
    }
    RegCloseKey(h);
    return s;
}

// Install dir: registry (InstallDir, older builds: InstallPath), else the
// folder this exe runs from.
static std::wstring GetInstallDir()
{
    std::wstring d = ReadRegStr(L"InstallDir");
    if (d.empty()) d = ReadRegStr(L"InstallPath");
    if (d.empty() || !PathIsDirectoryW(d.c_str())) d = DirOf(SelfPath());
    return StripSlash(d);
}

// Only delete folders that really look like an NDM install, are not a system
// folder and are not reached through a junction / symbolic link.
static bool IsSafeInstallDir(const std::wstring& dir)
{
    if (dir.size() < 4 || dir[1] != L':' || dir[2] != L'\\' || PathIsRelativeW(dir.c_str()) ||
        PathIsRootW(dir.c_str()))
        return false;
    std::wstring win = KnownFolderPath(FOLDERID_Windows);
    if (!win.empty() && (NdmSamePath(dir, win) || NdmPathIsUnder(dir, win))) return false;
    const KNOWNFOLDERID* ids[] = {
        &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_System, &FOLDERID_SystemX86,
        &FOLDERID_Profile, &FOLDERID_UserProfiles, &FOLDERID_Desktop, &FOLDERID_Documents,
        &FOLDERID_ProgramData, &FOLDERID_Public,
    };
    for (const KNOWNFOLDERID* id : ids) {
        std::wstring k = KnownFolderPath(*id);
        if (!k.empty() && NdmSamePath(dir, k)) return false;
    }
    if (NdmPathHasReparsePoint(dir)) return false;
    return NdmLooksLikeInstall(dir);
}

// Delete a file without following links; if it is locked, delete it on the
// next reboot. (Admin-controlled locations only: Program Files, all-users
// Start Menu / desktop.)
static void SafeDelete(const std::wstring& path)
{
    NdmDeleteNoFollow(path, g_rebootNeeded);
}

static void SafeRemoveDir(const std::wstring& dir)
{
    if (RemoveDirectoryW(dir.c_str())) return;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
    // Children scheduled before the parent, so the parent is empty by then.
    if (MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) g_rebootNeeded = true;
}

static void StopNdmProcesses(const std::wstring& installDir)
{
    NdmAskMainWindowToClose();   // NDM_ExitNow_v1, up to ~8 s, then terminate
    // Everything running from the install folder (NDMan, NDMMsgHost,
    // tools\yt-dlp / ffmpeg / ffprobe / deno) plus NDMan.exe anywhere.
    NdmTerminateProcesses(installDir, { NDM_EXE_NAME });
    Sleep(300);   // let the OS release file locks
}

// `app` is always a full path; cmdLine quotes it.
static bool LaunchHidden(const std::wstring& app, std::wstring cmdLine)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(app.c_str(), &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static std::wstring SystemExe(const wchar_t* name)
{
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);   // x64 process: System32
    return std::wstring(sys) + L"\\" + name;
}

static bool IsElevated()
{
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) return false;
    TOKEN_ELEVATION te = {};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &sz);
    CloseHandle(hTok);
    return ok && te.TokenIsElevated;
}

// Base folder for the stage-2 copy. GetTempPath2W (Windows 11 / updated
// Windows 10) returns C:\Windows\SystemTemp for SYSTEM; for an elevated
// administrator it - like GetTempPathW - returns the per-user %TEMP%, whose
// owner (the same, non-elevated user) may rename or replace anything inside
// it. An elevated uninstaller therefore uses %SystemRoot%\Temp instead
// (standard users can create entries there but not touch ours).
static std::wstring StageTempBase()
{
    std::wstring base;
    typedef DWORD (WINAPI* PFN_GTP2)(DWORD, LPWSTR);
    PFN_GTP2 gtp2 = (PFN_GTP2)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W");
    wchar_t buf[MAX_PATH + 1] = {};
    if (gtp2 && gtp2(MAX_PATH + 1, buf)) base = StripSlash(buf);
    else if (GetTempPathW(MAX_PATH + 1, buf)) base = StripSlash(buf);

    if (IsElevated()) {
        std::wstring profiles = KnownFolderPath(FOLDERID_UserProfiles);
        std::wstring win = KnownFolderPath(FOLDERID_Windows);
        bool userTemp = base.empty() || (!profiles.empty() && NdmPathIsUnder(base, profiles));
        if (userTemp && !win.empty()) base = win + L"\\Temp";
    }
    return base;
}

// Creates <base>\NDMUninst-<random> with a protected SY/BA-only DACL.
// Fails if it already exists.
static bool CreateStageDir(std::wstring& out)
{
    std::wstring base = StageTempBase();
    std::wstring rnd = NdmRandomHex(16);
    if (base.empty() || rnd.empty() || NdmPathHasReparsePoint(base)) return false;
    std::wstring dir = base + L"\\NDMUninst-" + rnd;
    NdmSecurityDescriptor sd(NDM_SDDL_ADMIN_ONLY);
    if (!sd.sd) return false;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.sd, FALSE };
    if (!CreateDirectoryW(dir.c_str(), &sa)) return false;   // ERROR_ALREADY_EXISTS -> fail
    out = dir;
    return true;
}

// ---------------------------------------------------------------------------
// Per-user state of the user who uses NDM (see the header comment)
// ---------------------------------------------------------------------------
// A shell folder of the user whose hive is `root` (HKCU or HKEY_USERS\<SID>):
// "User Shell Folders" (REG_EXPAND_SZ, %USERPROFILE% expanded with that
// user's profile), else the expanded "Shell Folders" copy. "" if unknown.
static std::wstring UserShellFolder(HKEY root, const wchar_t* name, const std::wstring& profile)
{
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", name,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &sz) == ERROR_SUCCESS)
    {
        std::wstring p = NdmExpandProfilePath(buf, profile);
        if (!p.empty()) return p;
    }
    buf[0] = 0;
    sz = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", name,
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
    {
        std::wstring p = NdmExpandProfilePath(buf, std::wstring());   // must already be absolute
        if (!p.empty()) return p;
    }
    return std::wstring();
}

// Removes the Run value, the per-user desktop shortcut and (removeData) the
// NDM settings key + data folders of the user `sid`. self: that user is the
// one this process runs as (HKCU / own known folders). Otherwise the user's
// hive is reached as HKEY_USERS\<SID> (present while they are logged on) and
// file paths must lie inside their profile folder from HKLM ProfileList.
static void CleanUserState(const std::wstring& sid, bool self, bool removeData, std::vector<std::wstring>& notes)
{
    HKEY root = nullptr;
    bool closeRoot = false;
    std::wstring roaming, local, desktop, guard;
    if (self) {
        root    = HKEY_CURRENT_USER;
        roaming = KnownFolderPath(FOLDERID_RoamingAppData);
        local   = KnownFolderPath(FOLDERID_LocalAppData);
        desktop = KnownFolderPath(FOLDERID_Desktop);
    } else {
        const std::wstring profile = NdmProfileDirOfSid(sid);
        guard = profile;
        if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &root) == ERROR_SUCCESS) closeRoot = true;
        else root = nullptr;
        if (root) {
            roaming = UserShellFolder(root, L"AppData", profile);
            local   = UserShellFolder(root, L"Local AppData", profile);
            desktop = UserShellFolder(root, L"Desktop", profile);
        }
        if (!profile.empty()) {   // defaults when the hive is not loaded
            if (roaming.empty()) roaming = profile + L"\\AppData\\Roaming";
            if (local.empty())   local   = profile + L"\\AppData\\Local";
            if (desktop.empty()) desktop = profile + L"\\Desktop";
        }
        if (profile.empty())
            notes.push_back(L"The profile folder of the user who installed NDM (" + sid + L") was not found.");
    }
    const std::wstring who = self ? std::wstring(L"HKEY_CURRENT_USER") : L"HKEY_USERS\\" + sid;

    if (root) {
        HKEY hRun = nullptr;
        if (RegOpenKeyExW(root, NDM_RUN_KEY, 0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS) {
            RegDeleteValueW(hRun, NDM_RUN_VALUE);
            RegCloseKey(hRun);
        }
    } else {
        notes.push_back(L"Startup entry \"" NDM_RUN_VALUE L"\" of the user who installed NDM (" + sid +
                        L"): not removed, that user's registry is not loaded (not logged on). "
                        L"Windows ignores it once NDMan.exe is gone.");
    }
    if (!self && !desktop.empty() && !NdmDeleteUserFile(desktop, NDM_SHORTCUT_NAME, guard))
        notes.push_back(L"Desktop shortcut not removed: " + desktop + L"\\" NDM_SHORTCUT_NAME);

    if (removeData) {
        if (root) {
            if (!NdmRegRemoveUserProductKey(root))
                notes.push_back(L"Settings not removed: " + who + L"\\Software\\Devnetra\\NDM");
        } else {
            notes.push_back(L"Settings not removed: HKEY_USERS\\" + sid +
                            L"\\Software\\Devnetra\\NDM (that user's registry is not loaded)");
        }
        if (roaming.empty() && local.empty())
            notes.push_back(L"The NDM data folders of the user who installed NDM were not found.");
        NdmRemoveNdmDataDir(roaming, true, guard, notes);
        NdmRemoveNdmDataDir(local, false, guard, notes);
    }
    if (closeRoot) RegCloseKey(root);
}

// ---------------------------------------------------------------------------
// The actual removal
// ---------------------------------------------------------------------------
static void DoUninstall(const std::wstring& installDir, bool deleteFiles, bool removeData,
                        std::vector<std::wstring>& notes)
{
    // Who uses NDM (read before the app key is deleted): the SID setup
    // recorded, else (installs from before it was recorded) the user at
    // this session's desktop.
    const std::wstring selfSid = NdmSelfUserSid();
    std::wstring userSid = ReadRegStr(NDM_INSTALL_USER_SID_VALUE);
    if (!NdmIsPlausibleSid(userSid)) userSid = NdmInteractiveUserSid();
    const bool userIsSelf = userSid.empty() || _wcsicmp(userSid.c_str(), selfSid.c_str()) == 0;

    StopNdmProcesses(installDir);

    // 1. Shell extension COM / context menu keys (written directly by setup;
    //    no regsvr32, so no elevated process loads the DLL). Both views:
    //    the 32-bit one may still hold an old x86 registration.
    NdmUnregisterShellExt(KEY_WOW64_64KEY);
    NdmUnregisterShellExt(KEY_WOW64_32KEY);

    // 2. Native messaging host registration
    for (const wchar_t* key : kNmHostKeys)
    {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY, 0);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, KEY_WOW64_32KEY, 0);
        RegDeleteKeyW(HKEY_CURRENT_USER, key);   // dev registration (register_msghost.bat)
    }
    {
        // Manifest location used by earlier builds. Standard users can create
        // folders in ProgramData: only when no part of the path is a link,
        // and never as a delete-at-reboot.
        std::wstring pd = KnownFolderPath(FOLDERID_ProgramData);
        if (!pd.empty()) {
            std::wstring d = pd + L"\\Devnetra\\NDM\\NativeMessaging";
            if (!NdmPathHasReparsePoint(d)) {
                bool rb = false;
                NdmDeleteNoFollow(d + L"\\com.devnetra.ndm.json", rb, 0, false);
                NdmDeleteNoFollow(d + L"\\com.devnetra.ndm.firefox.json", rb, 0, false);
                RemoveDirectoryW(d.c_str());
            }
        }
    }

    // 2b. magnet: / .torrent, store extension keys, Firefox policy entry
    NdmUnregisterAssociations();
    NdmUnregisterStoreExtensions();          // no-op while the store IDs are empty
    NdmUpdateFirefoxPolicies(installDir, false);

    // 3. Installed files (only NDM's own; user files in the folder stay)
    if (deleteFiles)
    {
        // Pin the folder (no FILE_SHARE_DELETE) so neither it nor a parent can
        // be swapped for a link while we delete inside it.
        HANDLE hold = NdmOpenNoFollow(installDir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE);
        DWORD ha = 0;
        bool pinned = hold != INVALID_HANDLE_VALUE && NdmHandleAttrs(hold, ha) &&
                      (ha & FILE_ATTRIBUTE_DIRECTORY) && !(ha & FILE_ATTRIBUTE_REPARSE_POINT) &&
                      NdmSamePath(NdmFinalPath(hold), NdmLongName(installDir));
        if (pinned) NdmRemoveInstalledFiles(installDir, g_rebootNeeded);
        if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);

        if (pinned) {
            // The folder itself (only if empty: user files are never touched),
            // then the empty "Devnetra" parent.
            if (!RemoveDirectoryW(installDir.c_str()) && g_rebootNeeded)
                SafeRemoveDir(installDir);
            std::wstring parent = DirOf(installDir);
            if (_wcsicmp(PathFindFileNameW(parent.c_str()), L"Devnetra") == 0)
                RemoveDirectoryW(parent.c_str());
        }
    }

    // 4. Registry keys (native view; 32-bit view leftovers of x86 builds too)
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM v : views) {
        NdmRegDeleteTree(HKEY_LOCAL_MACHINE, NDM_REG_UNINSTALL, v);
        NdmRegDeleteTree(HKEY_LOCAL_MACHINE, NDM_REG_APP, v);
        HKEY hParent = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Devnetra", 0, KEY_READ | v, &hParent) == ERROR_SUCCESS)
        {
            DWORD subKeyCount = 0, valueCount = 0;
            RegQueryInfoKeyW(hParent, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr,
                             &valueCount, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(hParent);
            if (subKeyCount == 0 && valueCount == 0)
                NdmRegDeleteTree(HKEY_LOCAL_MACHINE, L"Software\\Devnetra", v);
        }
    }

    // 4b. Per-user state. This process's own HKCU Run value as before (the
    //     user who runs the uninstaller elevated may have used NDM too), and
    //     the state of the user who uses NDM when that is someone else.
    {
        HKEY hRun = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, NDM_RUN_KEY, 0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS) {
            RegDeleteValueW(hRun, NDM_RUN_VALUE);
            RegCloseKey(hRun);
        }
    }
    if (userIsSelf) {
        if (removeData) CleanUserState(selfSid, true, true, notes);
    } else {
        CleanUserState(userSid, false, removeData, notes);
    }

    // 5. Shortcuts (Start Menu folder, all-users and this process's desktop)
    {
        wchar_t buf[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, buf)) && buf[0]) {
            std::wstring menuDir = std::wstring(buf) + L"\\Nee Download Manager";
            SafeDelete(menuDir + L"\\" NDM_SHORTCUT_NAME);
            SafeDelete(menuDir + L"\\Uninstall NDM.lnk");
            RemoveDirectoryW(menuDir.c_str());
        }
        const int desks[] = { CSIDL_COMMON_DESKTOPDIRECTORY, CSIDL_DESKTOPDIRECTORY };
        for (int c : desks) {
            buf[0] = 0;
            if (SUCCEEDED(SHGetFolderPathW(nullptr, c, nullptr, SHGFP_TYPE_CURRENT, buf)) && buf[0])
                SafeDelete(std::wstring(buf) + L"\\" NDM_SHORTCUT_NAME);
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

// Delete `exe` (and its folder if `dir` is given) once this process is gone.
static void ScheduleSelfDelete(const std::wstring& exe, const std::wstring& dir)
{
    std::wstring cmdExe = SystemExe(L"cmd.exe");
    std::wstring cmd = L"\"" + cmdExe + L"\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" +
                       exe + L"\"";
    if (!dir.empty()) cmd += L" & rd /q \"" + dir + L"\"";
    LaunchHidden(cmdExe, cmd);
    // Backup if the cmd could not delete it.
    MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!dir.empty()) MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// Final note (interactive mode only): result, leftovers, and the browser
// extension, which an uninstaller cannot remove (it lives in the browser
// profile; a store / policy install is managed by the browser).
static void ShowDone(const std::vector<std::wstring>& notes)
{
    std::wstring msg = L"Nee Download Manager has been removed.";
    if (g_rebootNeeded) msg += L"\n\nSome files were in use and will be deleted when Windows restarts.";
    if (!notes.empty()) {
        msg += L"\n\nNot removed:";
        size_t shown = 0;
        for (const auto& n : notes) {
            if (++shown > 8) { msg += L"\n\x2022 ..."; break; }
            msg += L"\n\x2022 " + n;
        }
    }
    msg += L"\n\nPlease also remove the browser extension \"Nee Download Manager\" yourself:"
           L"\n\x2022 Chrome / Edge / Brave: open chrome://extensions (Edge: edge://extensions) and click Remove."
           L"\n\x2022 Firefox: open about:addons and choose Remove.";
    MessageBoxW(nullptr, msg.c_str(), NDM_PRODUCT_NAME_W, MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // DLL planting: resolve every (delay-)loaded DLL from System32 only.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    NdmUninstArgs args = NdmParseUninstallArgs(argv ? argc : 0, argv);
    if (argv) LocalFree(argv);
    std::vector<std::wstring> notes;

    // ---- Stage 2: running from the temp copy ------------------------------
    if (args.stage2)
    {
        const std::wstring installDir = args.stage2Dir;
        if (HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, args.parentPid)) {
            WaitForSingleObject(hp, 15000);
            CloseHandle(hp);
        }
        DoUninstall(installDir, IsSafeInstallDir(installDir), args.removeData, notes);
        if (!args.silent) ShowDone(notes);
        std::wstring self = SelfPath();
        ScheduleSelfDelete(self, DirOf(self));
        return 0;
    }

    // ---- Stage 1 ----------------------------------------------------------
    if (!args.silent) {
        int choice = MessageBoxW(nullptr,
            L"Are you sure you want to remove Nee Download Manager?\n\n"
            L"All program files, including the bundled video tools, will be deleted.",
            NDM_PRODUCT_NAME_W, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (choice != IDYES)
            return 0;
        if (!args.removeData)
            args.removeData = MessageBoxW(nullptr,
                L"Also remove my download list and settings?\n\n"
                L"This deletes NDM's download list, settings and torrent cache. "
                L"Files you have downloaded are never deleted.",
                NDM_PRODUCT_NAME_W, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }

    std::wstring installDir = GetInstallDir();
    bool safeDir = IsSafeInstallDir(installDir);
    if (!safeDir && !args.silent)
        MessageBoxW(nullptr,
                    (L"The installation folder could not be verified:\n" + installDir +
                     L"\n\nRegistry entries and shortcuts will be removed, but no files will be deleted.").c_str(),
                    NDM_PRODUCT_NAME_W, MB_OK | MB_ICONWARNING);

    StopNdmProcesses(installDir);

    std::wstring self = SelfPath();
    bool selfInDir = safeDir && NdmPathIsUnder(self, installDir);
    if (selfInDir)
    {
        // Relaunch from a protected temp copy so this exe and its folder can be deleted.
        std::wstring tdir;
        if (CreateStageDir(tdir)) {
            std::wstring copy = tdir + L"\\Uninstall.exe";
            if (CopyFileW(self.c_str(), copy.c_str(), TRUE) &&
                LaunchHidden(copy, NdmStage2CommandLine(copy, installDir, GetCurrentProcessId(),
                                                        args.silent, args.removeData)))
                return 0;
            DeleteFileW(copy.c_str());
            RemoveDirectoryW(tdir.c_str());
        }
    }

    // Fallback: uninstall in-process; our own exe / folder are removed
    // by a delayed cmd (and on reboot as a backup).
    DoUninstall(installDir, safeDir, args.removeData, notes);
    if (!args.silent) ShowDone(notes);
    if (selfInDir) ScheduleSelfDelete(self, installDir);
    return 0;
}
