// ---------------------------------------------------------------------------
// ndm_media.cpp — NDM_KIND_MEDIA downloads through yt-dlp (+ ffmpeg merge)
//
//  * yt-dlp.exe runs hidden (CREATE_NO_WINDOW) inside a job object that is
//    killed with NDM (KILL_ON_JOB_CLOSE) and on pause / remove.
//  * Progress comes from a --progress-template line per update, tagged with a
//    random per-run marker (a page cannot forge it) and tab separated; every
//    site-controlled field (format id, file names) is printed as JSON
//    (%(...)j). The approximate total of all requested streams comes from a
//    before_dl print, the final file path from --print-to-file after_move
//    into a private temp file (never parsed from stdout). Nothing outside
//    dl->savePath is ever renamed or marked.
//  * The exact partial / intermediate file names yt-dlp reports are recorded
//    in dl->mediaTemps; only those are deleted when the download is removed.
//  * Browser cookies are written to a temporary Netscape cookies.txt exactly
//    as the browser holds them (host-only, secure, HttpOnly, expiry) and
//    deleted afterwards; the plain "a=b; c=d" header is only a fallback
//    (host-only cookies for the page's host).
//  * Never overwrites: --no-overwrites and a free "<name> (N)" unless the
//    user confirmed "Replace". The finished file gets Mark-of-the-Web.
//  * Resume = run again with --continue (yt-dlp keeps .part / fragments).
//  * yt-dlp self-updates at most once per day into
//    %LOCALAPPDATA%\Devnetra\NDM\tools\ (Program Files is read-only); an
//    elevated NDM neither updates nor runs that copy (install dir only).
// ---------------------------------------------------------------------------
#include "ndm_media.h"

#include <winsock2.h>       // loopback playlist server (pair| downloads)
#include <ws2tcpip.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cwctype>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

// ===========================================================================
// Strings
// ===========================================================================
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// One argv element, quoted per the MSVC CRT / CommandLineToArgvW rules.
void AppendArg(std::wstring& cmd, const std::wstring& arg)
{
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++bs; }
        if (i == arg.size()) { cmd.append(bs * 2, L'\\'); break; }
        if (arg[i] == L'"')  { cmd.append(bs * 2 + 1, L'\\'); cmd += L'"'; }
        else                 { cmd.append(bs, L'\\');         cmd += arg[i]; }
    }
    cmd += L'"';
}

bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

UINT64 FileSize(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((UINT64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

UINT64 FileTime64(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((UINT64)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

std::wstring DirOf(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return DirOf(std::wstring(buf, n));
}

#ifdef NDM_DEV_BUILD
// <repo>\third_party for dev builds only: derived from this source file's path.
// Release builds never embed or search a build-machine path.
std::wstring RepoThirdParty()
{
    std::wstring f = Utf8ToWide(__FILE__);            // ...\src\media\ndm_media.cpp
    std::wstring d = DirOf(DirOf(DirOf(f)));
    return d.empty() ? std::wstring() : d + L"\\third_party";
}
#endif

// Elevated NDM must never run a binary from a user-writable folder.
bool Elevated()
{
    static const bool e = NdmIsElevated();
    return e;
}

std::wstring LocalToolsDir()
{
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)) || !buf[0])
        return std::wstring();
    return std::wstring(buf) + L"\\Devnetra\\NDM\\tools";
}

std::wstring BundledTool(const wchar_t* name)
{
    std::wstring exe = ExeDir();
    const std::wstring cands[] = {
        exe + L"\\tools\\" + name,
        exe + L"\\" + name,
#ifdef NDM_DEV_BUILD
        RepoThirdParty() + L"\\" + name,
#endif
    };
    for (const auto& c : cands)
        if (!c.empty() && c[0] != L'\\' && FileExists(c)) return c;
    return std::wstring();
}

// yt-dlp: the self-updated copy in %LOCALAPPDATA% wins over the bundled one
// (never when elevated: that folder is writable by unelevated code).
std::wstring YtdlpPath()
{
    std::wstring local = Elevated() ? std::wstring() : LocalToolsDir();
    if (!local.empty()) {
        local += L"\\yt-dlp.exe";
        if (FileSize(local) > 1024 * 1024) return local;
    }
    return BundledTool(L"yt-dlp.exe");
}

// ===========================================================================
// Child process with job object, line reader
// ===========================================================================
struct ToolResult
{
    bool  started  = false;
    bool  stopped  = false;    // stop event signalled
    bool  timedOut = false;
    DWORD exitCode = 0;
    DWORD winErr   = 0;
};

// Environment block = ours + UTF-8 I/O for the Python inside yt-dlp, plus
// `extra` ("NAME=value"; replaces a variable of that name, any case).
std::vector<wchar_t> BuildEnv(const std::vector<std::wstring>& extra = {})
{
    std::vector<std::wstring> add = { L"PYTHONIOENCODING=utf-8", L"PYTHONUTF8=1" };
    add.insert(add.end(), extra.begin(), extra.end());
    auto replaced = [&](const wchar_t* p) {
        for (const auto& a : add) {
            size_t eq = a.find(L'=');
            if (eq != std::wstring::npos && _wcsnicmp(p, a.c_str(), eq + 1) == 0) return true;
        }
        return false;
    };
    std::vector<wchar_t> env;
    LPWCH cur = GetEnvironmentStringsW();
    if (cur) {
        for (const wchar_t* p = cur; *p; p += wcslen(p) + 1) {
            if (replaced(p)) continue;
            env.insert(env.end(), p, p + wcslen(p) + 1);
        }
        FreeEnvironmentStringsW(cur);
    }
    for (const auto& a : add) {
        env.insert(env.end(), a.begin(), a.end());
        env.push_back(L'\0');
    }
    env.push_back(L'\0');
    return env;
}

// Terminate every process of the job below root (the Python child of the
// PyInstaller bootloader, ffmpeg, ...). The bootloader then exits by itself
// and removes its temporary _MEI folder; afterwards the job is killed.
void KillTree(HANDLE hJob, HANDLE hRoot, DWORD rootPid)
{
    for (int pass = 0; pass < 3; ++pass) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) break;
        std::vector<std::pair<DWORD, DWORD>> procs;      // pid, parent
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do procs.emplace_back(pe.th32ProcessID, pe.th32ParentProcessID);
            while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        std::vector<DWORD> tree = { rootPid };
        for (size_t i = 0; i < tree.size(); ++i)
            for (const auto& pp : procs)
                if (pp.second == tree[i] && pp.first != rootPid &&
                    std::find(tree.begin(), tree.end(), pp.first) == tree.end())
                    tree.push_back(pp.first);

        int killed = 0;
        for (size_t i = 1; i < tree.size(); ++i) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, tree[i]);
            if (!h) continue;
            BOOL inJob = FALSE;
            if (IsProcessInJob(h, hJob, &inJob) && inJob && TerminateProcess(h, 1)) ++killed;
            CloseHandle(h);
        }
        if (WaitForSingleObject(hRoot, killed ? 4000 : 200) == WAIT_OBJECT_0) break;
    }
    TerminateJobObject(hJob, 1);
    WaitForSingleObject(hRoot, 5000);
}

