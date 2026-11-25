#pragma once
// ---------------------------------------------------------------------------
// File-system / security helpers shared by the installer and the uninstaller.
//
//   - NdmDeleteNoFollow: handle-based recursive delete that never follows a
//     reparse point (symlink / junction / mount point are removed as links).
//   - NdmPathHasReparsePoint: refuse install paths that traverse a link.
//   - NdmIsTrustedOwnerSid / NdmGetOwnerIsTrusted: Administrators, SYSTEM or
//     TrustedInstaller.
//   - NdmSecurityDescriptor: SDDL -> SECURITY_DESCRIPTOR (RAII).
//   - NdmRandomHex: BCryptGenRandom based random names.
//   - NdmRemoveInstalledFiles: delete exactly the files/dirs an NDM install
//     owns (user files in the folder are never touched).
// ---------------------------------------------------------------------------
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

// SY / BA full control, BU read + execute; protected (no inheritance from the
// parent), inherited by everything created below. Owner: Administrators.
#define NDM_SDDL_INSTALL_DIR L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"
// SY / BA only (uninstaller stage-2 temp folder).
#define NDM_SDDL_ADMIN_ONLY  L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"

static inline std::wstring NdmStripSlash(std::wstring d)
{
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

static inline bool NdmSamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(NdmStripSlash(a).c_str(), NdmStripSlash(b).c_str()) == 0;
}

// "\\?\" prefix for long drive-absolute paths.
static inline std::wstring NdmLongPath(const std::wstring& p)
{
    if (p.size() < MAX_PATH - 12 || p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.size() >= 3 && p[1] == L':' && p[2] == L'\\') return L"\\\\?\\" + p;
    return p;
}

// ---- SDDL security descriptor (RAII) -------------------------------------
struct NdmSecurityDescriptor
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    explicit NdmSecurityDescriptor(const wchar_t* sddl)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
            sd = nullptr;
    }
    ~NdmSecurityDescriptor() { if (sd) LocalFree(sd); }
    NdmSecurityDescriptor(const NdmSecurityDescriptor&) = delete;
    NdmSecurityDescriptor& operator=(const NdmSecurityDescriptor&) = delete;
    PACL Dacl() const
    {
        BOOL present = FALSE, def = FALSE; PACL acl = nullptr;
        return (sd && GetSecurityDescriptorDacl(sd, &present, &acl, &def) && present) ? acl : nullptr;
    }
    PSID Owner() const
    {
        BOOL def = FALSE; PSID o = nullptr;
        return (sd && GetSecurityDescriptorOwner(sd, &o, &def)) ? o : nullptr;
    }
};

// Apply `sddl` (owner + protected DACL) to an existing file-system object.
static inline DWORD NdmApplySddl(const std::wstring& path, const wchar_t* sddl)
{
    NdmSecurityDescriptor s(sddl);
    if (!s.sd || !s.Dacl() || !s.Owner()) return GetLastError() ? GetLastError() : ERROR_INVALID_SECURITY_DESCR;
    std::wstring p = path;   // SetNamedSecurityInfoW takes a non-const LPWSTR
    return SetNamedSecurityInfoW(&p[0], SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                 PROTECTED_DACL_SECURITY_INFORMATION,
                                 s.Owner(), nullptr, s.Dacl(), nullptr);
}

// ---- owner checks ----------------------------------------------------------
static inline bool NdmIsTrustedOwnerSid(PSID owner)
{
    if (!owner || !IsValidSid(owner)) return false;
    if (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid))
        return true;
    PSID ti = nullptr;   // NT SERVICE\TrustedInstaller
    bool eq = false;
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &ti)) {
        eq = EqualSid(owner, ti) != FALSE;
        LocalFree(ti);
    }
    return eq;
}

static inline bool NdmHandleOwnerIsTrusted(HANDLE h)
{
    PSID owner = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd)
        != ERROR_SUCCESS)
        return false;
    bool ok = NdmIsTrustedOwnerSid(owner);
    LocalFree(sd);
    return ok;
}

// ---- reparse points --------------------------------------------------------
// Open without following a final reparse point.
static inline HANDLE NdmOpenNoFollow(const std::wstring& path, DWORD access, DWORD share)
{
    return CreateFileW(NdmLongPath(path).c_str(), access, share, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

static inline bool NdmHandleAttrs(HANDLE h, DWORD& attrs)
{
    FILE_ATTRIBUTE_TAG_INFO ti = {};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof(ti))) return false;
    attrs = ti.FileAttributes;
    return true;
}

