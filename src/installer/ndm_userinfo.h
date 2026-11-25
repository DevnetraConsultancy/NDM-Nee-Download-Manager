#pragma once
// ---------------------------------------------------------------------------
// The interactive user, for setup and the uninstaller (both run elevated).
//
// With "over-the-shoulder" elevation (a standard user types an
// administrator's credentials at the UAC prompt) the elevated process runs
// as that ADMINISTRATOR: its HKCU, %APPDATA% and desktop are the admin's,
// not those of the person who actually uses NDM. NDM itself only ever runs
// unelevated, so its per-user state (HKCU Run value, HKCU\Software\Devnetra\
// NDM, %APPDATA%\Devnetra\NDM, ...) belongs to the interactive user.
//
//  - Setup records that user's SID as HKLM\Software\Devnetra\NDM
//    "InstallUserSid" (NdmInteractiveUserSid: owner of the desktop's Explorer
//    process, else the user logged on to this session).
//  - The uninstaller reads it back (falling back to the same detection for
//    installs made before the value existed) and cleans that user's
//    registry through HKEY_USERS\<SID> when the hive is loaded (the user is
//    logged on, which is normal: they are uninstalling from their session).
//    Files are located through the profile path in HKLM ProfileList and the
//    user's shell-folder settings; anything that cannot be reached is
//    reported instead of guessed.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")   // delay-loaded (CMakeLists: ndm_delay_load)

#define NDM_INSTALL_USER_SID_VALUE L"InstallUserSid"

// ---- pure helpers (unit-tested) -------------------------------------------
// "S-1-<rev>-<auth>[-<sub>...]": digits separated by single dashes, at most
// 15 sub-authorities. Used before a SID string becomes part of a key path.
static inline bool NdmIsPlausibleSid(const std::wstring& s)
{
    if (s.size() < 5 || s.size() > 184 || s.compare(0, 4, L"S-1-") != 0) return false;
    int groups = 0;
    size_t i = 4;
    while (i < s.size()) {
        size_t start = i;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') i++;
        if (i == start || i - start > 10) return false;
        groups++;
        if (i == s.size()) break;
        if (s[i] != L'-') return false;
        i++;
        if (i == s.size()) return false;   // trailing dash
    }
    return groups >= 1 && groups <= 16;
}

// A shell-folder value of another user ("%USERPROFILE%\AppData\Roaming") with
// %USERPROFILE% replaced by `profile`. Any other environment variable cannot
// be expanded for another user: "" (the caller falls back). The result must
// be drive-absolute.
static inline std::wstring NdmExpandProfilePath(const std::wstring& value, const std::wstring& profile)
{
    static const wchar_t kVar[] = L"%USERPROFILE%";
    const size_t kLen = sizeof(kVar) / sizeof(kVar[0]) - 1;
    std::wstring out;
    for (size_t i = 0; i < value.size();) {
        if (value[i] == L'%') {
            if (value.size() - i >= kLen && _wcsnicmp(value.c_str() + i, kVar, kLen) == 0 && !profile.empty()) {
                out += profile;
                i += kLen;
                continue;
            }
            return std::wstring();
        }
        out += value[i++];
    }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    if (out.size() < 3 || out[1] != L':' || out[2] != L'\\') return std::wstring();
    return out;
}

// ---- token / account lookups ---------------------------------------------
static inline std::wstring NdmTokenUserSid(HANDLE tok)
{
    std::vector<BYTE> buf(256);
    DWORD n = 0;
    if (!GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &n)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || n == 0 || n > 64 * 1024) return std::wstring();
        buf.resize(n);
        if (!GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &n)) return std::wstring();
    }
    LPWSTR str = nullptr;
    std::wstring s;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str) && str) {
        s = str;
        LocalFree(str);
    }
    return s;
}

// SID of this process's user.
static inline std::wstring NdmSelfUserSid()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return std::wstring();
    std::wstring s = NdmTokenUserSid(tok);
    CloseHandle(tok);
    return s;
}

// The user at this session's desktop: the owner of the shell (Explorer)
// process, else the account logged on to this session (WTS + LSA lookup).
// "" if neither can be determined.
static inline std::wstring NdmInteractiveUserSid()
{
    std::wstring sid;
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (shell) GetWindowThreadProcessId(shell, &pid);
    if (pid) {
        if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            HANDLE tok = nullptr;
            if (OpenProcessToken(hp, TOKEN_QUERY, &tok)) {
                sid = NdmTokenUserSid(tok);
                CloseHandle(tok);
            }
            CloseHandle(hp);
        }
    }
    if (!sid.empty() && NdmIsPlausibleSid(sid)) return sid;
    sid.clear();

    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return sid;
    LPWSTR user = nullptr, domain = nullptr;
    DWORD ub = 0, db = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &ub) && user && user[0] &&
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSDomainName, &domain, &db) && domain)
    {
        std::wstring account = domain[0] ? std::wstring(domain) + L"\\" + user : std::wstring(user);
        BYTE sidBuf[SECURITY_MAX_SID_SIZE] = {};
        DWORD sidLen = sizeof(sidBuf);
        wchar_t dom[256] = {};
        DWORD domLen = 256;
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountNameW(nullptr, account.c_str(), sidBuf, &sidLen, dom, &domLen, &use) && use == SidTypeUser) {
            LPWSTR str = nullptr;
            if (ConvertSidToStringSidW(sidBuf, &str) && str) {
                sid = str;
                LocalFree(str);
            }
        }
    }
    if (user) WTSFreeMemory(user);
    if (domain) WTSFreeMemory(domain);
    return NdmIsPlausibleSid(sid) ? sid : std::wstring();
}

// Profile folder of `sid` from HKLM ProfileList (written by Windows; not
// user-controlled). "" if unknown.
static inline std::wstring NdmProfileDirOfSid(const std::wstring& sid)
{
    if (!NdmIsPlausibleSid(sid)) return std::wstring();
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + sid;
    wchar_t raw[MAX_PATH] = {};
    DWORD sz = sizeof(raw) - sizeof(wchar_t);
    // RRF_NOEXPAND: expanded below with this process's (machine) variables,
    // which is what %SystemDrive% in ProfileImagePath refers to.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProfileImagePath",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, raw, &sz) != ERROR_SUCCESS)
        return std::wstring();
    wchar_t exp[MAX_PATH] = {};
    DWORD n = ExpandEnvironmentStringsW(raw, exp, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return std::wstring();
    std::wstring p = exp;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (p.size() < 4 || p[1] != L':' || p[2] != L'\\' || p.find(L'%') != std::wstring::npos) return std::wstring();
    return p;
}