// Runs exe with args. Output lines (stdout, plus stderr when mergeErr) go to
// onLine; when captureOut is given, raw stdout is collected there instead and
// stderr lines go to onLine. extraEnv: see BuildEnv.
ToolResult RunTool(const std::wstring& exe, const std::vector<std::wstring>& args,
                   HANDLE hStop, DWORD timeoutMs,
                   const std::function<void(const std::string&)>& onLine,
                   std::string* captureOut,
                   const std::vector<std::wstring>& extraEnv = {})
{
    ToolResult res;
    std::wstring cmd;
    AppendArg(cmd, exe);
    for (const auto& a : args) AppendArg(cmd, a);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 64 * 1024)) { res.winErr = GetLastError(); return res; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (captureOut) {
        if (!CreatePipe(&errR, &errW, &sa, 64 * 1024)) {
            res.winErr = GetLastError();
            CloseHandle(outR); CloseHandle(outW);
            return res;
        }
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    // Inherit only these handles (other threads may be spawning concurrently).
    HANDLE inherit[3] = { nul, outW, captureOut ? errW : outW };
    int nInherit = captureOut ? 3 : 2;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool attrOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                            inherit, nInherit * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput  = nul;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError  = captureOut ? errW : outW;
    if (attrOk) si.lpAttributeList = attrs;

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    std::vector<wchar_t> env = BuildEnv(extraEnv);
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(L'\0');
    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                  (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    std::wstring dir = DirOf(exe);
    BOOL ok = CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, TRUE, flags,
                             env.data(), dir.empty() ? nullptr : dir.c_str(),
                             &si.StartupInfo, &pi);
    if (!ok) res.winErr = GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(outW);
    if (errW) CloseHandle(errW);
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        CloseHandle(outR);
        if (errR) CloseHandle(errR);
        if (hJob) CloseHandle(hJob);
        return res;
    }
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    res.started = true;

    std::mutex lineLock;
    auto reader = [&](HANDLE h, bool raw) {
        std::string buf;
        char chunk[16384];
        DWORD got = 0;
        while (ReadFile(h, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
            if (raw) { captureOut->append(chunk, got); continue; }
            buf.append(chunk, got);
            size_t start = 0;
            for (size_t i = 0; i < buf.size(); ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (i > start && onLine) {
                        std::lock_guard<std::mutex> g(lineLock);
                        onLine(buf.substr(start, i - start));
                    }
                    start = i + 1;
                }
            }
            buf.erase(0, start);
            if (buf.size() > 1024 * 1024) buf.clear();
        }
        if (!raw && !buf.empty() && onLine) {
            std::lock_guard<std::mutex> g(lineLock);
            onLine(buf);
        }
    };
    std::thread tOut(reader, outR, captureOut != nullptr);
    std::thread tErr;
    if (errR) tErr = std::thread(reader, errR, false);

    HANDLE waits[2] = { pi.hProcess, hStop };
    DWORD nWaits = hStop ? 2 : 1;
    DWORD w = WaitForMultipleObjects(nWaits, waits, FALSE, timeoutMs ? timeoutMs : INFINITE);
    if (w != WAIT_OBJECT_0) {
        if (w == WAIT_TIMEOUT) res.timedOut = true; else res.stopped = true;
        if (hJob) KillTree(hJob, pi.hProcess, pi.dwProcessId);
        else      TerminateProcess(pi.hProcess, 1);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &res.exitCode);
    // Grandchildren (ffmpeg) may still hold the pipe: the job kill closes them.
    if (hJob) TerminateJobObject(hJob, 1);

    tOut.join();
    if (tErr.joinable()) tErr.join();
    CloseHandle(outR);
    if (errR) CloseHandle(errR);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);
    return res;
}

// ===========================================================================
// Cookies -> temporary Netscape cookies.txt
// ===========================================================================
std::wstring HostOf(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return std::wstring();
    p += 3;
    size_t e = url.find_first_of(L"/?#", p);
    std::wstring h = url.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
    size_t at = h.find_last_of(L'@');
    if (at != std::wstring::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == L'[') return std::wstring();      // IPv6: skip
    size_t colon = h.find(L':');
    if (colon != std::wstring::npos) h = h.substr(0, colon);
    for (auto& c : h) c = (wchar_t)towlower(c);
    return h;
}

// Returns the temp file path, or empty when there is nothing to write.
// cookieJar (exact Netscape lines built from the browser's structured
// cookies: host-only / secure / HttpOnly / expiry preserved) is written as is.
// Without it the plain "a=b; c=d" header is the fallback: host-only cookies
// for the page's exact host, secure when the page is https.
std::wstring WriteCookieFile(const std::wstring& cookieJar, const std::wstring& cookieHeader,
                             const std::wstring& pageUrl)
{
    std::wstring host = HostOf(pageUrl);
    if ((cookieJar.empty() && cookieHeader.empty()) || host.empty()) return std::wstring();

    std::string body = "# Netscape HTTP Cookie File\n";
    int count = 0;
    if (!cookieJar.empty()) {
        // Re-validate line by line (7 tab-separated fields, no control chars).
        std::string jar = WideToUtf8(cookieJar);
        size_t s = 0;
        while (s < jar.size()) {
            size_t e = jar.find('\n', s);
            std::string line = jar.substr(s, e == std::string::npos ? std::string::npos : e - s);
            s = e == std::string::npos ? jar.size() : e + 1;
            if (line.empty()) continue;
            int tabs = 0;
            bool bad = false;
            for (unsigned char c : line) {
                if (c == '\t') ++tabs;
                else if (c < 0x20 || c == 0x7F) { bad = true; break; }
            }
            if (bad || tabs != 6) continue;
            if (line[0] == '#' && line.compare(0, 10, "#HttpOnly_") != 0) continue;
            body += line + "\n";
            ++count;
        }
    } else {
        const bool secure = _wcsnicmp(pageUrl.c_str(), L"https://", 8) == 0;
        size_t s = 0;
        while (s < cookieHeader.size()) {
            size_t e = cookieHeader.find(L';', s);
            std::wstring pair = Trim(cookieHeader.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s));
            s = (e == std::wstring::npos) ? cookieHeader.size() : e + 1;
            size_t eq = pair.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            std::wstring name = Trim(pair.substr(0, eq)), value = Trim(pair.substr(eq + 1));
            bool bad = false;
            for (wchar_t c : name + value) if (c < 0x20 || c == 0x7F) { bad = true; break; }
            if (bad) continue;
            body += WideToUtf8(host) + "\tFALSE\t/\t" + (secure ? "TRUE" : "FALSE") + "\t0\t" +
                    WideToUtf8(name) + "\t" + WideToUtf8(value) + "\n";
            ++count;
        }
    }
    if (!count) return std::wstring();

    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) return std::wstring();
    wchar_t name[64];
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    swprintf_s(name, L"ndm_ck_%lu_%llx.txt", GetCurrentProcessId(), (unsigned long long)qpc.QuadPart);
    std::wstring path = std::wstring(tmp) + name;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    DWORD wr = 0;
    BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok) { DeleteFileW(path.c_str()); return std::wstring(); }
    return path;
}

// Referer for yt-dlp. yt-dlp sends --referer with EVERY request it makes
// (page, API, CDN, fragments), so even a same-origin page URL would leak to
// other hosts: strict-origin-when-cross-origin relative to the page, and
// then never more than the origin ("https://site/").
std::wstring YtdlpReferer(const std::wstring& referrer, const std::wstring& target)
{
    std::wstring r = NdmReferrerFor(referrer, target);
    if (r.empty()) return r;
    size_t p = r.find(L"://");
    if (p == std::wstring::npos) return std::wstring();
    size_t e = r.find_first_of(L"/?#", p + 3);
    return (e == std::wstring::npos ? r : r.substr(0, e)) + L"/";
}

// Options common to probe and download. `target` = the page yt-dlp opens.
void AddCommonArgs(std::vector<std::wstring>& a, const std::wstring& cookieFile,
                   const std::wstring& ua, const std::wstring& referrerIn, const std::wstring& target)
{
    const std::wstring referrer = YtdlpReferer(referrerIn, target);
    a.insert(a.end(), { L"--ignore-config", L"--no-playlist", L"--encoding", L"utf-8",
                        L"--no-colors", L"--socket-timeout", L"20" });
    std::wstring deno = BundledTool(L"deno.exe");
    if (!deno.empty()) { a.push_back(L"--js-runtimes"); a.push_back(L"deno:" + deno); }
    if (!cookieFile.empty()) { a.push_back(L"--cookies"); a.push_back(cookieFile); }
    if (!ua.empty())         { a.push_back(L"--user-agent"); a.push_back(ua); }
    if (!referrer.empty())   { a.push_back(L"--referer"); a.push_back(referrer); }
}

