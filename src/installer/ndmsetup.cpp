// ===========================================================================
// NDM Setup — Self-Extracting Installer Stub
//
// The build script (tools/make_sfx_payload.ps1) appends the payload in
// NDM-SFX v2 format (see ndm_sfx.h): NDMan.exe, NDMMsgHost.exe,
// NDMShellExt.dll, Uninstall.exe, browser extensions and the bundled
// third-party tools (tools\yt-dlp.exe, ffmpeg.exe, ffprobe.exe, ...).
//
// At runtime this stub:
//   1. Shows a compact dark-gold setup dialog.
//   2. Indexes the payload appended to itself.
//   3. Streams every file block by block (cabinet.dll LZMS / XPRESS_HUFF),
//      verifying each block CRC and the file CRC, into the install dir.
//   4. Registers shell extension, native messaging host, uninstall entry.
//   5. Creates desktop / Start Menu shortcuts and optionally starts NDMan.exe.
//
// Setup has no command-line switches. (The former test switches /extract-only
// and /preview were removed: an elevated binary must not write into a folder
// named on its command line.)
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <objidl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wingdi.h>
#include <string>
#include <vector>
#include <algorithm>
#include "ndm_sfx.h"
#include "sfx_extract.h"
#include <knownfolders.h>
#include "ndm_procs.h"
#include "ndm_fsutil.h"
#include "ndm_integration.h"   // publishing constants (store IDs, URLInfoAbout) live here
#include "ndm_userinfo.h"      // who uses NDM (recorded for the uninstaller)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "uxtheme.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define NDM_SETUP_MUTEX     "NDM_Setup_Mutex_k7pX3nW8"
#define NDM_VERSION_WIDE    L"1.0.5"
#define NDM_VERSION_MAJOR   1
#define NDM_VERSION_MINOR   0
#define NDM_PUBLISHER       L"Devnetra Consultancy"
#define NDM_TAGLINE         L"Invented with Care By Devnetra Consultancy"
#define NDM_EXE_NAME        L"NDMan.exe"
#define NDM_PRODUCT         L"Nee Download Manager"
#define NDM_UNINST_KEY      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Nee Download Manager"
#define NDM_APP_KEY         L"Software\\Devnetra\\NDM"
#define IDI_NDM_APP         1      // ndmsetup.rc

// Control IDs
#define IDC_PROGRESS        101
#define IDC_STATUS_LABEL    102
#define IDC_PATH_EDIT       103
#define IDC_PATH_BROWSE     104
#define IDC_INSTALL_BTN     105
#define IDC_CANCEL_BTN      106
#define IDC_DESKTOP_CHK     107
#define IDC_LAUNCH_CHK      108

// Messages
#define WM_SETUP_DONE       (WM_APP + 51)
#define WM_SETUP_ERROR      (WM_APP + 52)

// ---------------------------------------------------------------------------
// Theme colors (matches ndm_common.h)
// ---------------------------------------------------------------------------
#define T_BG_PANEL      RGB( 26,  29,  35)
#define T_BG_ALT        RGB( 32,  36,  44)
#define T_FG            RGB(220, 220, 225)
#define T_FG_DIM        RGB(140, 142, 150)
#define T_GOLD          RGB(212, 175,  55)
#define T_GOLD_LIGHT    RGB(240, 210, 100)
#define T_GOLD_DARK     RGB(160, 128,  30)
#define T_BORDER        RGB( 55,  60,  72)
#define T_BORDER_GOLD   RGB(140, 110,  28)
#define T_EDIT_BG       RGB( 30,  33,  40)
#define T_GREEN         RGB( 70, 200, 120)
static const COLORREF T_FOOTER = RGB(20, 22, 27);
static const COLORREF T_BODY   = RGB(24, 27, 33);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
enum SetupState { ST_READY = 0, ST_INSTALLING = 1, ST_FINISHED = 2 };

static HWND          g_hDlg        = nullptr;
static volatile LONG g_cancelled   = 0;
static SetupState    g_state       = ST_READY;
static std::wstring  g_launchExe;           // set when finished

static HBRUSH g_hbrBody   = nullptr;
static HBRUSH g_hbrEdit   = nullptr;
static HBRUSH g_hbrFooter = nullptr;
static HFONT  g_hfUI      = nullptr;
static HFONT  g_hfBold    = nullptr;
static HFONT  g_hfTitle   = nullptr;
static HICON  g_hIconBig  = nullptr;
static HICON  g_hIconSm   = nullptr;
static HICON  g_hIconLogo = nullptr;
static int    g_dpi       = 96;
static RECT   g_rcField   = {};
static RECT   g_rcFooter  = {};

static int SS(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

// Known folder path ("" on failure). Never taken from environment variables.
static std::wstring KnownFolderPath(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) s = p;
    if (p) CoTaskMemFree(p);
    return NdmStripSlash(s);
}

// REG_SZ value of any length; view = 0 (native 64-bit), KEY_WOW64_32KEY, ...
static std::wstring ReadRegStr(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view = 0)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) return {};
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

// ---------------------------------------------------------------------------
// Earlier x86 builds: installed to "Program Files (x86)\Devnetra\NDM" and
// registered in the 32-bit registry view (WOW6432Node).
// ---------------------------------------------------------------------------
static std::wstring FindLegacyX86InstallDir()
{
    std::wstring d = ReadRegStr(HKEY_LOCAL_MACHINE, NDM_APP_KEY, L"InstallDir", KEY_WOW64_32KEY);
    if (d.empty()) d = ReadRegStr(HKEY_LOCAL_MACHINE, NDM_APP_KEY, L"InstallPath", KEY_WOW64_32KEY);
    if (d.empty()) d = ReadRegStr(HKEY_LOCAL_MACHINE, NDM_UNINST_KEY, L"InstallLocation", KEY_WOW64_32KEY);
    return d.empty() ? d : NdmStripSlash(d);
}

static bool HasLegacyX86Registration()
{
    return NdmRegKeyExists(HKEY_LOCAL_MACHINE, NDM_APP_KEY, KEY_WOW64_32KEY) ||
           NdmRegKeyExists(HKEY_LOCAL_MACHINE, NDM_UNINST_KEY, KEY_WOW64_32KEY);
}

static std::wstring GetDefaultInstallDir()
{
    // Upgrade in place when the x64 build is already installed.
    std::wstring prev = ReadRegStr(HKEY_LOCAL_MACHINE, NDM_APP_KEY, L"InstallDir");
    if (!prev.empty() && PathIsDirectoryW(prev.c_str())) return NdmStripSlash(prev);
    // An x86 install in a custom folder keeps its folder; the default x86
    // location (Program Files (x86)) moves to Program Files.
    std::wstring legacy = FindLegacyX86InstallDir();
    std::wstring pf86 = KnownFolderPath(FOLDERID_ProgramFilesX86);
    if (!legacy.empty() && PathIsDirectoryW(legacy.c_str()) &&
        !(pf86.size() && NdmPathIsUnder(legacy, pf86)))
        return legacy;
    std::wstring pf = KnownFolderPath(FOLDERID_ProgramFiles);
    if (pf.empty()) pf = L"C:\\Program Files";
    return pf + L"\\Devnetra\\NDM";
}

static bool EnsureDir(const std::wstring& path, int* errOut = nullptr)
{
    int ret = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (errOut) *errOut = ret;
    return ret == ERROR_SUCCESS || ret == ERROR_ALREADY_EXISTS || ret == ERROR_FILE_EXISTS;
}

static std::wstring ErrorText(DWORD code) { return SfxErrorText(code); }

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

static std::wstring SelfPath()
{
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
    p.resize(n);
    return p;
}

static std::wstring DirOfPath(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

// ---------------------------------------------------------------------------
// Start a program as the normal (non-elevated) user. Setup runs elevated; the
// app must not inherit that token. Asks the desktop's Explorer to launch it
// (IShellWindows -> desktop view -> IShellDispatch2::ShellExecute).
// If Explorer isn't available the app is simply not started.
// ---------------------------------------------------------------------------
#include <exdisp.h>
#include <shldisp.h>
#pragma comment(lib, "oleaut32.lib")

static bool LaunchUnelevated(const std::wstring& exe, const std::wstring& args)
{
    bool ok = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* sw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                   IID_PPV_ARGS(&sw))))
    {
        VARIANT vEmpty = {}; VariantInit(&vEmpty);
        long hwnd = 0;
        IDispatch* disp = nullptr;
        if (sw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp)
        {
            IServiceProvider* sp = nullptr;
            IShellBrowser* sb = nullptr;
            IShellView* sv = nullptr;
            IDispatch* bg = nullptr;
            IShellFolderViewDual* fv = nullptr;
            IDispatch* app = nullptr;
            IShellDispatch2* sd = nullptr;
            if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) &&
                SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
                SUCCEEDED(sb->QueryActiveShellView(&sv)) &&
                SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg))) &&
                SUCCEEDED(bg->QueryInterface(IID_PPV_ARGS(&fv))) &&
                SUCCEEDED(fv->get_Application(&app)) &&
                SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&sd))))
            {
                BSTR bFile = SysAllocString(exe.c_str());
                VARIANT vArgs, vDir, vOp, vShow;
                VariantInit(&vArgs); vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str());
                VariantInit(&vDir);  vDir.vt  = VT_BSTR; vDir.bstrVal  = SysAllocString(DirOfPath(exe).c_str());
                VariantInit(&vOp);   vOp.vt   = VT_BSTR; vOp.bstrVal   = SysAllocString(L"open");
                VariantInit(&vShow); vShow.vt = VT_I4;   vShow.lVal    = SW_SHOWNORMAL;
                ok = SUCCEEDED(sd->ShellExecute(bFile, vArgs, vDir, vOp, vShow));
                SysFreeString(bFile);
                VariantClear(&vArgs); VariantClear(&vDir); VariantClear(&vOp);
            }
            if (sd) sd->Release();
            if (app) app->Release();
            if (fv) fv->Release();
            if (bg) bg->Release();
            if (sv) sv->Release();
            if (sb) sb->Release();
            if (sp) sp->Release();
            disp->Release();
        }
        sw->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