// True if any existing component of the drive-absolute path `full` (from the
// first folder below the drive root down to the leaf) is a reparse point or
// is not a directory, or cannot be inspected. Components that do not exist
// yet are fine (they will be created as plain folders).
static inline bool NdmPathHasReparsePoint(const std::wstring& full)
{
    if (full.size() < 3 || full[1] != L':' || full[2] != L'\\') return true;
    for (size_t i = 3; i <= full.size(); i++) {
        if (i != full.size() && full[i] != L'\\') continue;
        if (i == 3) continue;
        std::wstring pre = full.substr(0, i);
        DWORD a = GetFileAttributesW(NdmLongPath(pre).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            return !(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);
        }
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || !(a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}

// The real path behind an open handle, without the "\\?\" prefix.
static inline std::wstring NdmFinalPath(HANDLE h)
{
    std::wstring s(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, &s[0], (DWORD)s.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= s.size()) { s.resize(n + 1); n = GetFinalPathNameByHandleW(h, &s[0], (DWORD)s.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS); }
    if (n == 0 || n >= s.size()) return {};
    s.resize(n);
    if (s.compare(0, 8, L"\\\\?\\UNC\\") == 0) s = L"\\\\" + s.substr(8);
    else if (s.compare(0, 4, L"\\\\?\\") == 0) s = s.substr(4);
    return s;
}

// Long (non-8.3) form of an existing path, for comparing with NdmFinalPath.
static inline std::wstring NdmLongName(const std::wstring& p)
{
    std::wstring s(1024, L'\0');
    DWORD n = GetLongPathNameW(p.c_str(), &s[0], (DWORD)s.size());
    if (n == 0 || n >= s.size()) return p;
    s.resize(n);
    return s;
}

// ---- deletion --------------------------------------------------------------
static inline bool NdmMarkForDelete(HANDLE h)
{
    FILE_DISPOSITION_INFO d = { TRUE };
    return SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof(d)) != FALSE;
}

static inline void NdmClearReadOnly(HANDLE h)
{
    FILE_BASIC_INFO bi = {};
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) return;
    if (!(bi.FileAttributes & FILE_ATTRIBUTE_READONLY)) return;
    bi.FileAttributes &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
    if (bi.FileAttributes == 0) bi.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof(bi));
}

static inline void NdmDeleteAtReboot(const std::wstring& path, bool& reboot)
{
    if (MoveFileExW(NdmLongPath(path).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) reboot = true;
}

// Names of the entries of an open directory handle (without "." / "..").
static inline bool NdmListDir(HANDLE h, std::vector<std::wstring>& names)
{
    std::vector<BYTE> buf(64 * 1024);
    FILE_INFO_BY_HANDLE_CLASS cls = FileFullDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(h, cls, buf.data(), (DWORD)buf.size()))
            return GetLastError() == ERROR_NO_MORE_FILES;
        cls = FileFullDirectoryInfo;
        BYTE* p = buf.data();
        for (;;) {
            auto* fi = reinterpret_cast<FILE_FULL_DIR_INFO*>(p);
            std::wstring n(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
            if (n != L"." && n != L"..") names.push_back(n);
            if (!fi->NextEntryOffset) break;
            p += fi->NextEntryOffset;
        }
    }
}

// Delete `path` (file or directory tree) without ever following a reparse
// point: every object is opened with FILE_FLAG_OPEN_REPARSE_POINT, its
// attributes are checked on the handle, links are deleted as links, and a
// directory is held open (no FILE_SHARE_DELETE, so it cannot be renamed or
// swapped) while its children are removed. Objects that are in use are
// scheduled for deletion at reboot (`reboot` = true) unless `atReboot` is
// false: inside a user-writable folder (a user profile) an elevated caller
// must not leave a delete-at-reboot entry behind, because that runs later as
// SYSTEM by path and would follow a link planted there meanwhile.
// Returns true if `path` is gone (or will be once the last handle closes).
static inline bool NdmDeleteNoFollow(const std::wstring& path, bool& reboot, int depth = 0, bool atReboot = true)
{
    if (depth > 64) return false;
    HANDLE h = NdmOpenNoFollow(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                                     FILE_LIST_DIRECTORY | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        // In use (sharing violation / mapped image): retry without the list
        // right, then fall back to delete-at-reboot.
        h = NdmOpenNoFollow(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (h == INVALID_HANDLE_VALUE) { if (atReboot) NdmDeleteAtReboot(path, reboot); return false; }
    }
    DWORD attrs = 0;
    if (!NdmHandleAttrs(h, attrs)) { CloseHandle(h); return false; }

    bool ok = true;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<std::wstring> names;
        if (NdmListDir(h, names)) {
            for (const auto& n : names)
                if (!NdmDeleteNoFollow(path + L"\\" + n, reboot, depth + 1, atReboot)) ok = false;
        } else {
            ok = false;
        }
    } else if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        NdmClearReadOnly(h);
    }
    // A reparse point (file or directory link) is deleted as the link itself.
    if (!NdmMarkForDelete(h)) {
        CloseHandle(h);
        if (atReboot) NdmDeleteAtReboot(path, reboot);   // locked file, or folder with locked children
        return false;
    }
    CloseHandle(h);
    return ok;
}

