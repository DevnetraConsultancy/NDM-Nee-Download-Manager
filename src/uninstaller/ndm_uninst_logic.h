#pragma once
// ---------------------------------------------------------------------------
// Uninstaller logic that does not depend on running elevated against the
// real installation, kept apart from Uninstall.cpp so it can be unit-tested
// (build_p1/tests/uninsttest.cpp):
//   - command line (/S, /quiet, /removedata, internal /stage2)
//   - removal of NDM's per-user data ("Also remove my download list and
//     settings"): exactly the files and folders NDM creates, never the
//     user's downloads, never through a link
//   - registry tree removal that never follows registry symbolic links
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>
#include <vector>
#include "../installer/ndm_fsutil.h"

// ---- command line ---------------------------------------------------------
//   Uninstall.exe [/S | /quiet] [/removedata]
//   Uninstall.exe /stage2 "<install dir>" <pid> [/S] [/removedata]   (internal)
// /S (or /quiet): no message boxes at all; /removedata: also remove the
// per-user data (interactive mode asks instead). Unknown arguments are
// ignored, switches are case-insensitive and may start with '/' or '-'.
struct NdmUninstArgs
{
    bool         stage2     = false;
    std::wstring stage2Dir;           // /stage2: install folder
    DWORD        parentPid  = 0;      // /stage2: stage-1 process to wait for
    bool         silent     = false;
    bool         removeData = false;
};

static inline bool NdmIsSwitch(const wchar_t* a, const wchar_t* name)
{
    return a && (a[0] == L'/' || a[0] == L'-') && _wcsicmp(a + 1, name) == 0;
}

static inline NdmUninstArgs NdmParseUninstallArgs(int argc, const wchar_t* const* argv)
{
    NdmUninstArgs r;
    int i = 1;
    if (argv && argc >= 4 && NdmIsSwitch(argv[1], L"stage2")) {
        std::wstring d = argv[2] ? argv[2] : L"";
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        wchar_t* end = nullptr;
        unsigned long pid = argv[3] ? wcstoul(argv[3], &end, 10) : 0;
        if (!d.empty() && end && *end == 0 && pid > 0 && pid <= 0xFFFFFFFFul) {
            r.stage2    = true;
            r.stage2Dir = d;
            r.parentPid = (DWORD)pid;
            i = 4;
        }
    }
    for (; argv && i < argc; i++) {
        if (NdmIsSwitch(argv[i], L"S") || NdmIsSwitch(argv[i], L"quiet")) r.silent = true;
        else if (NdmIsSwitch(argv[i], L"removedata")) r.removeData = true;
    }
    return r;
}

// Command line of the stage-2 copy (parsed back by NdmParseUninstallArgs).
// installDir never ends with a backslash and cannot contain '"'.
static inline std::wstring NdmStage2CommandLine(const std::wstring& exe, const std::wstring& installDir, DWORD pid,
                                         bool silent, bool removeData)
{
    std::wstring c = L"\"" + exe + L"\" /stage2 \"" + installDir + L"\" " + std::to_wstring(pid);
    if (silent) c += L" /S";
    if (removeData) c += L" /removedata";
    return c;
}

// ---- per-user data ------------------------------------------------------
// Files NDM writes into %APPDATA%\Devnetra\NDM (ndm_config.cpp, ndm_queue.cpp
// SaveQueue, ndm_torrent.cpp aria2 session / DHT; aria2 writes "<f>__temp"
// first and renames it).
static const wchar_t* const kNdmRoamingDataFiles[] = {
    L"ndm.ini",
    L"queue.xml", L"queue.xml.bak", L"queue.xml.tmp",
    L"aria2.session", L"aria2.session__temp",
    L"aria2-dht.dat", L"aria2-dht.dat__temp", L"aria2-dht6.dat", L"aria2-dht6.dat__temp",
};
// Torrent metadata cache: %APPDATA%\Devnetra\NDM\torrents\<40 hex>.torrent
#define NDM_TORRENT_CACHE_DIR L"torrents"
// %LOCALAPPDATA%\Devnetra\NDM: self-updated yt-dlp (tools\) and the private
// aria2 run folder (run\). Both are NDM's own, removed as a whole.
static const wchar_t* const kNdmLocalDataDirs[] = { L"tools", L"run" };