static std::wstring FormatSize(UINT64 b)
{
    wchar_t buf[64];
    if (b >= 1024ull * 1024 * 1024) swprintf_s(buf, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024)   swprintf_s(buf, L"%.1f MB", b / (1024.0 * 1024));
    else                            swprintf_s(buf, L"%.1f KB", b / 1024.0);
    return buf;
}

// Status/progress from the install thread (synchronous, cross-thread safe).
static void SetProgress(HWND hDlg, int pct, const wchar_t* msg)
{
    if (!hDlg) return;
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, pct, 0);
    if (msg) SetDlgItemTextW(hDlg, IDC_STATUS_LABEL, msg);
}

// ---------------------------------------------------------------------------
// Install folder: pinned handle + secure payload creation
//
//  - A new install folder is created atomically with its protected security
//    descriptor (CreateDirectoryW + SECURITY_ATTRIBUTES); every payload
//    subfolder is created the same way.
//  - An existing folder is first locked down on the pinned handle
//    (SetKernelObjectSecurity: no tree walk), then swept with no-follow
//    handles: every reparse point, every object not owned by Administrators /
//    SYSTEM / TrustedInstaller and every hard-linked file is renamed aside and
//    deleted. Only then is the descriptor propagated to the remaining tree.
//  - Payload files: whatever is at the name is renamed aside and deleted
//    (never followed), the file is created with CREATE_NEW |
//    FILE_FLAG_OPEN_REPARSE_POINT, its final path must be inside the pinned
//    folder, and owner (Administrators) / DACL (inherited only) are reset on
//    the handle. A locked old file (e.g. the shell extension loaded by
//    Explorer) is renamed aside and deleted at reboot.
// ---------------------------------------------------------------------------
struct DirHandle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    DirHandle() = default;
    DirHandle(const DirHandle&) = delete;
    DirHandle& operator=(const DirHandle&) = delete;
    ~DirHandle() { Close(); }
    void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
};

struct InstallTarget
{
    std::wstring dir;            // validated, drive-absolute, no trailing slash
    std::wstring finalDir;       // GetFinalPathNameByHandle of the pinned folder
    DirHandle    hold;           // pinned (no FILE_SHARE_DELETE) for the whole install
    std::wstring sddl;           // descriptor for the folder and its subfolders
};