// "ERROR: [youtube] abc: Video unavailable" -> "Video unavailable"
std::wstring CleanError(const std::string& line)
{
    std::wstring w = Utf8ToWide(line);
    if (w.compare(0, 7, L"ERROR: ") == 0) w = w.substr(7);
    if (!w.empty() && w[0] == L'[') {
        size_t rb = w.find(L"] ");
        if (rb != std::wstring::npos) {
            w = w.substr(rb + 2);
            size_t colon = w.find(L": ");
            if (colon != std::wstring::npos && colon < 40 && w.find(L' ') >= colon) w = w.substr(colon + 2);
        }
    }
    // Login hints ("... use --cookies-from-browser or --cookies ...") -> one short sentence.
    size_t ck = w.find(L"--cookies");
    if (ck != std::wstring::npos) {
        size_t dot = w.rfind(L". ", ck);
        w = (dot != std::wstring::npos ? w.substr(0, dot + 1) : std::wstring(L"This video needs a login."))
            + L" Sign in to the site in your browser, then try again.";
    }
    // Drop yt-dlp's long hints ("See https://...", "please report this issue").
    for (const wchar_t* cut : { L" Use --cookies", L". See  https://", L" See  https://", L"; please report this issue" }) {
        size_t c = w.find(cut);
        if (c != std::wstring::npos && c > 10) w = w.substr(0, c);
    }
    w = Trim(w);
    if (w.size() > 300) w = w.substr(0, 300) + L"...";
    return w;
}

const wchar_t* kMissingTool = L"Video downloader component (yt-dlp) is missing. Reinstall NDM.";

// ===========================================================================
// Daily yt-dlp self-update
// ===========================================================================
std::atomic<bool> g_updating { false };