static inline bool NdmIsCachedTorrentName(const std::wstring& n)
{
    if (n.size() != 40 + 8 || _wcsicmp(n.c_str() + 40, L".torrent") != 0) return false;
    for (size_t i = 0; i < 40; i++)
        if (!iswxdigit(n[i])) return false;
    return true;
}

// Opens `dir` without following a link and WITHOUT FILE_SHARE_DELETE, so
// neither it nor a parent can be renamed / swapped for a link while we
// delete inside it, and checks it is a real folder at exactly that path.
static inline HANDLE NdmPinDir(const std::wstring& dir)
{
    HANDLE h = NdmOpenNoFollow(dir, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (h == INVALID_HANDLE_VALUE) return h;
    DWORD a = 0;
    if (!NdmHandleAttrs(h, a) || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !NdmSamePath(NdmFinalPath(h), NdmLongName(dir)))
    {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

static inline bool NdmPathExists(const std::wstring& p)
{
    return GetFileAttributesW(NdmLongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// `guard` (may be empty): the folder must lie inside it (another user's
// profile: their shell-folder settings are theirs to change, so an elevated
// delete only ever happens inside their own profile).
static inline bool NdmUserDirAcceptable(const std::wstring& dir, const std::wstring& guard)
{
    if (dir.size() < 4 || dir[1] != L':' || dir[2] != L'\\') return false;
    if (!guard.empty() && !NdmPathIsInside(dir, guard)) return false;
    return !NdmPathHasReparsePoint(dir);
}

// Remove NDM's data below `base` (the user's Roaming or Local AppData):
// base\Devnetra\NDM loses exactly NDM's own files / folders, then is removed
// if empty, then base\Devnetra if empty. Anything else in there (a file the
// user put there, a download saved there) stays. Never deletes at reboot
// (see NdmDeleteNoFollow). `notes` receives what was left behind and why.
static inline void NdmRemoveNdmDataDir(const std::wstring& base, bool roaming, const std::wstring& guard,
                                std::vector<std::wstring>& notes)
{
    if (base.empty()) return;
    const std::wstring parent = NdmStripSlash(base) + L"\\Devnetra";
    const std::wstring dir = parent + L"\\NDM";
    if (!NdmPathExists(dir)) {
        if (NdmPathExists(parent)) RemoveDirectoryW(NdmLongPath(parent).c_str());   // only if empty
        return;
    }
    if (!NdmUserDirAcceptable(dir, guard)) {
        notes.push_back(L"Not removed (folder is outside the user profile or reached through a link): " + dir);
        return;
    }
    HANDLE pin = NdmPinDir(dir);
    if (pin == INVALID_HANDLE_VALUE) {
        notes.push_back(L"Not removed (folder could not be opened safely): " + dir);
        return;
    }
    bool reboot = false;   // unused: atReboot = false
    if (roaming) {
        for (const wchar_t* f : kNdmRoamingDataFiles) NdmDeleteNoFollow(dir + L"\\" + f, reboot, 0, false);
        const std::wstring cache = dir + L"\\" NDM_TORRENT_CACHE_DIR;
        HANDLE hc = NdmPinDir(cache);
        if (hc != INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> names;
            if (NdmListDir(hc, names))
                for (const auto& n : names)
                    if (NdmIsCachedTorrentName(n)) NdmDeleteNoFollow(cache + L"\\" + n, reboot, 0, false);
            CloseHandle(hc);
            RemoveDirectoryW(NdmLongPath(cache).c_str());   // only if empty
        }
    } else {
        for (const wchar_t* d : kNdmLocalDataDirs) NdmDeleteNoFollow(dir + L"\\" + d, reboot, 0, false);
    }
    CloseHandle(pin);
    RemoveDirectoryW(NdmLongPath(dir).c_str());      // only if empty
    if (NdmPathExists(dir)) notes.push_back(L"Kept (contains other files or files in use): " + dir);
    else RemoveDirectoryW(NdmLongPath(parent).c_str());   // "Devnetra", only if empty
}

// Delete the file dir\name (e.g. a desktop shortcut) without following links;
// `guard` as above. True if it is gone (or was not there).
static inline bool NdmDeleteUserFile(const std::wstring& dir, const std::wstring& name, const std::wstring& guard)
{
    const std::wstring path = NdmStripSlash(dir) + L"\\" + name;
    if (!NdmPathExists(path)) return true;
    if (!NdmUserDirAcceptable(dir, guard)) return false;
    HANDLE pin = NdmPinDir(dir);
    if (pin == INVALID_HANDLE_VALUE) return false;
    bool reboot = false;
    HANDLE h = NdmOpenNoFollow(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    DWORD a = 0;
    bool isFile = h != INVALID_HANDLE_VALUE && NdmHandleAttrs(h, a) && !(a & FILE_ATTRIBUTE_DIRECTORY);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    bool ok = isFile && NdmDeleteNoFollow(path, reboot, 0, false);
    CloseHandle(pin);
    return ok;
}

// ---- registry ------------------------------------------------------------
typedef LONG (NTAPI* NdmPfnNtDeleteKey)(HANDLE);

static inline NdmPfnNtDeleteKey NdmNtDeleteKey()
{
    static NdmPfnNtDeleteKey p = reinterpret_cast<NdmPfnNtDeleteKey>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey")));
    return p;
}

// A key opened with REG_OPTION_OPEN_LINK is a symbolic link if it carries
// a REG_LINK "SymbolicLinkValue".
static inline bool NdmRegIsLink(HKEY h)
{
    DWORD type = 0;
    return RegQueryValueExW(h, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
           type == REG_LINK;
}

// Delete parent\name and everything below it. Every key is opened with
// REG_OPTION_OPEN_LINK and deleted through its own handle (NtDeleteKey), so
// a symbolic link planted below is deleted as the link itself and its
// target is never touched (RegDeleteTree would follow it).
static inline bool NdmRegDeleteTreeNoFollow(HKEY parent, const std::wstring& name, int depth = 0)
{
    NdmPfnNtDeleteKey del = NdmNtDeleteKey();
    if (depth > 32 || !del || name.empty()) return false;
    HKEY h = nullptr;
    LSTATUS ls = RegOpenKeyExW(parent, name.c_str(), REG_OPTION_OPEN_LINK,
                               KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | DELETE, &h);
    if (ls == ERROR_FILE_NOT_FOUND) return true;
    if (ls != ERROR_SUCCESS) return false;
    bool ok = true;
    if (!NdmRegIsLink(h)) {
        std::vector<std::wstring> subs;
        for (DWORD i = 0; subs.size() < 100000; i++) {
            wchar_t buf[256];
            DWORD n = 256;
            if (RegEnumKeyExW(h, i, buf, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            subs.emplace_back(buf, n);
        }
        for (const auto& s : subs)
            if (!NdmRegDeleteTreeNoFollow(h, s, depth + 1)) ok = false;
    }
    if (del(h) < 0) ok = false;
    RegCloseKey(h);
    return ok;
}

// root\Software\Devnetra\NDM (the app's per-user settings), then
// root\Software\Devnetra if nothing else is left in it. Refuses if
// "Devnetra" itself is a link. True if NDM's key is gone.
static inline bool NdmRegRemoveUserProductKey(HKEY root)
{
    HKEY sw = nullptr;
    if (RegOpenKeyExW(root, L"Software", 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &sw) != ERROR_SUCCESS)
        return false;
    HKEY dev = nullptr;
    LSTATUS ls = RegOpenKeyExW(sw, L"Devnetra", REG_OPTION_OPEN_LINK,
                               KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | DELETE, &dev);
    RegCloseKey(sw);
    if (ls == ERROR_FILE_NOT_FOUND) return true;
    if (ls != ERROR_SUCCESS) return false;
    bool ok = false;
    if (!NdmRegIsLink(dev)) {
        ok = NdmRegDeleteTreeNoFollow(dev, L"NDM");
        DWORD subs = 1, vals = 1;
        if (RegQueryInfoKeyW(dev, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &vals,
                             nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && subs == 0 && vals == 0)
        {
            NdmPfnNtDeleteKey del = NdmNtDeleteKey();
            if (del) del(dev);
        }
    }
    RegCloseKey(dev);
    return ok;
}