static bool PathIsGone(const std::wstring& p)
{
    if (GetFileAttributesW(NdmLongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

// Remove whatever is at `path` inside the (locked) install folder without
// following it: rename it aside first (MoveFileEx renames a link itself), so
// the name is free at once and a delete-at-reboot for an in-use object is
// recorded under the aside name, never under `path` (which is about to be
// re-created). The folder is locked, so the aside object cannot be moved back.
static bool RemoveFromInstallDir(const std::wstring& path)
{
    if (PathIsGone(path)) return true;
    std::wstring aside = path + L".old" + std::to_wstring(GetTickCount()) + NdmRandomHex(4);
    if (!MoveFileExW(NdmLongPath(path).c_str(), NdmLongPath(aside).c_str(), 0)) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    bool reboot = false;
    NdmDeleteNoFollow(aside, reboot);   // in use: scheduled for reboot (aside name)
    return true;
}

// Sweep the children of `path` (a trusted, non-link folder): reparse points,
// objects not owned by Administrators / SYSTEM / TrustedInstaller, hard-linked
// files and anything that cannot be inspected are removed; trusted folders are
// descended into. False if something untrusted could not be removed.
static bool SweepInstallTree(const std::wstring& path, int depth = 0)
{
    if (depth > 32) return false;
    HANDLE hd = NdmOpenNoFollow(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (hd == INVALID_HANDLE_VALUE) return false;
    DWORD da = 0;
    std::vector<std::wstring> names;
    if (!NdmHandleAttrs(hd, da) || !(da & FILE_ATTRIBUTE_DIRECTORY) || (da & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !NdmListDir(hd, names))
    {
        CloseHandle(hd);
        return false;
    }
    bool ok = true;
    for (const auto& n : names) {
        std::wstring child = path + L"\\" + n;
        bool keep = false, isDir = false;
        HANDLE h = NdmOpenNoFollow(child, READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD a = 0;
            FILE_STANDARD_INFO si = {};
            if (NdmHandleAttrs(h, a) && !(a & FILE_ATTRIBUTE_REPARSE_POINT) && NdmHandleOwnerIsTrusted(h) &&
                GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si)))
            {
                isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                keep  = isDir || si.NumberOfLinks <= 1;
            }
            CloseHandle(h);
        }
        if (keep) {
            if (isDir && !SweepInstallTree(child, depth + 1)) ok = false;
        } else if (!RemoveFromInstallDir(child)) {
            ok = false;
        }
    }
    CloseHandle(hd);
    return ok;
}

static bool DirIsEmpty(HANDLE h)
{
    std::vector<std::wstring> names;
    return NdmListDir(h, names) && names.empty();
}

// Create (atomically protected) or take over + sanitise t.dir and pin it.
static bool SecureInstallDir(InstallTarget& t, std::wstring& err)
{
    if (t.sddl.empty()) t.sddl = NDM_SDDL_INSTALL_DIR;
    NdmSecurityDescriptor sd(t.sddl.c_str());
    if (!sd.sd || !sd.Dacl() || !sd.Owner()) {
        err = L"Cannot build the security descriptor of the installation folder.";
        return false;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.sd, FALSE };
    const std::wstring& dir = t.dir;
    const DWORD ownerInfo = OWNER_SECURITY_INFORMATION;

    for (int attempt = 0; attempt < 2; attempt++) {
        std::wstring parent = DirOfPath(dir);
        if (parent.size() > 3 && !PathIsDirectoryW(parent.c_str())) {
            int pe = SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
            if (pe != ERROR_SUCCESS && pe != ERROR_ALREADY_EXISTS && pe != ERROR_FILE_EXISTS) {
                err = L"Cannot create folder:\n" + parent + L"\n\n" + ErrorText((DWORD)pe) +
                      (IsElevated() ? L"" : L"\n\nSetup is not running as administrator.");
                return false;
            }
        }
        if (NdmPathHasReparsePoint(dir)) {
            err = L"The installation folder path goes through a link (junction, symbolic link or mount point) "
                  L"or a file is in the way:\n" + dir + L"\n\nPlease choose another folder.";
            return false;
        }
        bool created = CreateDirectoryW(NdmLongPath(dir).c_str(), &sa) != FALSE;
        if (!created && GetLastError() != ERROR_ALREADY_EXISTS) {
            DWORD le = GetLastError();
            err = L"Cannot create installation folder:\n" + dir + L"\n\n" + ErrorText(le) +
                  (IsElevated() ? L"" : L"\n\nSetup is not running as administrator.");
            return false;
        }

        // Pin it without FILE_SHARE_DELETE: it (and its parents) can no longer
        // be renamed or replaced by a link until setup finishes.
        t.hold.Close();
        t.hold.h = NdmOpenNoFollow(dir, READ_CONTROL | WRITE_DAC | WRITE_OWNER |
                                        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (t.hold.h == INVALID_HANDLE_VALUE) {
            err = L"Cannot open the installation folder:\n" + dir + L"\n\n" + ErrorText(GetLastError());
            return false;
        }
        DWORD ha = 0;
        t.finalDir = NdmFinalPath(t.hold.h);
        if (!NdmHandleAttrs(t.hold.h, ha) || !(ha & FILE_ATTRIBUTE_DIRECTORY) || (ha & FILE_ATTRIBUTE_REPARSE_POINT) ||
            t.finalDir.empty() || !NdmSamePath(t.finalDir, NdmLongName(dir)) || NdmPathHasReparsePoint(dir))
        {
            t.hold.Close();
            err = L"The installation folder changed while setup was preparing it, or it is reached through a link:\n" +
                  dir + L"\n\nPlease choose another folder.";
            return false;
        }
        if (created) return true;   // carries the protected descriptor from birth, nothing inside

        // ---- existing folder ----
        if (!NdmHandleOwnerIsTrusted(t.hold.h)) {
            // An empty folder someone else created is replaced by a fresh one.
            bool empty = DirIsEmpty(t.hold.h);
            t.hold.Close();
            if (empty && attempt == 0 && RemoveDirectoryW(NdmLongPath(dir).c_str())) continue;
            err = L"The installation folder already exists and is not owned by an administrator:\n" + dir +
                  L"\n\nPlease choose another folder or remove this one.";
            return false;
        }
        // 1. Lock it down first (raw set on the handle: no propagation, so no
        //    walk through whatever is planted inside).
        if (!SetKernelObjectSecurity(t.hold.h, ownerInfo | DACL_SECURITY_INFORMATION, sd.sd)) {
            DWORD le = GetLastError();
            t.hold.Close();
            err = L"Cannot set the permissions of the installation folder:\n" + dir + L"\n\n" + ErrorText(le);
            return false;
        }
        // 2. Remove links / foreign-owned objects / hard links planted inside.
        if (!SweepInstallTree(dir)) {
            t.hold.Close();
            err = L"The installation folder contains links or files not owned by an administrator that "
                  L"cannot be removed (in use?):\n" + dir + L"\n\nPlease close all programs and try again, "
                  L"or choose another folder.";
            return false;
        }
        // 3. The tree is clean now: propagate the descriptor to it.
        DWORD se = SetSecurityInfo(t.hold.h, SE_FILE_OBJECT,
                                   ownerInfo | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   sd.Owner(), nullptr, sd.Dacl(), nullptr);
        if (se != ERROR_SUCCESS) {
            t.hold.Close();
            err = L"Cannot set the permissions of the installation folder:\n" + dir + L"\n\n" + ErrorText(se);
            return false;
        }
        return true;
    }
    err = L"Cannot create installation folder:\n" + dir;
    return false;
}

// Create every folder of `relDir` (relative to the pinned folder) with the
// protected descriptor; existing ones must be real, (trusted) folders whose
// final path is the expected one inside the pinned folder.
static bool EnsurePayloadDir(const InstallTarget& t, const std::wstring& relDir, std::wstring& err)
{
    NdmSecurityDescriptor sd(t.sddl.c_str());
    if (!sd.sd) { err = L"Cannot build the security descriptor of " + relDir; return false; }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.sd, FALSE };
    std::wstring rel;
    size_t pos = 0;
    while (pos <= relDir.size()) {
        size_t e = relDir.find(L'\\', pos);
        if (e == std::wstring::npos) e = relDir.size();
        std::wstring comp = relDir.substr(pos, e - pos);
        pos = e + 1;
        if (comp.empty() || comp == L"." || comp == L"..") { err = L"Bad payload path: " + relDir; return false; }
        rel = rel.empty() ? comp : rel + L"\\" + comp;
        std::wstring full = t.dir + L"\\" + rel;
        if (!CreateDirectoryW(NdmLongPath(full).c_str(), &sa)) {
            DWORD le = GetLastError();
            if (le != ERROR_ALREADY_EXISTS) { err = L"Cannot create folder " + rel + L"\n\n" + ErrorText(le); return false; }
        }
        HANDLE h = NdmOpenNoFollow(full, READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (h == INVALID_HANDLE_VALUE) { err = L"Cannot open folder " + rel + L"\n\n" + ErrorText(GetLastError()); return false; }
        DWORD a = 0;
        bool good = NdmHandleAttrs(h, a) && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    NdmSamePath(NdmFinalPath(h), t.finalDir + L"\\" + rel) &&
                    NdmHandleOwnerIsTrusted(h);
        CloseHandle(h);
        if (!good) { err = L"The folder " + rel + L" is a link or is not owned by an administrator."; return false; }
    }
    return true;
}

// Create the payload file t.dir\rel for writing (see the block comment above).
static HANDLE CreatePayloadFile(const InstallTarget& t, const std::wstring& rel, std::wstring& err)
{
    size_t slash = rel.rfind(L'\\');
    if (slash != std::wstring::npos && !EnsurePayloadDir(t, rel.substr(0, slash), err))
        return INVALID_HANDLE_VALUE;
    std::wstring full = t.dir + L"\\" + rel;
    if (!RemoveFromInstallDir(full)) {
        DWORD le = GetLastError();
        err = L"Cannot replace " + rel + L" (in use?)\n\n" + ErrorText(le);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(NdmLongPath(full).c_str(),
                           GENERIC_WRITE | READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD le = GetLastError();
        err = L"Failed to create " + rel + L"\n\n" + ErrorText(le);
        return INVALID_HANDLE_VALUE;
    }
    DWORD a = 0;
    if (!NdmHandleAttrs(h, a) || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !NdmSamePath(NdmFinalPath(h), t.finalDir + L"\\" + rel))
    {
        NdmMarkForDelete(h);
        CloseHandle(h);
        err = L"Refusing to write " + rel + L": it resolves outside the installation folder.";
        return INVALID_HANDLE_VALUE;
    }
    // Owner Administrators (elevated install); DACL = inherited from the folder only.
    ACL empty = {};
    BYTE baBuf[SECURITY_MAX_SID_SIZE] = {};
    DWORD baLen = sizeof(baBuf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, baBuf, &baLen)) {
        NdmMarkForDelete(h); CloseHandle(h);
        err = L"Cannot set the owner of " + rel;
        return INVALID_HANDLE_VALUE;
    }
    PSID ba = baBuf;
    InitializeAcl(&empty, sizeof(empty), ACL_REVISION);
    DWORD se = SetSecurityInfo(h, SE_FILE_OBJECT,
                               OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                               UNPROTECTED_DACL_SECURITY_INFORMATION,
                               ba, nullptr, &empty, nullptr);
    if (se != ERROR_SUCCESS) {
        NdmMarkForDelete(h);
        CloseHandle(h);
        err = L"Cannot set the permissions of " + rel + L"\n\n" + ErrorText(se);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

// Extract one entry into the pinned folder; on failure the partial file is removed.
static bool ExtractEntryToDir(SfxReader& rd, const SfxIndexEntry& e, const InstallTarget& t,
                              SfxProgressFn cb, void* ctx, std::wstring& err)
{
    std::wstring cerr;
    HANDLE h = CreatePayloadFile(t, e.relPath, cerr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Failed to write: " + e.relPath + L"\n\n" + cerr;
        return false;
    }
    bool ok = rd.Extract(e, h, cb, ctx, err);
    if (ok && !FlushFileBuffers(h)) {
        DWORD le = GetLastError();
        err = L"Failed to write: " + e.relPath + L"\n\n" + ErrorText(le);
        ok = false;
    }
    if (!ok) NdmMarkForDelete(h);
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Registry / shortcut helpers
// ---------------------------------------------------------------------------
static void RegWriteStr(HKEY hKey, const wchar_t* name, const std::wstring& val)
{
    RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                   (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

static void RegWriteDword(HKEY hKey, const wchar_t* name, DWORD val)
{
    RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
}

static bool CreateShortcut(const std::wstring& lnk, const std::wstring& target,
                           const std::wstring& workDir, const std::wstring& desc)
{
    bool ok = false;
    IShellLinkW* pLink = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&pLink))))
    {
        pLink->SetPath(target.c_str());
        pLink->SetWorkingDirectory(workDir.c_str());
        pLink->SetDescription(desc.c_str());
        pLink->SetIconLocation(target.c_str(), 0);
        IPersistFile* pFile = nullptr;
        if (SUCCEEDED(pLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pFile)))) {
            ok = SUCCEEDED(pFile->Save(lnk.c_str(), TRUE));
            pFile->Release();
        }
        pLink->Release();
    }
    return ok;
}

static std::wstring KnownFolder(int csidl)
{
    wchar_t buf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf);
    return buf;
}

// ---------------------------------------------------------------------------
// Native messaging host (browser extension <-> NDMMsgHost.exe).
// Keep these key names in sync with Uninstall.cpp.
// ---------------------------------------------------------------------------
// Chrome/Edge extension ID, fixed by the "key" in
// src/browser_ext/chrome/manifest.json (private key: keys\ndm_ext_private.pem,
// never shipped). Firefox add-on ID: browser_specific_settings.gecko.id.
// The IDs themselves (unpacked keyed ID + optional store IDs) live in
// ndm_integration.h together with the other publishing constants.

static const wchar_t* const kNmChromiumKeys[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.devnetra.ndm",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.devnetra.ndm",
    L"Software\\Chromium\\NativeMessagingHosts\\com.devnetra.ndm",
};
static const wchar_t* const kNmFirefoxKey =
    L"Software\\Mozilla\\NativeMessagingHosts\\com.devnetra.ndm";

static std::string JsonEscapePath(const std::wstring& w)
{
    std::string out;
    for (char c : WideToUtf8(w)) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// rel: file name inside the pinned install folder.
static bool WriteTextFile(const InstallTarget& t, const std::wstring& rel, const std::string& text)
{
    std::wstring err;
    HANDLE h = CreatePayloadFile(t, rel, err);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wr, nullptr) &&
              wr == (DWORD)text.size() && FlushFileBuffers(h);
    DWORD le = ok ? 0 : GetLastError();
    if (!ok) NdmMarkForDelete(h);
    CloseHandle(h);
    if (!ok) SetLastError(le ? le : ERROR_WRITE_FAULT);
    return ok != FALSE;
}

#define NDM_CHROME_MANIFEST  L"NDMMsgHost_chrome.json"
#define NDM_FIREFOX_MANIFEST L"NDMMsgHost_firefox.json"

static bool WriteNativeHostManifests(const InstallTarget& t)
{
    const std::wstring& dir = t.dir;
    std::string exe = JsonEscapePath(dir + L"\\NDMMsgHost.exe");
    std::string head =
        "{\r\n"
        "  \"name\": \"com.devnetra.ndm\",\r\n"
        "  \"description\": \"Nee Download Manager Native Messaging Host\",\r\n"
        "  \"path\": \"" + exe + "\",\r\n"
        "  \"type\": \"stdio\",\r\n";
    std::string origins;
    for (const std::string& id : NdmChromeExtensionIds()) {
        if (!origins.empty()) origins += ",\r\n";
        origins += "    \"chrome-extension://" + id + "/\"";
    }
    bool a = WriteTextFile(t, NDM_CHROME_MANIFEST, head +
        "  \"allowed_origins\": [\r\n" + origins + "\r\n"
        "  ]\r\n"
        "}\r\n");
    bool b = WriteTextFile(t, NDM_FIREFOX_MANIFEST, head +
        "  \"allowed_extensions\": [\r\n"
        "    \"" NDM_FIREFOX_ADDON_ID "\"\r\n"
        "  ]\r\n"
        "}\r\n");
    return a && b;
}

static void RegisterNativeHostKey(const wchar_t* subKey, const std::wstring& manifest)
{
    const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM view : views) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr, 0, KEY_WRITE | view,
                            nullptr, &hKey, nullptr) == ERROR_SUCCESS)
        {
            RegWriteStr(hKey, nullptr, manifest);
            RegCloseKey(hKey);
        }
    }
}