// ---- random ----------------------------------------------------------------
static inline std::wstring NdmRandomHex(size_t bytes)
{
    std::vector<BYTE> b(bytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring s;
    for (BYTE x : b) { s += hex[x >> 4]; s += hex[x & 15]; }
    return s;
}

// ---- NDM install content ---------------------------------------------------
static inline bool NdmFileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(NdmLongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline bool NdmLooksLikeInstall(const std::wstring& dir)
{
    return NdmFileExists(dir + L"\\NDMan.exe") || NdmFileExists(dir + L"\\Uninstall.exe") ||
           NdmFileExists(dir + L"\\NDMMsgHost.exe");
}

// Delete what an NDM install put into `dir` (the caller verified `dir`).
// User files elsewhere in the folder are left alone.
static inline void NdmRemoveInstalledFiles(const std::wstring& dir, bool& reboot)
{
    static const wchar_t* const kFiles[] = {
        L"NDMan.exe", L"NDMMsgHost.exe", L"NDMShellExt.dll", L"Uninstall.exe",
        L"NDMMsgHost.json", L"NDMMsgHost_chrome.json", L"NDMMsgHost_firefox.json",
        L"LICENSE.txt", L"THIRD_PARTY_NOTICES.txt",
    };
    for (const wchar_t* f : kFiles) NdmDeleteNoFollow(dir + L"\\" + f, reboot);

    static const wchar_t* const kDirs[] = { L"tools", L"ext", L"Languages", L"Toolbar" };
    for (const wchar_t* d : kDirs) NdmDeleteNoFollow(dir + L"\\" + d, reboot);

    // In-use files the installer renamed aside (*.old<tick>)
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW((NdmLongPath(dir) + L"\\*.old*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                NdmDeleteNoFollow(dir + L"\\" + fd.cFileName, reboot);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
}

// ---- upgrade into another folder (pure; unit-tested) ------------------------
// True if `path` is strictly inside `dir` (case-insensitive, '\' separated).
static inline bool NdmPathIsInside(const std::wstring& path, const std::wstring& dir)
{
    std::wstring p = NdmStripSlash(path), d = NdmStripSlash(dir);
    if (d.empty() || p.size() <= d.size() || _wcsnicmp(p.c_str(), d.c_str(), d.size()) != 0) return false;
    if (d.back() == L'\\') return true;                       // "C:\" root
    return p[d.size()] == L'\\' && p.size() > d.size() + 1;
}

// Folder of a previous x64 install that setup should clean up before the new
// one is registered. `candidates` are what that install recorded (uninstall
// key InstallLocation, then HKLM\Software\Devnetra\NDM InstallDir). Returns
// the first candidate that is a plain drive-absolute path ("X:\..." without
// "." / ".." components or forward slashes), is not `newDir`, and is neither
// inside `newDir` nor contains it (removing NDM's files there would hit the
// new install). "" = nothing to clean. The caller still checks the folder on
// disk (system folder, links, looks like an NDM install) before deleting.
static inline std::wstring NdmPickPreviousInstallDir(const std::vector<std::wstring>& candidates,
                                              const std::wstring& newDir)
{
    for (std::wstring c : candidates) {
        c = NdmStripSlash(c);
        if (c.size() < 4 || !iswalpha(c[0]) || c[1] != L':' || c[2] != L'\\') continue;
        if (c.find(L'/') != std::wstring::npos || c.find_first_of(L"*?\"<>|") != std::wstring::npos ||
            c.find(L':', 2) != std::wstring::npos)
            continue;
        bool dots = false;
        for (size_t i = 2; i < c.size() && !dots; i++) {
            if (c[i] != L'\\') continue;
            size_t e = c.find(L'\\', i + 1);
            std::wstring comp = c.substr(i + 1, e == std::wstring::npos ? std::wstring::npos : e - i - 1);
            dots = comp.empty() || comp == L"." || comp == L"..";
        }
        if (dots) continue;
        if (NdmSamePath(c, newDir) || NdmPathIsInside(c, newDir) || NdmPathIsInside(newDir, c)) continue;
        return c;
    }
    return std::wstring();
}