void UpdateYtdlpWorker()
{
    std::wstring dir = LocalToolsDir();
    std::wstring bundled = BundledTool(L"yt-dlp.exe");
    if (dir.empty() || bundled.empty()) { g_updating = false; return; }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring local = dir + L"\\yt-dlp.exe";
    std::wstring stamp = dir + L"\\yt-dlp.lastupdate";

    // Stamp first so a failing update is not retried more than once a day.
    HANDLE h = CreateFileW(stamp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    // A newer NDM install may bring a newer bundled yt-dlp than our copy.
    if (FileSize(local) < 1024 * 1024 || FileTime64(bundled) > FileTime64(local)) {
        std::wstring tmp = local + L".new";
        if (CopyFileW(bundled.c_str(), tmp.c_str(), FALSE))
            if (!MoveFileExW(tmp.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING))
                DeleteFileW(tmp.c_str());
    }
    if (FileSize(local) > 1024 * 1024)
        RunTool(local, { L"-U", L"--ignore-config", L"--no-colors" }, nullptr, 180000, nullptr, nullptr);
    DeleteFileW((local + L".old").c_str());
    g_updating = false;
}

void MaybeUpdateYtdlp()
{
    if (GetEnvironmentVariableW(L"NDM_NO_YTDLP_UPDATE", nullptr, 0) > 0) return;
    if (Elevated()) return;                     // no self-update while elevated
    std::wstring dir = LocalToolsDir();
    if (dir.empty()) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    UINT64 n = ((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    UINT64 last = FileTime64(dir + L"\\yt-dlp.lastupdate");
    const UINT64 day = 24ULL * 3600ULL * 10000000ULL;
    if (last && n - last < day) return;
    bool expected = false;
    if (!g_updating.compare_exchange_strong(expected, true)) return;
    std::thread(UpdateYtdlpWorker).detach();
}

// ===========================================================================
// Progress parsing helpers
// ===========================================================================
double NumOrNeg(const std::string& t)
{
    if (t.empty() || t == "NA" || t == "None") return -1;
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    return (end && end != t.c_str()) ? v : -1;
}

void Store64(UINT64& field, UINT64 v)
{
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&field), (LONG64)v);
}

bool IsPartialRest(const std::wstring& rest) { return NdmMediaIsPartialRest(rest); }

// A finished file "<dir>\<stem>.<ext>" (anything that is not our partial) exists.
bool StemTaken(const std::wstring& dir, const std::wstring& stem)
{
    if (stem.empty() || stem.find_first_of(L"*?") != std::wstring::npos) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + stem + L".*").c_str(), &fd);
    bool taken = false;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fn = fd.cFileName;
            if (fn.size() <= stem.size() + 1 || _wcsnicmp(fn.c_str(), stem.c_str(), stem.size()) != 0 ||
                fn[stem.size()] != L'.')
                continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsPartialRest(fn.substr(stem.size() + 1))) {
                taken = true;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // "<stem>" itself without an extension also blocks the name
    return taken || GetFileAttributesW((dir + L"\\" + stem).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Lower-case names of the files in dir (to tell "created now" from "was there").
std::set<std::wstring> DirNames(const std::wstring& dir)
{
    std::set<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        for (auto& ch : n) ch = (wchar_t)towlower(ch);
        out.insert(n);
    } while (out.size() < 200000 && FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// yt-dlp output template: a literal '%' is "%%".
std::wstring EscapeTmpl(const std::wstring& s)
{
    std::wstring esc;
    for (wchar_t c : s) { esc += c; if (c == L'%') esc += L'%'; }
    return esc;
}

std::wstring RandomTag(int bytes)
{
    std::vector<unsigned char> b((size_t)bytes);
    if (BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (unsigned char)((q.QuadPart >> ((i % 8) * 8)) ^ (i * 131));
    }
    static const wchar_t* hx = L"0123456789abcdef";
    std::wstring s;
    for (unsigned char x : b) { s += hx[x >> 4]; s += hx[x & 15]; }
    return s;
}

// One JSON value printed by yt-dlp's %(...)j: a string -> its text, null /
// anything else -> "". ensure_ascii output: \uXXXX (with surrogate pairs).
std::wstring JsonStr(const std::string& t)
{
    std::string v = t;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return std::wstring();
    std::wstring out;
    std::string raw;                                  // pending UTF-8 bytes
    auto flush = [&]() { if (!raw.empty()) { out += Utf8ToWide(raw); raw.clear(); } };
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        char c = v[i];
        if (c != '\\') { raw += c; continue; }
        if (i + 2 >= v.size()) return std::wstring();
        char e = v[++i];
        flush();
        switch (e) {
        case '"': out += L'"'; break;
        case '\\': out += L'\\'; break;
        case '/': out += L'/'; break;
        case 'b': out += L'\b'; break;
        case 'f': out += L'\f'; break;
        case 'n': out += L'\n'; break;
        case 'r': out += L'\r'; break;
        case 't': out += L'\t'; break;
        case 'u': {
            if (i + 4 >= v.size()) return std::wstring();
            unsigned cp = 0;
            for (int k = 1; k <= 4; ++k) {
                char h = v[i + k];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else return std::wstring();
            }
            i += 4;
            out += (wchar_t)cp;
            break;
        }
        default: return std::wstring();
        }
    }
    flush();
    return out;
}

std::vector<std::string> SplitTab(const std::string& s)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find('\t', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::wstring LowerW(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Bare file name of `path` when its folder is exactly `dir` (after full-path
// normalisation), else "". yt-dlp never writes below the save folder.
std::wstring NameInDir(const std::wstring& path, const std::wstring& dir)
{
    if (path.empty() || dir.empty() || path.size() > 32000) return std::wstring();
    for (wchar_t c : path) if (c < 0x20) return std::wstring();
    if (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) return std::wstring();
    auto full = [](const std::wstring& p) {
        DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
        if (!n || n > 32768) return std::wstring();
        std::wstring b((size_t)n, L'\0');
        DWORD m = GetFullPathNameW(p.c_str(), n, &b[0], nullptr);
        if (!m || m >= n) return std::wstring();
        b.resize(m);
        return b;
    };
    std::wstring fp = full(path), fd = full(dir);
    if (fp.empty() || fd.empty()) return std::wstring();
    while (fd.size() > 3 && (fd.back() == L'\\' || fd.back() == L'/')) fd.pop_back();
    size_t sl = fp.find_last_of(L"\\/");
    if (sl == std::wstring::npos) return std::wstring();
    std::wstring parent = fp.substr(0, sl);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    if (LowerW(parent) != LowerW(fd)) return std::wstring();
    std::wstring name = fp.substr(sl + 1);
    if (name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"<>:\"|?*") != std::wstring::npos) return std::wstring();
    return name;
}

bool HasDeceptive(const std::wstring& s)
{
    for (wchar_t c : s) if (NdmIsDeceptiveChar(c)) return true;
    return false;
}

const wchar_t* const kMediaExts[] = {
    L"mp4", L"m4a", L"m4v", L"webm", L"mkv", L"mov", L"flv", L"f4v", L"3gp", L"ogg", L"ogv", L"oga",
    L"opus", L"mp3", L"aac", L"wav", L"flac", L"ts", L"mpg", L"mpeg", L"avi", L"wmv", L"weba", L"mp2",
    L"m2ts", L"vtt", L"srt", L"ass", L"mhtml", L"unknown_video" };

bool IsMediaExt(const std::wstring& e)
{
    for (const wchar_t* m : kMediaExts) if (e == m) return true;
    return false;
}
} // namespace

// ===========================================================================
// Public API
// ===========================================================================
std::wstring NdmMediaToolPath(const wchar_t* name)
{
    if (_wcsicmp(name, L"yt-dlp.exe") == 0) return YtdlpPath();
    return BundledTool(name);
}

bool NdmMediaIsPartialRest(const std::wstring& rest)
{
    std::wstring r = LowerW(rest);
    // strip one yt-dlp suffix: ".part", ".ytdl", ".part-FragN"
    bool suffix = false;
    auto cut = [&](const wchar_t* sfx) {
        size_t n = wcslen(sfx);
        if (r.size() > n && r.compare(r.size() - n, n, sfx) == 0) { r.resize(r.size() - n); return true; }
        return false;
    };
    size_t fr = r.rfind(L".part-frag");
    if (fr != std::wstring::npos && fr > 0 && fr + 10 < r.size() &&
        std::all_of(r.begin() + (std::ptrdiff_t)(fr + 10), r.end(), [](wchar_t c) { return iswdigit(c) != 0; })) {
        r.resize(fr);
        suffix = true;
    } else if (cut(L".part") || cut(L".ytdl")) {
        suffix = true;
    }
    if (r.empty()) return false;
    // "<ext>" (only with a suffix: "Lecture.mp4.part")
    if (r.find(L'.') == std::wstring::npos) return suffix && IsMediaExt(r);
    // "temp.<ext>" (ffmpeg merge / fixup output)
    if (r.compare(0, 5, L"temp.") == 0) return IsMediaExt(r.substr(5));
    // "f<format id>.<ext>"  (f137.mp4, f251-drc.webm, fhls-720p.mp4)
    size_t dot = r.find(L'.');
    if (r[0] == L'f' && dot > 1 && r.find(L'.', dot + 1) == std::wstring::npos) {
        std::wstring id = r.substr(1, dot - 1);
        bool okId = std::any_of(id.begin(), id.end(), [](wchar_t c) { return iswdigit(c) != 0; }) &&
                    std::all_of(id.begin(), id.end(), [](wchar_t c) {
                        return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                               c == L'-' || c == L'_' || c == L'=' || c == L'+';
                    });
        return okId && IsMediaExt(r.substr(dot + 1));
    }
    return false;
}

bool NdmMediaIsPartialName(const std::wstring& name)
{
    if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    for (size_t i = name.find(L'.'); i != std::wstring::npos; i = name.find(L'.', i + 1))
        if (i > 0 && i + 1 < name.size() && NdmMediaIsPartialRest(name.substr(i + 1))) return true;
    return false;
}

void NdmMediaDeleteTempFiles(const NdmDownload* dl)
{
    NdmHttpEngine::LockFields(false);
    std::wstring dir   = dl->savePath;
    std::wstring done  = dl->filename.c_str();
    std::wstring temps = dl->mediaTemps;
    NdmHttpEngine::UnlockFields(false);
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return;

    std::vector<std::wstring> names;
    for (size_t a = 0; a <= temps.size();) {
        size_t b = temps.find(L'|', a);
        if (b == std::wstring::npos) b = temps.size();
        if (b > a) names.push_back(temps.substr(a, b - a));
        a = b + 1;
    }
    // Entries from before this list existed: only "<name>.part" / ".ytdl".
    if (names.empty() && !done.empty()) { names.push_back(done + L".part"); names.push_back(done + L".ytdl"); }

    std::set<std::wstring> cands;
    for (const std::wstring& n : names) {
        if (n.empty() || n.size() > 255 || n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) continue;
        cands.insert(n);
        cands.insert(n + L".part");
        cands.insert(n + L".ytdl");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + n + L".part-Frag*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do cands.insert(fd.cFileName); while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (const std::wstring& c : cands) {
        if (!done.empty() && _wcsicmp(c.c_str(), done.c_str()) == 0) continue;   // never the finished file
        if (!NdmMediaIsPartialName(c)) continue;
        std::wstring full = dir + L"\\" + c;
        DWORD at = GetFileAttributesW(full.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || (at & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            continue;
        DeleteFileW(full.c_str());
    }
}

bool NdmMediaProbe(const NdmMediaProbeArgs& a, std::string& jsonOut, std::wstring& errorOut)
{
    jsonOut.clear();
    errorOut.clear();
    std::wstring ytdlp = YtdlpPath();
    if (ytdlp.empty()) { errorOut = kMissingTool; return false; }

    if (!NdmIsHttpUrl(a.pageUrl)) { errorOut = L"Only http and https pages can be read."; return false; }
    std::wstring cookieFile = WriteCookieFile(a.cookieJar, a.cookies, a.pageUrl);
    std::vector<std::wstring> args = { L"-J", L"--playlist-items", L"1", L"--no-warnings" };
    AddCommonArgs(args, cookieFile, a.userAgent, a.referrer, a.pageUrl);
    args.push_back(L"--");
    args.push_back(a.pageUrl);

    std::string lastErr;
    ToolResult r = RunTool(ytdlp, args, a.hAbort, a.timeoutMs,
                           [&](const std::string& line) {
                               if (line.compare(0, 6, "ERROR:") == 0) lastErr = line;
                           },
                           &jsonOut);
    if (!cookieFile.empty()) DeleteFileW(cookieFile.c_str());

    if (!r.started)  { errorOut = kMissingTool; return false; }
    if (r.timedOut)  { errorOut = L"The site took too long to answer."; return false; }
    if (r.stopped)   { errorOut = L"Cancelled."; return false; }
    size_t p = jsonOut.find_first_not_of(" \t\r\n");
    if (r.exitCode != 0 || p == std::string::npos || jsonOut[p] != '{') {
        errorOut = !lastErr.empty() ? CleanError(lastErr) : L"No downloadable video was found on this page.";
        jsonOut.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Joined HLS playlists: a video-only variant plus its audio rendition (the
// site's master playlist was not seen) -> a tiny master playlist that yt-dlp
// reads and merges like any video + audio.
//
// The master playlist is served by a one-shot loopback HTTP server, never
// as a local file: yt-dlp would need --enable-file-urls for that, which
// enables file:// for the WHOLE run, so a hostile remote variant playlist
// could name file:///C:/... segments or keys and have local files read into
// the output. The server
//   * listens on 127.0.0.1 only, on an ephemeral port,
//   * answers exactly one path, /<32 random hex>.m3u8 (404 for anything
//     else, 405 for any method but GET, 400 for a malformed request),
//   * serves application/vnd.apple.mpegurl, Connection: close,
//   * lives only while that yt-dlp run lives (started right before it,
//     stopped right after it; no file is written).
// ---------------------------------------------------------------------------
static bool SafePlaylistUrl(const std::wstring& u)
{
    if (u.size() > 2048 || !NdmIsHttpUrl(u)) return false;
    for (wchar_t c : u)
        if (c <= 0x20 || c == 0x7F || c == L'"' || c == L'\\' || (c >= 0x80 && c < 0xA0)) return false;
    return true;
}

// The master playlist text for `video` (video-only variant) + `audio`.
static std::string JoinedPlaylistBody(const std::wstring& video, const std::wstring& audio)
{
    // CODECS tells yt-dlp the variant is video only (otherwise it counts the
    // variant as video + audio and leaves the audio out); the family comes
    // from the address ("/avc1/", "/hvc1/", ...), the exact profile does not matter.
    const std::string v = WideToUtf8(video);
    std::string lv = v, codecs = "avc1.640028", res;
    for (auto& ch : lv) ch = (char)tolower((unsigned char)ch);
    if (lv.find("/hvc1/") != std::string::npos || lv.find("/hev1/") != std::string::npos) codecs = "hvc1.1.6.L120.90";
    else if (lv.find("/vp09/") != std::string::npos || lv.find("/vp9/") != std::string::npos) codecs = "vp09.00.40.08";
    else if (lv.find("/av01/") != std::string::npos) codecs = "av01.0.08M.08";
    for (size_t i = 0; i < v.size(); ++i) {                    // "/1280x720/" -> RESOLUTION=1280x720
        if (v[i] != '/') continue;
        size_t j = i + 1;
        const size_t w0 = j;
        while (j < v.size() && isdigit((unsigned char)v[j])) ++j;
        if (j - w0 < 3 || j - w0 > 4 || j >= v.size() || v[j] != 'x') continue;
        const size_t h0 = ++j;
        while (j < v.size() && isdigit((unsigned char)v[j])) ++j;
        if (j - h0 >= 3 && j - h0 <= 4 && j < v.size() && v[j] == '/') {
            res = "RESOLUTION=" + v.substr(w0, j - w0) + ",";
            break;
        }
    }
    std::string body =
        "#EXTM3U\n#EXT-X-INDEPENDENT-SEGMENTS\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\"" +
        WideToUtf8(audio) + "\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"" + codecs + ",mp4a.40.2\"," + res + "AUDIO=\"aud\"\n" +
        v + "\n";
    return body;
}

class PlaylistServer
{
public:
    PlaylistServer(std::string body, const std::wstring& token)
        : m_body(std::move(body)), m_path("/" + WideToUtf8(token) + ".m3u8") {}
    ~PlaylistServer() { Stop(); }
    PlaylistServer(const PlaylistServer&) = delete;
    PlaylistServer& operator=(const PlaylistServer&) = delete;

    bool Start()
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        m_wsa = true;
        // Not inheritable: yt-dlp (and anything else started meanwhile) never gets it.
        m_ls = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (m_ls == INVALID_SOCKET) { Stop(); return false; }
        BOOL excl = TRUE;   // nobody can bind the same port beside us
        setsockopt(m_ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
        sockaddr_in a = {};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;   // ephemeral
        int al = sizeof(a);
        if (bind(m_ls, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 || listen(m_ls, 8) != 0 ||
            getsockname(m_ls, reinterpret_cast<sockaddr*>(&a), &al) != 0)
        {
            Stop();
            return false;
        }
        m_port = ntohs(a.sin_port);
        try { m_thread = std::thread(&PlaylistServer::Loop, this); }
        catch (...) { Stop(); return false; }
        return true;
    }

    void Stop()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        if (m_ls != INVALID_SOCKET) { closesocket(m_ls); m_ls = INVALID_SOCKET; }
        if (m_wsa) { WSACleanup(); m_wsa = false; }
    }

    std::wstring Url() const
    {
        return L"http://127.0.0.1:" + std::to_wstring(m_port) + Utf8ToWide(m_path);
    }

private:
    void Loop()
    {
        while (!m_stop) {
            WSAPOLLFD pfd = {};
            pfd.fd     = m_ls;
            pfd.events = POLLRDNORM;
            int n = WSAPoll(&pfd, 1, 200);
            if (m_stop || n == SOCKET_ERROR) break;
            if (n == 0) continue;
            sockaddr_in peer = {};
            int pl = sizeof(peer);
            SOCKET c = accept(m_ls, reinterpret_cast<sockaddr*>(&peer), &pl);
            if (c == INVALID_SOCKET) continue;
            if ((ntohl(peer.sin_addr.s_addr) >> 24) == 127) Serve(c);
            closesocket(c);
        }
    }

    void Serve(SOCKET c)
    {
        DWORD to = 3000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        std::string req;
        char buf[2048];
        while (req.size() < 16384 && req.find("\r\n\r\n") == std::string::npos && !m_stop) {
            int got = recv(c, buf, sizeof(buf), 0);
            if (got <= 0) break;
            req.append(buf, (size_t)got);
        }
        size_t le = req.find("\r\n");
        size_t s1 = le == std::string::npos ? std::string::npos : req.find(' ');
        size_t s2 = (s1 == std::string::npos || s1 > le) ? std::string::npos : req.find(' ', s1 + 1);
        if (req.find("\r\n\r\n") == std::string::npos || s2 == std::string::npos || s2 > le ||
            req.compare(s2 + 1, 5, "HTTP/") != 0)
            return Reply(c, "400 Bad Request", nullptr);
        const std::string method = req.substr(0, s1), target = req.substr(s1 + 1, s2 - s1 - 1);
        if (method != "GET") return Reply(c, "405 Method Not Allowed", nullptr, "Allow: GET\r\n");
        if (target != m_path) return Reply(c, "404 Not Found", nullptr);
        Reply(c, "200 OK", &m_body);
    }

    static void Reply(SOCKET c, const char* status, const std::string* body, const char* extra = "")
    {
        const std::string content = body ? *body : std::string();
        std::string r = std::string("HTTP/1.1 ") + status + "\r\n" +
                        (body ? "Content-Type: application/vnd.apple.mpegurl\r\n" : "Content-Type: text/plain\r\n") +
                        "Content-Length: " + std::to_string(content.size()) + "\r\n"
                        "Cache-Control: no-store\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n" + extra + "\r\n" + content;
        size_t off = 0;
        while (off < r.size()) {
            int sent = send(c, r.data() + off, (int)std::min<size_t>(r.size() - off, 65536), 0);
            if (sent <= 0) break;
            off += (size_t)sent;
        }
        shutdown(c, SD_SEND);
    }

    std::string       m_body, m_path;
    SOCKET            m_ls   = INVALID_SOCKET;
    USHORT            m_port = 0;
    bool              m_wsa  = false;
    std::atomic<bool> m_stop { false };
    std::thread       m_thread;
};

// yt-dlp (Python urllib) takes proxies from *_PROXY environment variables,
// else from the WinINet settings (HKCU ... Internet Settings, ProxyEnable /
// ProxyServer); NO_PROXY is honoured only in the first case. So that the
// loopback playlist never goes to a proxy (which cannot reach our
// 127.0.0.1), the pair run gets NO_PROXY += 127.0.0.1 and, when the proxy
// came from the registry, that proxy as HTTP(S)_PROXY (parsed the way
// urllib does) so it keeps applying to the real media requests. Nothing is
// added when no proxy is configured.
static std::vector<std::wstring> LoopbackProxyEnv()
{
    std::vector<std::wstring> env;
    bool envProxy = false;
    std::wstring noProxy;
    if (LPWCH cur = GetEnvironmentStringsW()) {
        for (const wchar_t* p = cur; *p; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p + 1, L'=');
            if (!eq || !eq[1]) continue;
            std::wstring name(p, eq);
            for (auto& ch : name) ch = (wchar_t)towlower(ch);
            if (name.size() < 6 || name.compare(name.size() - 6, 6, L"_proxy") != 0) continue;
            if (name == L"no_proxy") noProxy = eq + 1;
            else envProxy = true;
        }
        FreeEnvironmentStringsW(cur);
    }
    if (!envProxy) {
        const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
        DWORD enable = 0, sz = sizeof(enable);
        wchar_t server[1024] = {};
        DWORD ssz = sizeof(server) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"ProxyEnable", RRF_RT_REG_DWORD, nullptr, &enable, &sz) == ERROR_SUCCESS &&
            enable &&
            RegGetValueW(HKEY_CURRENT_USER, key, L"ProxyServer", RRF_RT_REG_SZ, nullptr, server, &ssz) == ERROR_SUCCESS &&
            server[0])
        {
            std::wstring srv = Trim(server), http, https, socks;
            auto withScheme = [](const std::wstring& addr, const wchar_t* scheme) {
                return addr.find(L"://") != std::wstring::npos ? addr : std::wstring(scheme) + addr;
            };
            if (srv.find_first_of(L"=;") == std::wstring::npos) {
                http = https = withScheme(srv, L"http://");
            } else {
                size_t pos = 0;
                while (pos <= srv.size()) {
                    size_t e = srv.find(L';', pos);
                    if (e == std::wstring::npos) e = srv.size();
                    std::wstring part = Trim(srv.substr(pos, e - pos));
                    pos = e + 1;
                    size_t q = part.find(L'=');
                    if (q == std::wstring::npos || q + 1 >= part.size()) continue;
                    std::wstring proto = part.substr(0, q), addr = Trim(part.substr(q + 1));
                    for (auto& ch : proto) ch = (wchar_t)towlower(ch);
                    if (proto == L"http")       http  = withScheme(addr, L"http://");
                    else if (proto == L"https") https = withScheme(addr, L"http://");
                    else if (proto == L"socks") socks = withScheme(addr, L"socks4://");
                }
                if (!socks.empty()) {
                    if (http.empty())  http  = socks;
                    if (https.empty()) https = socks;
                }
            }
            if (!http.empty())  env.push_back(L"HTTP_PROXY=" + http);
            if (!https.empty()) env.push_back(L"HTTPS_PROXY=" + https);
        }
    }
    if (envProxy || !env.empty())
        env.push_back(L"NO_PROXY=" + (noProxy.empty() ? std::wstring() : noProxy + L",") + L"127.0.0.1");
    return env;
}

NdmRunResult NdmMediaRun(NdmDownload* dl, const NdmRunOptions& opt)
{
    if (!dl->hStopEvent) dl->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto finish = [&](NdmRunResult r) {
        dl->speed = 0;
        if (opt.setStatus)
            dl->status = r == NDM_RUN_COMPLETED ? NDM_STATUS_COMPLETED
                       : r == NDM_RUN_STOPPED   ? NDM_STATUS_PAUSED : NDM_STATUS_ERROR;
        return r;
    };
    auto fail = [&](const std::wstring& msg) {
        NdmHttpEngine::SetSharedString(dl->errorMsg, msg);
        return finish(NDM_RUN_ERROR);
    };
    if (opt.setStatus) dl->status = NDM_STATUS_DOWNLOADING;
    NdmHttpEngine::SetSharedString(dl->errorMsg, L"");
    if (WaitForSingleObject(dl->hStopEvent, 0) == WAIT_OBJECT_0) return finish(NDM_RUN_STOPPED);

    std::wstring ytdlp = YtdlpPath();
    if (ytdlp.empty()) return fail(kMissingTool);
    MaybeUpdateYtdlp();

    NdmHttpEngine::LockFields(false);
    std::wstring source   = !dl->pageUrl.empty() ? dl->pageUrl : dl->url;
    std::wstring format   = dl->mediaFormat;
    std::wstring savePath = dl->savePath;
    std::wstring cookies  = dl->cookies;
    std::wstring jar      = dl->cookieJar;
    std::wstring ua       = dl->userAgent;
    std::wstring referrer = dl->referrer;
    const bool   replace  = dl->overwriteConfirmed || opt.overwriteConfirmed;
    NdmHttpEngine::UnlockFields(false);
    std::wstring filename = NdmHttpEngine::GetSharedString(dl->filename);
    if (!NdmIsHttpUrl(source)) return fail(L"Only http and https pages can be downloaded as video.");
    if (!referrer.empty() && !NdmIsHttpUrl(referrer)) referrer.clear();
    // "pair|<audio playlist>|<selector>": source is a video-only HLS variant.
    std::wstring pairAudio;
    if (format.compare(0, 5, L"pair|") == 0) {
        size_t bar = format.find(L'|', 5);
        if (bar == std::wstring::npos) return fail(L"Invalid video format.");
        pairAudio = format.substr(5, bar - 5);
        format = format.substr(bar + 1);
        if (format.compare(0, 2, L"v=") == 0) {          // the resolution picked from the list
            size_t vb = format.find(L'|');
            if (vb == std::wstring::npos) return fail(L"Invalid video format.");
            source = format.substr(2, vb - 2);
            format = format.substr(vb + 1);
        }
        if (!SafePlaylistUrl(pairAudio) || !SafePlaylistUrl(source)) return fail(L"Invalid video address.");
    }
    // "x:m4a|<selector>" / "x:mp3|<selector>" / "x:mp3:<kbps>|<selector>":
    // audio extraction (-x). m4a copies an AAC source (re-encodes anything
    // else); mp3 encodes, at <kbps> CBR when given (a standard rate), else
    // at yt-dlp's best VBR. Any other "x:" prefix is not ours: plain best
    // audio, no conversion.
    std::wstring extract, audioQuality = L"0";
    if (format.compare(0, 2, L"x:") == 0) {
        size_t bar = format.find(L'|');
        std::wstring codec = bar == std::wstring::npos ? std::wstring() : format.substr(2, bar - 2);
        if (codec.compare(0, 4, L"mp3:") == 0) {
            static const wchar_t* kRates[] = { L"320", L"256", L"192", L"160", L"128", L"96", L"64", L"48", L"32" };
            const std::wstring rate = codec.substr(4);
            for (const wchar_t* k : kRates) if (rate == k) audioQuality = rate + L"K";
            codec = L"mp3";                              // an odd rate: still an MP3, best VBR
        }
        if (codec == L"m4a" || codec == L"mp3") extract = codec;
        format = (bar == std::wstring::npos || !extract.size()) ? std::wstring() : format.substr(bar + 1);
        if (format.empty()) format = L"ba/b";
    }
    if (format.empty()) format = L"bv*+ba/b";
    while (!savePath.empty() && (savePath.back() == L'\\' || savePath.back() == L'/')) savePath.pop_back();
    if (savePath.empty()) return fail(L"No download folder.");
    int mk = SHCreateDirectoryExW(nullptr, savePath.c_str(), nullptr);
    if (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS)
        return fail(L"Cannot create the download folder: " + NdmHttpEngine::ErrorText((DWORD)mk));

    // Output template: "<name without extension>.%(ext)s" (yt-dlp picks mp4/mkv/m4a).
    std::wstring stem, ext0;
    if (opt.userNamed || (!filename.empty() && filename != NdmSafeFilename(source))) {
        stem = NdmHttpEngine::SanitizeFilename(filename);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0 && stem.size() - dot <= 6) {
            ext0 = stem.substr(dot);
            stem = stem.substr(0, dot);
        }
    }
    if (stem == L"download") stem.clear();

    // Never overwrite: a finished file "<stem>.*" (not one of our own partial
    // files) -> "<stem> (2)", "<stem> (3)", ... The pick is stored in
    // dl->filename so a resume keeps using the same partial files.
    if (!stem.empty() && !replace) {
        std::wstring base = stem, pick = stem;
        for (int k = 2; k < 10000 && StemTaken(savePath, pick); ++k)
            pick = base + L" (" + std::to_wstring(k) + L")";
        if (pick != stem) {
            stem = pick;
            NdmHttpEngine::SetSharedString(dl->filename, stem + (ext0.empty() ? L".mp4" : ext0));
            if (opt.onChange) opt.onChange(dl, opt.onChangeCtx);
        }
    }

    // pair| only: the joined master playlist, served over loopback per run.
    std::string joinedBody;
    std::wstring joinedToken;
    std::vector<std::wstring> joinedEnv;
    if (!pairAudio.empty()) {
        joinedBody  = JoinedPlaylistBody(source, pairAudio);
        joinedToken = RandomTag(16);                 // 32 hex characters
        joinedEnv   = LoopbackProxyEnv();
    }
    std::wstring cookieFile = WriteCookieFile(jar, cookies, source);
    std::wstring ffmpeg = BundledTool(L"ffmpeg.exe");

    // Per-stream progress (video and audio are fetched one after another).
    std::map<std::string, std::pair<UINT64, UINT64>> streams;
    UINT64      expected = 0;
    std::string lastErr;
    std::wstring finalPath, existingPath;
    bool anyBytes = false;              // some stream really transferred data this run

    const bool debug = GetEnvironmentVariableW(L"NDM_MEDIA_DEBUG", nullptr, 0) > 0;
    // Random per-run marker: lines yt-dlp prints for us start with it; text
    // a site controls (titles, error messages) cannot know it.
    const std::wstring tag = L"NDM" + RandomTag(12);
    const std::string  tagP = WideToUtf8(tag) + "P\t", tagI = WideToUtf8(tag) + "I\t";

    // Record a partial / intermediate name yt-dlp reported (bare name in savePath).
    auto recordTemp = [&](const std::wstring& name) {
        if (name.empty() || !NdmMediaIsPartialName(name)) return;
        bool added = false;
        NdmHttpEngine::LockFields(true);
        std::wstring& list = dl->mediaTemps;
        std::wstring key = L"|" + LowerW(list) + L"|";
        if (key.find(L"|" + LowerW(name) + L"|") == std::wstring::npos &&
            std::count(list.begin(), list.end(), L'|') < 63 && list.size() + name.size() < 16000) {
            if (!list.empty()) list += L'|';
            list += name;
            added = true;
        }
        NdmHttpEngine::UnlockFields(true);
        if (added && opt.onChange) opt.onChange(dl, opt.onChangeCtx);
    };
    auto recordFromPath = [&](const std::wstring& path) {
        std::wstring nm = NameInDir(path, savePath);
        if (nm.empty()) return;
        recordTemp(nm);
        // "<S>.f137.mp4" is merged by ffmpeg through "<S>.temp.mp4|mkv"
        size_t d1 = nm.find_last_of(L'.');
        size_t d0 = d1 == std::wstring::npos || d1 == 0 ? std::wstring::npos : nm.find_last_of(L'.', d1 - 1);
        if (d0 != std::wstring::npos && d0 > 0 && d1 > d0 + 1 && (nm[d0 + 1] == L'f' || nm[d0 + 1] == L'F') &&
            NdmMediaIsPartialRest(nm.substr(d0 + 1))) {
            std::wstring base = nm.substr(0, d0);
            for (const wchar_t* e : { L".temp.mp4", L".temp.mkv" }) recordTemp(base + e);
        }
    };

    auto onLine = [&](const std::string& line) {
        if (debug) fprintf(stderr, "[yt-dlp] %s\n", line.c_str());
        if (line.compare(0, tagP.size(), tagP) == 0) {
            std::vector<std::string> t = SplitTab(line.substr(tagP.size()));
            if (t.size() < 5) return;
            double got = NumOrNeg(t[1]), tot = NumOrNeg(t[2]), est = NumOrNeg(t[3]), spd = NumOrNeg(t[4]);
            std::string fid = t.size() >= 6 ? WideToUtf8(JsonStr(t[5])) : "";
            if (t.size() >= 7) recordFromPath(JsonStr(t[6]));
            if (t.size() >= 8) recordFromPath(JsonStr(t[7]));
            auto& st = streams[fid];
            if (got >= 0) { st.first = (UINT64)got; anyBytes = true; }
            if (tot > 0) st.second = (UINT64)tot; else if (est > 0) st.second = (UINT64)est;
            if (t[0] == "finished" && st.second < st.first) st.second = st.first;
            UINT64 sumGot = 0, sumTot = 0;
            for (const auto& kv : streams) { sumGot += kv.second.first; sumTot += std::max(kv.second.first, kv.second.second); }
            UINT64 total = std::max(expected, sumTot);
            Store64(dl->downloadedSize, total ? std::min(sumGot, total) : sumGot);
            Store64(dl->totalSize, total);
            dl->speed = (spd > 0 && spd < 4.0e9) ? (DWORD)spd : 0;
        } else if (line.compare(0, tagI.size(), tagI) == 0) {
            double v = NumOrNeg(line.substr(tagI.size()));
            if (v > 0) {
                expected = (UINT64)v;
                if (NdmHttpEngine::AtomicRead64(&dl->totalSize) < expected) Store64(dl->totalSize, expected);
            }
        } else if (line.compare(0, 6, "ERROR:") == 0) {
            lastErr = line;
        } else if (line.compare(0, 11, "[download] ") == 0) {
            // --no-overwrites: "[download] <path> has already been downloaded"
            static const char kHave[] = " has already been downloaded";
            size_t p = line.find(kHave);
            if (p != std::string::npos && p > 11) {
                std::wstring ep = Trim(Utf8ToWide(line.substr(11, p - 11)));
                std::wstring nm = NameInDir(ep, savePath);
                // a finished stream of our own interrupted run ("<stem>.f137.mp4") is fine
                bool own = !stem.empty() && nm.size() > stem.size() + 1 &&
                           _wcsnicmp(nm.c_str(), stem.c_str(), stem.size()) == 0 && nm[stem.size()] == L'.' &&
                           IsPartialRest(nm.substr(stem.size() + 1));
                if (!own && !nm.empty()) existingPath = savePath + L"\\" + nm;
            }
        }
    };

    // Final path: --print-to-file into a private temp file (not stdout).
    std::wstring printFile;
    {
        wchar_t tmp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp)) printFile = std::wstring(tmp) + L"ndm_mf_" + RandomTag(8) + L".txt";
    }
    auto readFinal = [&]() -> std::wstring {
        if (printFile.empty()) return std::wstring();
        HANDLE h = CreateFileW(printFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::wstring();
        std::string data;
        char buf[8192];
        DWORD got = 0;
        while (data.size() < 1024 * 1024 && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) data.append(buf, got);
        CloseHandle(h);
        DeleteFileW(printFile.c_str());
        // last non-empty line
        size_t e = data.find_last_not_of("\r\n");
        if (e == std::string::npos) return std::wstring();
        size_t b = data.find_last_of("\r\n", e);
        std::wstring line = Utf8ToWide(data.substr(b == std::string::npos ? 0 : b + 1, e + 1 - (b == std::string::npos ? 0 : b + 1)));
        std::wstring nm = NameInDir(line, savePath);
        return nm.empty() ? std::wstring() : savePath + L"\\" + nm;
    };

    // A list entry promised MP4 ("1080p MP4", name "<title>.mp4"): merge into
    // MP4 even when yt-dlp does not know a codec (X's HLS audio has no codec
    // info, so "mp4/mkv" would pick MKV for H.264 + AAC). If ffmpeg cannot
    // put the streams into MP4, run again with the "mp4/mkv" choice (the
    // downloaded streams are reused).
    bool forceMp4 = extract.empty() && _wcsicmp(ext0.c_str(), L".mp4") == 0;

    ToolResult r;
    bool joinedFailed = false;           // the loopback playlist server could not start
    for (int attempt = 0; attempt < 4; ++attempt) {
        // This run's share of the bandwidth (global limit / priority /
        // per-download limit). yt-dlp gets it as -r when it starts and cannot
        // change it while running: the flow is pinned to that rate.
        NdmFlowScope flow(dl, NDM_KIND_MEDIA);
        std::wstring outTmpl = savePath + L"\\" + (stem.empty() ? std::wstring(L"%(title).150B") : EscapeTmpl(stem)) +
                               L".%(ext)s";
        std::vector<std::wstring> args = {
            L"--newline", L"--progress", L"--no-mtime", L"--continue", L"--no-simulate",
            L"--windows-filenames", L"--retries", L"10", L"--fragment-retries", L"10",
            // The entry the quality list described (the probe reads item 1 too):
            // a post with several videos never downloads all of them under one
            // name. "/status/ID/video/N" pages are a single entry already.
            L"--playlist-items", L"1",
            replace ? L"--force-overwrites" : L"--no-overwrites",
            L"--no-post-overwrites",
            L"-f", format,
            L"-S", L"res,fps,vcodec:h264,acodec:aac,channels:2",
            L"--merge-output-format", forceMp4 ? L"mp4" : L"mp4/mkv",
            L"-N", std::to_wstring(std::max(1, std::min(16, dl->numConnections))),
            L"--http-chunk-size", L"10M",
            L"-o", outTmpl,
            L"--progress-template",
            L"download:" + tag + L"P\t%(progress.status)s\t%(progress.downloaded_bytes)s\t%(progress.total_bytes)s\t"
            L"%(progress.total_bytes_estimate)s\t%(progress.speed)s\t%(info.format_id)j\t"
            L"%(progress.filename)j\t%(progress.tmpfilename)j",
            L"--print", L"before_dl:" + tag + L"I\t%(filesize,filesize_approx)s",
        };
        if (!printFile.empty()) {
            DeleteFileW(printFile.c_str());
            args.push_back(L"--print-to-file");
            args.push_back(L"after_move:%(filepath)s");
            args.push_back(EscapeTmpl(printFile));
        }
        // The folder, not the exe: the shared ffmpeg build needs its av*.dll next to it,
        // and yt-dlp then also finds ffprobe there.
        if (!ffmpeg.empty()) { args.push_back(L"--ffmpeg-location"); args.push_back(DirOf(ffmpeg)); }
        if (!extract.empty()) {
            args.insert(args.end(), { L"-x", L"--audio-format", extract, L"--audio-quality", audioQuality });
        }
        DWORD limit = flow.Rate();
        NdmHttpEngine::SetFlowFixedRate(flow.get(), limit);
        if (limit > 0) { args.push_back(L"-r"); args.push_back(std::to_wstring(limit)); }
        AddCommonArgs(args, cookieFile, ua, referrer, source);
        std::unique_ptr<PlaylistServer> joined;       // lives exactly as long as this yt-dlp run
        if (!joinedBody.empty()) {
            joined.reset(new PlaylistServer(joinedBody, joinedToken));
            if (!joined->Start()) { r = ToolResult(); r.started = true; r.exitCode = 1; joinedFailed = true; break; }
        }
        args.push_back(L"--");
        args.push_back(joined ? joined->Url() : source);

        finalPath.clear();
        existingPath.clear();
        lastErr.clear();
        anyBytes = false;
        const std::set<std::wstring> before = replace ? std::set<std::wstring>() : DirNames(savePath);
        r = RunTool(ytdlp, args, dl->hStopEvent, 0, onLine, nullptr, joined ? joinedEnv : std::vector<std::wstring>());
        if (joined) joined->Stop();
        finalPath = readFinal();
        // --print makes yt-dlp quiet, so "has already been downloaded" is not
        // printed: a run that moved no bytes and "produced" a file that was
        // already there means yt-dlp skipped an existing file.
        if (!replace && existingPath.empty() && !anyBytes && !finalPath.empty()) {
            size_t sl = finalPath.find_last_of(L"\\/");
            std::wstring nm = sl == std::wstring::npos ? finalPath : finalPath.substr(sl + 1);
            for (auto& ch : nm) ch = (wchar_t)towlower(ch);
            if (before.count(nm)) existingPath = finalPath;
        }
        if (forceMp4 && r.started && !r.stopped && r.exitCode != 0 &&
            WaitForSingleObject(dl->hStopEvent, 0) != WAIT_OBJECT_0) {
            std::string le = lastErr;
            for (auto& ch : le) ch = (char)tolower((unsigned char)ch);
            if (le.find("ffmpeg") != std::string::npos || le.find("postprocess") != std::string::npos ||
                le.find("merg") != std::string::npos || le.find("conversion failed") != std::string::npos) {
                forceMp4 = false;
                continue;
            }
        }
        if (!r.started || r.stopped || r.exitCode != 0 || existingPath.empty() || replace) break;

        // A finished file of that name already exists (title template, or it
        // appeared meanwhile): yt-dlp skipped it. Retry under a free name.
        size_t slash = existingPath.find_last_of(L"\\/");
        std::wstring nm = slash == std::wstring::npos ? existingPath : existingPath.substr(slash + 1);
        size_t dot = nm.find_last_of(L'.');
        std::wstring base = (dot != std::wstring::npos && dot > 0) ? nm.substr(0, dot) : nm;
        if (base.empty()) break;
        ext0 = (dot != std::wstring::npos && dot > 0) ? nm.substr(dot) : std::wstring(L".mp4");
        std::wstring pick = base;
        for (int k = 2; k < 10000 && StemTaken(savePath, pick); ++k)
            pick = base + L" (" + std::to_wstring(k) + L")";
        if (pick == base) break;                       // nothing to dodge after all
        stem = pick;
        NdmHttpEngine::SetSharedString(dl->filename, stem + ext0);
        if (opt.onChange) opt.onChange(dl, opt.onChangeCtx);
        streams.clear();
        expected = 0;
    }
    if (!cookieFile.empty()) DeleteFileW(cookieFile.c_str());
    if (!printFile.empty()) DeleteFileW(printFile.c_str());

    if (joinedFailed) return fail(L"Cannot start the local playlist helper (network error).");
    if (!r.started) return fail(kMissingTool);
    if (r.stopped)  return finish(NDM_RUN_STOPPED);
    if (WaitForSingleObject(dl->hStopEvent, 0) == WAIT_OBJECT_0) return finish(NDM_RUN_STOPPED);
    if (r.exitCode != 0) {
        if (!lastErr.empty()) return fail(CleanError(lastErr));
        wchar_t buf[96];
        swprintf_s(buf, L"Video download failed (yt-dlp exit code %lu).", r.exitCode);
        return fail(buf);
    }
    if (!existingPath.empty() && !replace)
        return fail(L"A file with this name already exists in the download folder.");

    if (!finalPath.empty() && FileExists(finalPath)) {
        size_t slash = finalPath.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? finalPath : finalPath.substr(slash + 1);
        // A title may carry bidi / invisible characters ("clip\u202Egpj.exe"):
        // rename to the sanitised name (never over an existing file).
        if (HasDeceptive(name)) {
            std::wstring clean = NdmHttpEngine::SanitizeFilename(name);
            size_t dot = clean.find_last_of(L'.');
            std::wstring b = (dot != std::wstring::npos && dot > 0) ? clean.substr(0, dot) : clean;
            std::wstring e = (dot != std::wstring::npos && dot > 0) ? clean.substr(dot) : std::wstring();
            std::wstring pick = clean;
            for (int k = 2; k < 1000 && GetFileAttributesW((savePath + L"\\" + pick).c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
                pick = b + L" (" + std::to_wstring(k) + L")" + e;
            if (MoveFileExW(finalPath.c_str(), (savePath + L"\\" + pick).c_str(), 0)) {
                finalPath = savePath + L"\\" + pick;
                name = pick;
            }
        }
        if (name != filename) {
            NdmHttpEngine::SetSharedString(dl->filename, name);
            if (opt.onChange) opt.onChange(dl, opt.onChangeCtx);
        }
        UINT64 sz = FileSize(finalPath);
        Store64(dl->totalSize, sz);
        Store64(dl->downloadedSize, sz);
        // Mark-of-the-Web: the page the video came from is the source.
        NdmMarkOfTheWeb(finalPath, source, referrer);
    } else {
        UINT64 got = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
        Store64(dl->totalSize, got);
    }
    return finish(NDM_RUN_COMPLETED);
}