// ---------------------------------------------------------------------------
// Install folder validation + protection
//
//  - local drive-absolute paths only; "\NDM" is appended unless the folder
//    is already called NDM
//  - never a system folder (Windows and below, System32, Program Files
//    roots, drive root, ProgramData root, user profile roots)
//  - no component of the path may be a reparse point (junction / symlink /
//    mount point), re-checked through the opened handle after creation
//  - an existing non-empty folder must be owned by Administrators, SYSTEM
//    or TrustedInstaller
//  - the folder gets a protected DACL (SY/BA full, BU read+execute, owner
//    Administrators) BEFORE any file is extracted into it
//  - the folder stays open (no FILE_SHARE_DELETE) for the whole install,
//    so neither it nor any parent can be renamed / swapped meanwhile
// ---------------------------------------------------------------------------
static bool IsProtectedSystemDir(const std::wstring& dir)
{
    if (dir.size() <= 3) return true;   // drive root
    std::wstring win = KnownFolderPath(FOLDERID_Windows);
    if (!win.empty() && (NdmSamePath(dir, win) || NdmPathIsUnder(dir, win))) return true;
    const KNOWNFOLDERID* ids[] = {
        &FOLDERID_System, &FOLDERID_SystemX86, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
        &FOLDERID_ProgramFilesCommon, &FOLDERID_ProgramFilesCommonX86, &FOLDERID_ProgramData,
        &FOLDERID_Profile, &FOLDERID_UserProfiles, &FOLDERID_Public,
    };
    for (const KNOWNFOLDERID* id : ids) {
        std::wstring k = KnownFolderPath(*id);
        if (!k.empty() && NdmSamePath(dir, k)) return true;
    }
    return false;
}

// Normalises `dir` (in/out) and prepares it; `hold` keeps it open.
static bool PrepareInstallDir(std::wstring& dir, InstallTarget& t, std::wstring& err)
{
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/' || dir.back() == L' '))
        dir.pop_back();
    while (!dir.empty() && dir.front() == L' ') dir.erase(dir.begin());
    if (dir.empty() || PathIsRelativeW(dir.c_str())) {
        err = L"Please choose a full installation folder path (for example C:\\Program Files\\NDM).";
        return false;
    }
    {   // canonical form: resolves "..", ".", forward slashes
        wchar_t full[MAX_PATH] = {};
        DWORD n = GetFullPathNameW(dir.c_str(), MAX_PATH, full, nullptr);
        if (n == 0 || n >= MAX_PATH) { err = L"The installation folder path is too long."; return false; }
        dir = NdmStripSlash(full);
    }
    if (dir.size() < 3 || dir[1] != L':' || dir[2] != L'\\' || !iswalpha(dir[0])) {
        err = L"Please choose a folder on a local drive (for example C:\\Program Files\\NDM).\n\n" + dir;
        return false;
    }
    if (dir.find_first_of(L"*?\"<>|", 0) != std::wstring::npos || dir.find(L':', 2) != std::wstring::npos) {
        err = L"The installation folder path contains invalid characters:\n" + dir;
        return false;
    }
    if (_wcsicmp(PathFindFileNameW(dir.c_str()), L"NDM") != 0) {
        if (dir.back() != L'\\') dir += L'\\';
        dir += L"NDM";
    }
    if (dir.size() > MAX_PATH - 64) { err = L"The installation folder path is too long."; return false; }
    if (IsProtectedSystemDir(dir)) {
        err = L"NDM cannot be installed into this system folder:\n" + dir +
              L"\n\nPlease choose another folder (for example C:\\Program Files\\NDM).";
        return false;
    }
    if (NdmPathHasReparsePoint(dir)) {
        err = L"The installation folder path goes through a link (junction, symbolic link or mount point) "
              L"or a file is in the way:\n" + dir + L"\n\nPlease choose another folder.";
        return false;
    }

    t.dir = dir;
    return SecureInstallDir(t, err);
}

// ---------------------------------------------------------------------------
// Upgrade from an x86 build: stop it, drop its 32-bit COM registration and
// 32-bit-view keys, and delete its files when the new folder is different.
// (The 32-bit shell extension's keys are removed directly instead of running
// SysWOW64\regsvr32 /u: no elevated process loads the old DLL.)
// ---------------------------------------------------------------------------
static void RemoveOldInstallFolder(const std::wstring& old);

static void CleanupLegacyX86Install(const std::wstring& newDir)
{
    std::wstring old = FindLegacyX86InstallDir();
    if (old.empty() && !HasLegacyX86Registration()) return;

    if (!old.empty() && old.size() > 3) NdmTerminateProcesses(old, {});

    NdmUnregisterShellExt(KEY_WOW64_32KEY);
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, NDM_UNINST_KEY, KEY_WOW64_32KEY);
    NdmRegDeleteTree(HKEY_LOCAL_MACHINE, NDM_APP_KEY, KEY_WOW64_32KEY);
    {
        HKEY h = nullptr;   // empty 32-bit "Software\Devnetra"
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Devnetra", 0, KEY_READ | KEY_WOW64_32KEY, &h) == ERROR_SUCCESS) {
            DWORD sub = 0, vals = 0;
            RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &sub, nullptr, nullptr, &vals, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(h);
            if (sub == 0 && vals == 0) NdmRegDeleteTree(HKEY_LOCAL_MACHINE, L"Software\\Devnetra", KEY_WOW64_32KEY);
        }
    }

    if (old.empty() || NdmSamePath(old, newDir)) return;
    RemoveOldInstallFolder(old);
}

// ---------------------------------------------------------------------------
// Upgrade from an x64 build installed in a DIFFERENT folder: its folder is
// recorded in the uninstall key (InstallLocation) and the app key
// (InstallDir) that this install is about to overwrite. Called after the new
// files are in place and before the new registration is written, so a
// failed extraction never leaves the user without a working NDM. Its shell
// extension / native messaging / association keys are rewritten to the new
// folder by the registration that follows.
// ---------------------------------------------------------------------------
static void CleanupPreviousX64Install(const std::wstring& newDir)
{
    const std::wstring recorded[] = {
        ReadRegStr(HKEY_LOCAL_MACHINE, NDM_UNINST_KEY, L"InstallLocation"),
        ReadRegStr(HKEY_LOCAL_MACHINE, NDM_APP_KEY, L"InstallDir"),
    };
    std::vector<std::wstring> done;
    for (const std::wstring& r : recorded) {
        std::wstring old = NdmPickPreviousInstallDir({ r }, newDir);
        if (old.empty()) continue;
        bool seen = false;
        for (const auto& d : done) seen = seen || NdmSamePath(d, old);
        if (seen) continue;
        done.push_back(old);
        RemoveOldInstallFolder(old);
    }
}

// Delete what an older NDM install put into `old` (a folder other than the
// new one), using the same checks and no-follow deletion as the uninstaller:
// local drive-absolute, not a system folder, no link anywhere in the path,
// looks like an NDM install, pinned while deleting; user files stay.
static void RemoveOldInstallFolder(const std::wstring& old)
{
    if (!PathIsDirectoryW(old.c_str())) return;
    if (old.size() < 4 || old[1] != L':' || old[2] != L'\\' || IsProtectedSystemDir(old) ||
        NdmPathHasReparsePoint(old) || !NdmLooksLikeInstall(old))
        return;
    NdmTerminateProcesses(old, {});   // e.g. NDMMsgHost.exe started by a browser
    DirHandle hold;   // keep the old folder pinned while deleting inside it
    hold.h = NdmOpenNoFollow(old, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE);
    DWORD ha = 0;
    if (hold.h == INVALID_HANDLE_VALUE || !NdmHandleAttrs(hold.h, ha) || (ha & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !NdmSamePath(NdmFinalPath(hold.h), NdmLongName(old)))
        return;
    bool reboot = false;
    NdmRemoveInstalledFiles(old, reboot);
    hold.Close();
    if (!RemoveDirectoryW(old.c_str()) && reboot)
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    std::wstring parent = DirOfPath(old);
    if (_wcsicmp(PathFindFileNameW(parent.c_str()), L"Devnetra") == 0) RemoveDirectoryW(parent.c_str());
}

// ---------------------------------------------------------------------------
// Install thread
// ---------------------------------------------------------------------------
struct InstallParams
{
    HWND         hDlg;
    std::wstring installDir;
    bool         desktop;
    bool         launch;
};

struct ExtractProgress
{
    HWND   hDlg;
    UINT64 baseBytes;     // bytes of all previous entries
    UINT64 totalBytes;
    int    lastPct;       // overall bar position last sent
    int    lastFilePct;   // per-file percentage last shown
};

static const int kPctExtractBegin = 8;
static const int kPctExtractEnd   = 88;

static bool ExtractProgressCb(void* ctx, const SfxIndexEntry& e, UINT64 done)
{
    ExtractProgress* p = static_cast<ExtractProgress*>(ctx);
    if (g_cancelled) return false;
    int filePct = e.origSize ? (int)(done * 100 / e.origSize) : 100;
    UINT64 all = p->baseBytes + done;
    int pct = kPctExtractBegin +
              (int)((p->totalBytes ? (double)all / p->totalBytes : 1.0) * (kPctExtractEnd - kPctExtractBegin));
    if (filePct != p->lastFilePct || pct != p->lastPct) {
        p->lastFilePct = filePct;
        p->lastPct     = pct;
        std::wstring msg = L"Extracting " + e.relPath + L"  " + std::to_wstring(filePct) + L"%";
        SetProgress(p->hDlg, pct, msg.c_str());
    }
    return true;
}

static DWORD WINAPI InstallThread(LPVOID param)
{
    InstallParams* p = reinterpret_cast<InstallParams*>(param);
    HWND  hDlg      = p->hDlg;
    std::wstring dir = p->installDir;

    // Message is heap-allocated; the dialog proc deletes it.
    auto fail = [&](const std::wstring& msg) {
        PostMessageW(hDlg, WM_SETUP_ERROR, 0, reinterpret_cast<LPARAM>(new std::wstring(msg)));
        delete p;
        return (DWORD)1;
    };
    auto cancelled = [&]() {
        PostMessageW(hDlg, WM_SETUP_ERROR, 1, 0);   // wParam 1 = user cancel
        delete p;
        return (DWORD)2;
    };

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 1. Index + verify (SHA-256) the SFX payload ---------------------
    SetProgress(hDlg, 2, L"Verifying setup data...");
    SfxReader rd;
    std::wstring err;
    if (!rd.Open(SelfPath(), err)) return fail(err);
    if (rd.entries.empty()) return fail(L"Setup data is empty. Please re-download the installer.");

    // ---- 2. Validate, create and protect the install directory -----------
    SetProgress(hDlg, 4, L"Creating installation folder...");
    InstallTarget target;   // pinned folder, released when this thread returns
    if (!PrepareInstallDir(dir, target, err)) return fail(err);

    ULARGE_INTEGER freeBytes = {};
    if (GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, nullptr, nullptr) &&
        freeBytes.QuadPart < rd.totalOrig + 8ull * 1024 * 1024)
        return fail(L"Not enough free disk space in:\n" + dir + L"\n\nRequired: " +
                    FormatSize(rd.totalOrig) + L"\nAvailable: " + FormatSize(freeBytes.QuadPart));

    // ---- 3. Stop running NDM / bundled tools -----------------------------
    SetProgress(hDlg, 6, L"Closing running NDM...");
    NdmAskMainWindowToClose();
    NdmTerminateProcesses(dir, {});
    CleanupLegacyX86Install(dir);      // older x86 build (Program Files (x86), WOW6432Node)
    if (g_cancelled) return cancelled();

    // ---- 4. Extract all files --------------------------------------------
    ExtractProgress prog = { hDlg, 0, rd.totalOrig, -1, -1 };
    for (const auto& e : rd.entries)
    {
        if (g_cancelled) return cancelled();
        prog.lastFilePct = -1;
        ExtractProgressCb(&prog, e, 0);
        if (!ExtractEntryToDir(rd, e, target, ExtractProgressCb, &prog, err)) {
            if (rd.cancelled) return cancelled();
            return fail(err);
        }
        prog.baseBytes += e.origSize;
    }

    // ---- 4b. Previous x64 install in another folder: remove its files ----
    CleanupPreviousX64Install(dir);

    // ---- 5. Register shell extension -------------------------------------
    // The COM / context-menu keys are written directly (same keys as the
    // DLL's DllRegisterServer): no regsvr32, so no elevated process loads
    // NDMShellExt.dll. 64-bit Explorer reads the native 64-bit view.
    SetProgress(hDlg, 90, L"Registering shell extension...");
    if (!NdmRegisterShellExt(dir + L"\\NDMShellExt.dll"))
        return fail(L"Cannot register the Explorer context menu extension.");

    // ---- 6. Native messaging manifests + registration --------------------
    SetProgress(hDlg, 93, L"Registering browser integration...");
    {
        // Manifests live in the install dir and are registered for Chrome,
        // Edge, Chromium and Firefox in BOTH registry views (32-bit browser
        // builds read the 32-bit view, 64-bit ones the native view).
        std::wstring chromeJson = dir + L"\\" NDM_CHROME_MANIFEST;
        std::wstring ffJson     = dir + L"\\" NDM_FIREFOX_MANIFEST;
        if (!WriteNativeHostManifests(target)) {
            DWORD le = GetLastError();
            return fail(L"Failed to write the browser integration manifest.\n\n" + ErrorText(le));
        }
        // (The ProgramData manifest of very old builds is no longer cleaned up
        // here: nothing points at it once the keys below are rewritten, and
        // the uninstaller still removes it.)
        for (const wchar_t* key : kNmChromiumKeys) RegisterNativeHostKey(key, chromeJson);
        RegisterNativeHostKey(kNmFirefoxKey, ffJson);
    }

    // ---- 6b. magnet: / .torrent, store extensions, Firefox policy -------
    SetProgress(hDlg, 94, L"Registering magnet links and torrent files...");
    NdmRegisterAssociations(dir);
    // Browser extension auto-install, deliberately kept although it is a
    // no-op with the current constants (ndm_integration.h: empty store IDs,
    // FIREFOX_SIGNED_XPI_AVAILABLE = false): it is the prepared code that
    // switches on by setting those constants once the extensions are published.
    NdmRegisterStoreExtensions();             // no-op until store IDs are set
    NdmUpdateFirefoxPolicies(dir, true);      // no-op until a signed XPI ships

    // ---- 7. Uninstall entry + app key ------------------------------------
    SetProgress(hDlg, 95, L"Registering uninstaller...");
    {
        HKEY hKey = nullptr;
        LSTATUS ls = RegCreateKeyExW(HKEY_LOCAL_MACHINE, NDM_UNINST_KEY, 0, nullptr, 0, KEY_WRITE,
                                     nullptr, &hKey, nullptr);
        if (ls != ERROR_SUCCESS)
            return fail(L"Cannot register the uninstaller.\n\n" + ErrorText((DWORD)ls));
        UINT64 kb = (rd.totalOrig + 1023) / 1024 + 4;   // + generated manifests
        RegWriteStr(hKey, L"DisplayName",     NDM_PRODUCT);
        RegWriteStr(hKey, L"DisplayVersion",  NDM_VERSION_WIDE);
        RegWriteStr(hKey, L"Publisher",       NDM_PUBLISHER);
        RegWriteStr(hKey, L"InstallLocation", dir);
        RegWriteStr(hKey, L"DisplayIcon",     dir + L"\\" NDM_EXE_NAME L",0");
        RegWriteStr(hKey, L"UninstallString", L"\"" + dir + L"\\Uninstall.exe\"");
        RegWriteStr(hKey, L"QuietUninstallString", L"\"" + dir + L"\\Uninstall.exe\" /S");
        RegWriteStr(hKey, L"Comments",        NDM_TAGLINE);
        {   // "Installed On" column in Programs and Features / Revo
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t d[16]; swprintf_s(d, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
            RegWriteStr(hKey, L"InstallDate", d);
        }
        RegWriteDword(hKey, L"EstimatedSize", (DWORD)(kb > 0xFFFFFFFFull ? 0xFFFFFFFFull : kb));
        RegWriteDword(hKey, L"VersionMajor",  NDM_VERSION_MAJOR);
        RegWriteDword(hKey, L"VersionMinor",  NDM_VERSION_MINOR);
        if (NDM_URL_INFO_ABOUT[0]) RegWriteStr(hKey, L"URLInfoAbout", NDM_URL_INFO_ABOUT);
        else RegDeleteValueW(hKey, L"URLInfoAbout");
        RegWriteDword(hKey, L"NoModify",      1);
        RegWriteDword(hKey, L"NoRepair",      1);
        RegCloseKey(hKey);

        HKEY hNdm = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, NDM_APP_KEY, 0, nullptr, 0, KEY_WRITE, nullptr,
                            &hNdm, nullptr) == ERROR_SUCCESS)
        {
            RegWriteStr(hNdm, L"InstallDir",  dir);
            RegWriteStr(hNdm, L"InstallPath", dir);   // read by NDMShellExt.dll
            RegWriteStr(hNdm, L"ToolsDir",    dir + L"\\tools");
            RegWriteStr(hNdm, L"Version",     NDM_VERSION_WIDE);
            // The user who will run NDM (setup itself may run as another,
            // administrator, account): the uninstaller cleans that user's
            // Run value / shortcut / data through HKEY_USERS\<SID>.
            std::wstring user = NdmInteractiveUserSid();
            if (user.empty()) user = NdmSelfUserSid();
            if (NdmIsPlausibleSid(user)) RegWriteStr(hNdm, NDM_INSTALL_USER_SID_VALUE, user);
            else RegDeleteValueW(hNdm, NDM_INSTALL_USER_SID_VALUE);
            RegCloseKey(hNdm);
        }
    }

    // ---- 8. Shortcuts ----------------------------------------------------
    SetProgress(hDlg, 97, L"Creating shortcuts...");
    {
        std::wstring exe = dir + L"\\" + NDM_EXE_NAME;
        std::wstring menu = KnownFolder(CSIDL_COMMON_PROGRAMS);
        if (!menu.empty()) {
            menu += L"\\" NDM_PRODUCT;
            EnsureDir(menu);
            CreateShortcut(menu + L"\\" NDM_PRODUCT L".lnk", exe, dir, NDM_PRODUCT);
            CreateShortcut(menu + L"\\Uninstall NDM.lnk", dir + L"\\Uninstall.exe", dir,
                           L"Uninstall " NDM_PRODUCT);
        }
        if (p->desktop) {
            std::wstring desk = KnownFolder(CSIDL_COMMON_DESKTOPDIRECTORY);
            if (desk.empty()) desk = KnownFolder(CSIDL_DESKTOPDIRECTORY);
            if (!desk.empty())
                CreateShortcut(desk + L"\\" NDM_PRODUCT L".lnk", exe, dir, NDM_PRODUCT);
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    // ---- 9. Done -----------------------------------------------------------
    SetProgress(hDlg, 100, L"Installed");
    std::wstring exePath = dir + L"\\" + NDM_EXE_NAME;
    delete p;
    if (SUCCEEDED(hrCo)) CoUninitialize();
    PostMessageW(hDlg, WM_SETUP_DONE, 0, reinterpret_cast<LPARAM>(new std::wstring(exePath)));
    return 0;
}

// ---------------------------------------------------------------------------
// Visual layer (dark + gold) -- owner-drawn buttons, toggles, custom
// progress bar. Controls keep their IDs and standard messages (BM_GETCHECK,
// PBM_SETPOS, WM_GETTEXT) so the behaviour code is unaffected.
// ---------------------------------------------------------------------------
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <dwmapi.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

static ULONG_PTR g_gdipTok = 0;

static Gdiplus::Color GCol(COLORREF c, BYTE a = 255)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void GRound(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r)
{
    float d = r * 2; if (d > w) d = w; if (d > h) d = h;
    if (d < 1) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void GFillRound(HDC dc, RECT r, int rad, COLORREF top, COLORREF bot, BYTE a = 255)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath p;
    GRound(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)rad);
    Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1),
                                   GCol(top, a), GCol(bot, a));
    g.FillPath(&b, &p);
}

static void GFrameRound(HDC dc, RECT r, int rad, COLORREF c, float w = 1.0f, BYTE a = 255)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath p;
    float in = w / 2;
    GRound(p, r.left + in, r.top + in, (r.right - r.left) - w, (r.bottom - r.top) - w, rad - in);
    Gdiplus::Pen pen(GCol(c, a), w);
    g.DrawPath(&pen, &p);
}

static void GCircle(HDC dc, int cx, int cy, int rad, COLORREF c)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(GCol(c));
    g.FillEllipse(&b, (float)(cx - rad), (float)(cy - rad), (float)(2 * rad), (float)(2 * rad));
}

static void GText(HDC dc, const wchar_t* s, RECT r, COLORREF c, HFONT f, UINT fmt)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, o);
}

static void GFill(HDC dc, RECT r, COLORREF c)
{
    COLORREF o = SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    SetBkColor(dc, o);
}

// ---- RAII double buffer --------------------------------------------------
struct SetupMemDC
{
    HDC dst, dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; RECT rc;
    SetupMemDC(HDC d, RECT r) : dst(d), rc(r)
    {
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;
        dc = CreateCompatibleDC(d);
        if (!dc) return;
        bmp = CreateCompatibleBitmap(d, w, h);
        if (!bmp) { DeleteDC(dc); dc = nullptr; return; }
        old = SelectObject(dc, bmp);
        SetWindowOrgEx(dc, r.left, r.top, nullptr);
    }
    ~SetupMemDC()
    {
        if (!dc) return;
        SetWindowOrgEx(dc, 0, 0, nullptr);
        BitBlt(dst, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc);
    }
    HDC get() const { return dc ? dc : dst; }
};

// ---- owner-draw button subclass: hover, hand cursor, toggle check state --
static LRESULT CALLBACK SetupBtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR toggle)
{
    switch (m)
    {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, L"ndm.hot"))
        {
            SetPropW(h, L"ndm.hot", (HANDLE)1);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, L"ndm.hot");
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETCURSOR:
        if (IsWindowEnabled(h)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDBLCLK:
        m = WM_LBUTTONDOWN;
        break;
    case BM_GETCHECK:
        if (toggle) return GetPropW(h, L"ndm.chk") ? BST_CHECKED : BST_UNCHECKED;
        break;
    case BM_SETCHECK:
        if (toggle)
        {
            if (w == BST_CHECKED) SetPropW(h, L"ndm.chk", (HANDLE)1); else RemovePropW(h, L"ndm.chk");
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_KEYUP:
        if (toggle && (m == WM_LBUTTONUP || w == VK_SPACE))
        {
            bool pushed = (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
            RECT rc; GetClientRect(h, &rc);
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            if (pushed && (m == WM_KEYUP || PtInRect(&rc, pt)))
            {
                if (GetPropW(h, L"ndm.chk")) RemovePropW(h, L"ndm.chk"); else SetPropW(h, L"ndm.chk", (HANDLE)1);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        break;
    case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemovePropW(h, L"ndm.hot");
        RemovePropW(h, L"ndm.chk");
        RemoveWindowSubclass(h, SetupBtnProc, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---- custom progress bar (keeps PBM_* semantics of the real control) -----
static LRESULT CALLBACK SetupProgProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
{
    switch (m)
    {
    case WM_ERASEBKGND:
        return 1;
    case PBM_SETPOS:
    {
        if ((int)w == (int)SendMessageW(h, PBM_GETPOS, 0, 0)) return w;
        LRESULT r = DefSubclassProc(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        {
            SetupMemDC mem(hdc, rc);
            HDC dc = mem.get();
            GFill(dc, rc, T_BODY);
            int pos = (int)SendMessageW(h, PBM_GETPOS, 0, 0);
            int hgt = rc.bottom - rc.top;
            GFillRound(dc, rc, hgt / 2, RGB(40, 44, 53), RGB(36, 40, 48));
            if (pos > 0)
            {
                RECT f = rc;
                f.right = rc.left + (rc.right - rc.left) * (pos > 100 ? 100 : pos) / 100;
                if (f.right - f.left < hgt) f.right = f.left + hgt;
                if (g_state == ST_FINISHED) GFillRound(dc, f, hgt / 2, RGB(120, 220, 150), RGB(60, 170, 100));
                else
                {
                    Gdiplus::Graphics g(dc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    Gdiplus::GraphicsPath p;
                    GRound(p, (float)f.left, (float)f.top, (float)(f.right - f.left), (float)hgt, hgt / 2.0f);
                    Gdiplus::LinearGradientBrush b(Gdiplus::PointF((float)f.left - 1, 0),
                                                   Gdiplus::PointF((float)f.right + 1, 0),
                                                   GCol(T_GOLD_DARK), GCol(T_GOLD_LIGHT));
                    g.FillPath(&b, &p);
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, SetupProgProc, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static void DrawSetupButton(const DRAWITEMSTRUCT* di)
{
    RECT rc = di->rcItem;
    SetupMemDC mem(di->hDC, rc);
    HDC dc = mem.get();
    bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    bool focus    = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    bool hot      = !disabled && GetPropW(di->hwndItem, L"ndm.hot") != nullptr;
    int  rad      = SS(5);
    wchar_t txt[96] = {};
    GetWindowTextW(di->hwndItem, txt, 96);

    COLORREF parent = (di->CtlID == IDC_PATH_BROWSE || di->CtlID == IDC_DESKTOP_CHK ||
                       di->CtlID == IDC_LAUNCH_CHK) ? T_BODY : T_FOOTER;
    GFill(dc, rc, parent);

    if (di->CtlID == IDC_DESKTOP_CHK || di->CtlID == IDC_LAUNCH_CHK)
    {
        bool on = GetPropW(di->hwndItem, L"ndm.chk") != nullptr;
        int th = SS(16), tw = SS(28);
        int ty = rc.top + ((rc.bottom - rc.top) - th) / 2;
        RECT tr = { rc.left + SS(2), ty, rc.left + SS(2) + tw, ty + th };
        if (on) GFillRound(dc, tr, th / 2, disabled ? RGB(110, 94, 40) : T_GOLD_LIGHT,
                           disabled ? RGB(90, 76, 30) : T_GOLD_DARK);
        else
        {
            GFillRound(dc, tr, th / 2, hot ? RGB(62, 67, 80) : RGB(50, 54, 64), hot ? RGB(62, 67, 80) : RGB(50, 54, 64));
            GFrameRound(dc, tr, th / 2, T_BORDER);
        }
        if (focus) GFrameRound(dc, { tr.left - 2, tr.top - 2, tr.right + 2, tr.bottom + 2 }, th / 2 + 2, T_GOLD, 1.0f, 160);
        int kr = th / 2 - SS(3);
        GCircle(dc, on ? tr.right - th / 2 : tr.left + th / 2, ty + th / 2, kr,
                on ? RGB(30, 26, 10) : RGB(200, 202, 210));
        RECT lr = rc; lr.left = tr.right + SS(8);
        GText(dc, txt, lr, disabled ? RGB(100, 102, 110) : T_FG, g_hfUI,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    if (di->CtlID == IDC_INSTALL_BTN)
    {
        if (disabled)
        {
            GFillRound(dc, rc, rad, RGB(58, 54, 40), RGB(52, 48, 36));
            GText(dc, txt, rc, RGB(130, 122, 100), g_hfBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            GFillRound(dc, rc, rad, pressed ? T_GOLD_DARK : (hot ? RGB(250, 222, 120) : T_GOLD_LIGHT),
                       pressed ? RGB(140, 110, 26) : (hot ? T_GOLD : RGB(190, 152, 40)));
            GFrameRound(dc, rc, rad, RGB(255, 236, 160), 1.0f, pressed ? 40 : 90);
            RECT t = rc; OffsetRect(&t, 0, pressed ? 1 : 0);
            GText(dc, txt, t, RGB(24, 20, 8), g_hfBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (focus) GFrameRound(dc, { rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2 }, rad - 1, RGB(60, 48, 10), 1.0f, 120);
        }
        return;
    }

    if (di->CtlID == IDC_CANCEL_BTN)
    {
        if (!disabled && (hot || pressed))
            GFillRound(dc, rc, rad, pressed ? T_BG_ALT : RGB(40, 44, 54), pressed ? T_BG_ALT : RGB(40, 44, 54));
        if (focus) GFrameRound(dc, rc, rad, T_GOLD, 1.0f, 160);
        RECT t = rc; OffsetRect(&t, 0, pressed ? 1 : 0);
        GText(dc, txt, t, disabled ? RGB(90, 92, 100) : (hot ? T_GOLD_LIGHT : T_GOLD), g_hfBold,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    // Browse (secondary)
    COLORREF bg = disabled ? T_BG_PANEL : pressed ? RGB(50, 44, 10) : (hot ? RGB(40, 44, 54) : RGB(34, 38, 46));
    GFillRound(dc, rc, rad, bg, bg);
    GFrameRound(dc, rc, rad, disabled ? T_BORDER : ((hot || focus) ? T_BORDER_GOLD : T_BORDER));
    RECT t = rc; OffsetRect(&t, 0, pressed ? 1 : 0);
    GText(dc, txt, t, disabled ? RGB(90, 92, 100) : (hot ? T_GOLD_LIGHT : T_FG), g_hfBold,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// DPI handling (per-monitor v2 via manifest; falls back to system DPI)
// ---------------------------------------------------------------------------
static int QueryWindowDpi(HWND h)
{
    typedef UINT (WINAPI* PFN_GDFW)(HWND);
    static PFN_GDFW pfn = (PFN_GDFW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    int dpi = 0;
    if (pfn && h) dpi = (int)pfn(h);
    if (dpi <= 0) {
        HDC sdc = GetDC(nullptr);
        dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSY) : 96;
        if (sdc) ReleaseDC(nullptr, sdc);
    }
    return dpi < 96 ? 96 : dpi;
}

static int MetricForDpi(int idx)
{
    typedef int (WINAPI* PFN_GSMFD)(int, UINT);
    static PFN_GSMFD pfn = (PFN_GSMFD)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    return pfn ? pfn(idx, (UINT)g_dpi) : GetSystemMetrics(idx);
}

static HICON LoadAppIcon(int size)
{
    HICON h = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_NDM_APP),
                                     size, size, &h)) || !h)
        h = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_NDM_APP), IMAGE_ICON,
                              size, size, LR_DEFAULTCOLOR);
    return h;
}

static void FreeDpiResources()
{
    HFONT* fonts[] = { &g_hfUI, &g_hfBold, &g_hfTitle };
    for (HFONT* f : fonts) if (*f) { DeleteObject(*f); *f = nullptr; }
    HICON* icons[] = { &g_hIconBig, &g_hIconSm, &g_hIconLogo };
    for (HICON* i : icons) if (*i) { DestroyIcon(*i); *i = nullptr; }
}

static void CreateDpiResources(HWND hDlg)
{
    FreeDpiResources();
    auto mk = [](int px, int wt) {
        return CreateFontW(-SS(px), 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    g_hfUI    = mk(12, FW_NORMAL);
    g_hfBold  = mk(12, FW_SEMIBOLD);
    g_hfTitle = mk(15, FW_SEMIBOLD);
    g_hIconBig  = LoadAppIcon(MetricForDpi(SM_CXICON));
    g_hIconSm   = LoadAppIcon(MetricForDpi(SM_CXSMICON));
    g_hIconLogo = LoadAppIcon(SS(28));
    SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)g_hIconBig);
    SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconSm);
}

// Client layout in logical px (96 dpi): 420 x 200
static const int kW = 420, kH = 200;

static void LayoutControls(HWND hDlg)
{
    const int W = SS(kW), H = SS(kH), M = SS(16);
    auto place = [&](int id, int x, int y, int w, int h, HFONT f) {
        HWND c = GetDlgItem(hDlg, id);
        if (!c) return;
        SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (f) SendMessageW(c, WM_SETFONT, (WPARAM)f, FALSE);
        InvalidateRect(c, nullptr, TRUE);
    };
    // Row 1 (painted): logo + product title, y 12..40
    // Row 2: install folder + Browse, y 52..80
    const int bw = SS(80), fy = SS(52), fh = SS(28);
    g_rcField = { M, fy, W - M - bw - SS(8), fy + fh };
    TEXTMETRICW tm = {};
    {
        HDC dc = GetDC(hDlg);
        HGDIOBJ o = SelectObject(dc, g_hfUI);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, o);
        ReleaseDC(hDlg, dc);
    }
    // A single-line EDIT draws its text at the very top of its client area,
    // so the control must be exactly one line tall to sit centred in the field.
    int eh = tm.tmHeight;
    place(IDC_PATH_EDIT, g_rcField.left + SS(8), g_rcField.top + (fh - eh + 1) / 2,
          (g_rcField.right - g_rcField.left) - SS(16), eh, g_hfUI);
    {
        // Created tiny, an ES_AUTOHSCROLL edit stays scrolled to its end:
        // show the start of the path again.
        HWND e = GetDlgItem(hDlg, IDC_PATH_EDIT);
        DWORD s0 = 0, s1 = 0;
        SendMessageW(e, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
        SendMessageW(e, EM_SETSEL, 0, 0);
        SendMessageW(e, EM_SCROLLCARET, 0, 0);
        if (GetFocus() == e) SendMessageW(e, EM_SETSEL, s0, s1);
    }
    place(IDC_PATH_BROWSE, W - M - bw, fy, bw, fh, g_hfBold);
    // Row 3: two toggles side by side, y 88..110
    const int half = (W - 2 * M) / 2;
    place(IDC_DESKTOP_CHK, M, SS(88), half - SS(4), SS(22), g_hfUI);
    place(IDC_LAUNCH_CHK, M + half, SS(88), W - 2 * M - half, SS(22), g_hfUI);
    // Row 4: status line + progress bar
    place(IDC_STATUS_LABEL, M, SS(118), W - 2 * M, SS(18), g_hfUI);
    place(IDC_PROGRESS, M, SS(140), W - 2 * M, SS(6), nullptr);
    // Footer: buttons
    g_rcFooter = { 0, SS(156), W, H };
    const int bh = SS(28), by = g_rcFooter.top + ((H - g_rcFooter.top) - bh) / 2;
    place(IDC_INSTALL_BTN, W - M - SS(92), by, SS(92), bh, g_hfBold);
    place(IDC_CANCEL_BTN, W - M - SS(92) - SS(8) - SS(80), by, SS(80), bh, g_hfBold);
    InvalidateRect(hDlg, nullptr, TRUE);
}

static void SizeWindowForDpi(HWND hDlg, const RECT* suggested)
{
    RECT wr = { 0, 0, SS(kW), SS(kH) };
    typedef BOOL (WINAPI* PFN_AWREFD)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN_AWREFD pfn = (PFN_AWREFD)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    DWORD st = (DWORD)GetWindowLongPtrW(hDlg, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
    if (pfn) pfn(&wr, st, FALSE, ex, (UINT)g_dpi); else AdjustWindowRectEx(&wr, st, FALSE, ex);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (suggested) {
        SetWindowPos(hDlg, nullptr, suggested->left, suggested->top, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hDlg, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    SetWindowPos(hDlg, nullptr, wa.left + ((wa.right - wa.left) - ww) / 2,
                 wa.top + ((wa.bottom - wa.top) - wh) / 2, ww, wh, SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// UI state transitions
// ---------------------------------------------------------------------------
static void EnableInputs(HWND hDlg, bool en)
{
    const int ids[] = { IDC_PATH_EDIT, IDC_PATH_BROWSE, IDC_DESKTOP_CHK, IDC_LAUNCH_CHK, IDC_INSTALL_BTN };
    for (int id : ids) EnableWindow(GetDlgItem(hDlg, id), en);
}

static void EnterInstalling(HWND hDlg)
{
    g_state = ST_INSTALLING;
    EnableInputs(hDlg, false);
    InvalidateRect(hDlg, nullptr, TRUE);
}

static void EnterReady(HWND hDlg, const wchar_t* status)
{
    g_state = ST_READY;
    EnableInputs(hDlg, true);
    ShowWindow(GetDlgItem(hDlg, IDC_CANCEL_BTN), SW_SHOW);
    SetDlgItemTextW(hDlg, IDC_INSTALL_BTN, L"Install");
    SetProgress(hDlg, 0, status);
    InvalidateRect(hDlg, nullptr, TRUE);
}

static void EnterFinished(HWND hDlg)
{
    g_state = ST_FINISHED;
    EnableInputs(hDlg, false);
    SetDlgItemTextW(hDlg, IDC_INSTALL_BTN, L"Finish");
    ShowWindow(GetDlgItem(hDlg, IDC_CANCEL_BTN), SW_HIDE);
    EnableWindow(GetDlgItem(hDlg, IDC_INSTALL_BTN), TRUE);
    EnableWindow(GetDlgItem(hDlg, IDC_LAUNCH_CHK), TRUE);   // still choosable
    SetProgress(hDlg, 100, L"Installed");
    InvalidateRect(GetDlgItem(hDlg, IDC_PROGRESS), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hDlg, IDC_STATUS_LABEL), nullptr, TRUE);
    InvalidateRect(hDlg, nullptr, TRUE);
    SetFocus(GetDlgItem(hDlg, IDC_INSTALL_BTN));
}

// ---------------------------------------------------------------------------
// Setup Dialog Procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SetupDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g_hDlg = hDlg;
        HINSTANCE hi = GetModuleHandleW(nullptr);

        if (!g_gdipTok) {
            Gdiplus::GdiplusStartupInput in;
            Gdiplus::GdiplusStartup(&g_gdipTok, &in, nullptr);
        }
        // We lay out every control ourselves: opt out of automatic dialog
        // DPI scaling (Windows 10 1703+).
        typedef BOOL (WINAPI* PFN_SDDCB)(HWND, int, int);
        PFN_SDDCB sddcb = (PFN_SDDCB)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetDialogDpiChangeBehavior");
        if (sddcb) sddcb(hDlg, 1 /*DDC_DISABLE_ALL*/, 1 /*DDC_DISABLE_ALL*/);
        g_dpi = QueryWindowDpi(hDlg);

        g_hbrBody   = CreateSolidBrush(T_BODY);
        g_hbrEdit   = CreateSolidBrush(T_EDIT_BG);
        g_hbrFooter = CreateSolidBrush(T_FOOTER);
        CreateDpiResources(hDlg);

        BOOL dark = TRUE;   // dark title bar
        if (FAILED(DwmSetWindowAttribute(hDlg, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hDlg, 19, &dark, sizeof(dark));

        auto ctl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id) {
            return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                   hDlg, (HMENU)(INT_PTR)id, hi, nullptr);
        };
        ctl(L"EDIT", GetDefaultInstallDir().c_str(), WS_TABSTOP | ES_AUTOHSCROLL, IDC_PATH_EDIT);
        SendDlgItemMessageW(hDlg, IDC_PATH_EDIT, EM_LIMITTEXT, MAX_PATH - 1, 0);
        SetWindowSubclass(ctl(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, IDC_PATH_BROWSE),
                          SetupBtnProc, 1, 0);
        SetWindowSubclass(ctl(L"BUTTON", L"Desktop shortcut", WS_TABSTOP | BS_OWNERDRAW, IDC_DESKTOP_CHK),
                          SetupBtnProc, 1, 1);
        SetWindowSubclass(ctl(L"BUTTON", L"Launch after install", WS_TABSTOP | BS_OWNERDRAW, IDC_LAUNCH_CHK),
                          SetupBtnProc, 1, 1);
        CheckDlgButton(hDlg, IDC_DESKTOP_CHK, BST_CHECKED);
        CheckDlgButton(hDlg, IDC_LAUNCH_CHK, BST_CHECKED);
        ctl(L"STATIC", L"Ready to install.", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS_LABEL);
        HWND hProg = ctl(PROGRESS_CLASSW, nullptr, PBS_SMOOTH, IDC_PROGRESS);
        SendMessageW(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SetWindowSubclass(hProg, SetupProgProc, 1, 0);
        HWND hCancel = ctl(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, IDC_CANCEL_BTN);
        SetWindowSubclass(hCancel, SetupBtnProc, 1, 0);
        HWND hInstall = ctl(L"BUTTON", L"Install", WS_TABSTOP | BS_OWNERDRAW, IDC_INSTALL_BTN);
        SetWindowSubclass(hInstall, SetupBtnProc, 1, 0);

        SizeWindowForDpi(hDlg, nullptr);
        LayoutControls(hDlg);

        SetFocus(hInstall);
        return FALSE;      // focus set explicitly
    }

    case WM_DPICHANGED:
    {
        g_dpi = HIWORD(wParam) ? HIWORD(wParam) : 96;
        CreateDpiResources(hDlg);
        SizeWindowForDpi(hDlg, reinterpret_cast<const RECT*>(lParam));
        LayoutControls(hDlg);
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!di || di->CtlType != ODT_BUTTON) return FALSE;
        DrawSetupButton(di);
        return TRUE;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hDlg, &rc);
        SetupMemDC mem(hdc, rc);
        HDC dc = mem.get();
        GFill(dc, rc, T_BODY);
        // Title row: logo + product name
        const int ls = SS(28), lx = SS(16), ly = SS(12);
        if (g_hIconLogo) DrawIconEx(dc, lx, ly, g_hIconLogo, ls, ls, 0, nullptr, DI_NORMAL);
        RECT t = { lx + ls + SS(10), ly, rc.right - SS(16), ly + ls };
        GText(dc, NDM_PRODUCT, t, T_FG, g_hfTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // Install folder field
        GFillRound(dc, g_rcField, SS(5), T_EDIT_BG, T_EDIT_BG);
        GFrameRound(dc, g_rcField, SS(5), T_BORDER);
        // Footer band
        GFill(dc, g_rcFooter, T_FOOTER);
        RECT ln = { 0, g_rcFooter.top, rc.right, g_rcFooter.top + 1 };
        GFill(dc, ln, T_BORDER);
        return 1;
    }

    case WM_CTLCOLORSTATIC:   // status label, and the read-only/disabled edit
    {
        HDC hdc = (HDC)wParam;
        int id = GetDlgCtrlID((HWND)lParam);
        if (id == IDC_PATH_EDIT) {
            SetTextColor(hdc, T_FG_DIM);
            SetBkColor(hdc, T_EDIT_BG);
            return (INT_PTR)g_hbrEdit;
        }
        SetTextColor(hdc, g_state == ST_FINISHED ? T_GREEN : T_FG_DIM);
        SetBkColor(hdc, T_BODY);
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)g_hbrBody;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, T_FG);
        SetBkColor(hdc, T_EDIT_BG);
        return (INT_PTR)g_hbrEdit;
    }

    case WM_CTLCOLORDLG:
        return (INT_PTR)g_hbrBody;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        if (id == IDC_PATH_BROWSE)
        {
            BROWSEINFOW bi = {};
            bi.hwndOwner   = hDlg;
            bi.lpszTitle   = L"Select installation folder:";
            bi.ulFlags     = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t folder[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, folder)) {
                    // Picking a parent folder installs into <parent>\NDM.
                    std::wstring f = folder;
                    const wchar_t* leaf = PathFindFileNameW(folder);
                    if (_wcsicmp(leaf, L"NDM") != 0) {
                        if (!f.empty() && f.back() != L'\\') f += L'\\';
                        f += L"NDM";
                    }
                    SetDlgItemTextW(hDlg, IDC_PATH_EDIT, f.c_str());
                }
                CoTaskMemFree(pidl);
            }
            return TRUE;
        }

        if (id == IDC_INSTALL_BTN || id == IDOK)
        {
            if (g_state == ST_FINISHED) {
                if (IsDlgButtonChecked(hDlg, IDC_LAUNCH_CHK) == BST_CHECKED && !g_launchExe.empty())
                    LaunchUnelevated(g_launchExe, L"-firstrun");
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (g_state != ST_READY) return TRUE;

            wchar_t pathBuf[MAX_PATH] = {};
            GetDlgItemTextW(hDlg, IDC_PATH_EDIT, pathBuf, MAX_PATH);

            InstallParams* p = new InstallParams();
            p->hDlg       = hDlg;
            p->installDir = pathBuf;
            p->desktop    = (IsDlgButtonChecked(hDlg, IDC_DESKTOP_CHK) == BST_CHECKED);
            p->launch     = (IsDlgButtonChecked(hDlg, IDC_LAUNCH_CHK)  == BST_CHECKED);
            g_cancelled   = 0;
            EnterInstalling(hDlg);
            SetFocus(GetDlgItem(hDlg, IDC_CANCEL_BTN));

            HANDLE hThread = CreateThread(nullptr, 0, InstallThread, p, 0, nullptr);
            if (hThread) CloseHandle(hThread);
            else {
                DWORD le = GetLastError();
                delete p;
                MessageBoxW(hDlg, (L"Cannot start setup.\n\n" + ErrorText(le)).c_str(),
                            L"Setup Error", MB_OK | MB_ICONERROR);
                EnterReady(hDlg, L"Ready to install.");
            }
            return TRUE;
        }

        if (id == IDC_CANCEL_BTN || id == IDCANCEL)
        {
            if (g_state == ST_INSTALLING) {
                InterlockedExchange(&g_cancelled, 1);
                SetDlgItemTextW(hDlg, IDC_STATUS_LABEL, L"Cancelling...");
            } else if (g_state == ST_FINISHED) {
                EndDialog(hDlg, IDOK);
            } else {
                EndDialog(hDlg, IDCANCEL);
            }
            return TRUE;
        }
        return 0;
    }

    case WM_SETUP_DONE:
    {
        std::wstring* pExe = reinterpret_cast<std::wstring*>(lParam);
        if (pExe) { g_launchExe = *pExe; delete pExe; }
        EnterFinished(hDlg);
        return 0;
    }

    case WM_SETUP_ERROR:
    {
        std::wstring* m = reinterpret_cast<std::wstring*>(lParam);
        bool userCancel = (wParam == 1);
        if (!userCancel)
            MessageBoxW(hDlg, m ? m->c_str() : L"An unknown error occurred.", L"Setup Error",
                        MB_OK | MB_ICONERROR);
        delete m;
        EnterReady(hDlg, userCancel ? L"Installation cancelled." : L"Installation failed. Please try again.");
        return 0;
    }

    case WM_CLOSE:
        SendMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
        return TRUE;

    case WM_DESTROY:
    {
        FreeDpiResources();
        HBRUSH* br[] = { &g_hbrBody, &g_hbrEdit, &g_hbrFooter };
        for (HBRUSH* b : br) if (*b) { DeleteObject(*b); *b = nullptr; }
        return 0;
    }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // DLL planting: setup typically runs from Downloads. Static imports are
    // KnownDLLs only (/DEPENDENTLOADFLAG:0x800 as well); everything else is
    // delay-loaded and from now on resolved from System32 only.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    // Single-instance guard
    HANDLE hMutex = CreateMutexA(nullptr, TRUE, NDM_SETUP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"NDM Setup is already running.", L"Setup", MB_OK | MB_ICONWARNING);
        return 1;
    }

    CoInitialize(nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // In-memory dialog template. DS_SETFONT requires the point size +
    // typeface record right after the title.
    struct alignas(DWORD) {
        DLGTEMPLATE tmpl;
        WORD        menu;
        WORD        cls;
        wchar_t     title[27];      // exactly "Nee Download Manager Setup" + NUL:
        WORD        pointSize;      // the font record must follow the title
        wchar_t     font[9];        // "Segoe UI" + NUL
    } dlgTmpl = {};
    static_assert(sizeof(L"Nee Download Manager Setup") == sizeof(dlgTmpl.title), "title size");
    static_assert(sizeof(L"Segoe UI") == sizeof(dlgTmpl.font), "font size");

    dlgTmpl.tmpl.style           = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION |
                                   WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    dlgTmpl.tmpl.dwExtendedStyle = WS_EX_APPWINDOW;
    dlgTmpl.tmpl.cx              = 280;     // resized in WM_INITDIALOG
    dlgTmpl.tmpl.cy              = 130;
    wcscpy_s(dlgTmpl.title, L"Nee Download Manager Setup");
    dlgTmpl.pointSize            = 9;
    wcscpy_s(dlgTmpl.font, L"Segoe UI");

    DialogBoxIndirectParamW(hInst, &dlgTmpl.tmpl, nullptr, SetupDlgProc, 0);

    if (g_gdipTok) { Gdiplus::GdiplusShutdown(g_gdipTok); g_gdipTok = 0; }
    CoUninitialize();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}
