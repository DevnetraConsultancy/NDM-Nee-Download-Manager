#include "ndm_http.h"
#include <winhttp.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

// One registered download of the bandwidth allocator (see ndm_http.h).
struct NdmHttpEngine::Flow
{
    NdmDownload* dl       = nullptr;
    int          kind     = NDM_KIND_HTTP;
    bool         pinned   = false;   // media: rate fixed when the tool started
    DWORD        fixed    = 0;       // that rate (0 = started unlimited)
    DWORD        alloc    = 0;       // current rate, 0 = unlimited
    int          under    = 0;       // consecutive rounds clearly below alloc
    ULONGLONG    since    = 0;       // registration tick
};

// ===========================================================================
// Constants
// ===========================================================================
namespace {

// Default User-Agent when the download did not come with one from a browser.
// (Some servers drop connections from clients that fake a browser UA.)
const wchar_t* kDefaultUA = L"NDM/1.0 (Nee Download Manager; Windows)";

// One WinHttpReadData call (sync WinHTTP fills the whole buffer, so this is
// also the progress / pause granularity of one connection).
const DWORD   kReadChunk     = 128 * 1024;
// Per-connection write buffer: received data is collected here and written
// to the .ndmpart in one sequential WriteFile, instead of one positional
// write per network read.
const DWORD   kWriteBuf      = 2 * 1024 * 1024;
const UINT64  kMinSegSize    = 512ULL * 1024ULL;   // initial split granularity
const UINT64  kMinSplit      = 1024ULL * 1024ULL;  // steal only if >= 2x this left
const int     kMaxSegRetries = 6;                  // per segment, without progress
const int     kMaxConns      = 32;
const size_t  kSharedCap     = 1024;               // fixed capacity of shared strings
const wchar_t kPartExt[]     = L".ndmpart";
const wchar_t kStateExt[]    = L".ndmstate";

SRWLOCK       g_fieldLock    = SRWLOCK_INIT;

// Global speed limit (shared out by the bandwidth allocator).
volatile LONG g_speedLimit   = 0;

// Whole-download retry policy (NdmHttpEngine::SetRetryPolicy).
volatile LONG g_maxRetries   = 10;                 // 0 = unlimited
volatile LONG g_maxDelaySec  = 60;

// Bandwidth allocator state.
SRWLOCK                             g_flowLock  = SRWLOCK_INIT;
std::vector<NdmHttpEngine::Flow*>   g_flows;
ULONGLONG                           g_flowTick  = 0;
bool                                g_flowDirty = true;


// ===========================================================================
// Small string helpers
// ===========================================================================
std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string TrimA(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Header values must never contain CR/LF (header injection / broken request)
std::wstring NoCrLf(std::wstring s)
{
    for (wchar_t& c : s)
        if (c == L'\r' || c == L'\n') c = L' ';
    return Trim(s);
}

std::wstring U64ToStr(UINT64 v)
{
    wchar_t b[32];
    swprintf_s(b, L"%llu", (unsigned long long)v);
    return b;
}

bool TryUtf8(const std::string& s, std::wstring& out)
{
    out.clear();
    if (s.empty()) return true;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.resize(n);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.data(), (int)s.size(), &out[0], n);
    return true;
}

std::wstring BytesToWide(const std::string& s, UINT cp)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// UTF-8 if valid, else Windows-1252
std::wstring DecodeSmart(const std::string& s)
{
    std::wstring w;
    if (TryUtf8(s, w)) return w;
    return BytesToWide(s, 1252);
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

int HexVal(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            int h = HexVal((unsigned char)s[i + 1]);
            int l = HexVal((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0)
            {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool HasPercentEscapes(const std::string& s)
{
    for (size_t i = 0; i + 2 < s.size(); ++i)
        if (s[i] == '%' && HexVal((unsigned char)s[i + 1]) >= 0 &&
            HexVal((unsigned char)s[i + 2]) >= 0)
            return true;
    return false;
}

std::string LowerA(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// ===========================================================================
// Content-Disposition parsing (RFC 6266 / 5987)
// ===========================================================================
std::wstring ParseContentDisposition(const std::string& cdRaw)
{
    std::string cd = cdRaw;
    std::string lc = LowerA(cd);

    // 1) filename*=charset'lang'percent-encoded
    size_t p = lc.find("filename*");
    while (p != std::string::npos)
    {
        size_t q = p + 9;
        while (q < lc.size() && (lc[q] == ' ' || lc[q] == '\t')) ++q;
        if (q < lc.size() && lc[q] == '=')
        {
            ++q;
            size_t e = cd.find(';', q);
            std::string v = TrimA(cd.substr(q, e == std::string::npos
                                                  ? std::string::npos : e - q));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            size_t a1 = v.find('\'');
            size_t a2 = (a1 == std::string::npos) ? std::string::npos
                                                  : v.find('\'', a1 + 1);
            if (a2 != std::string::npos)
            {
                std::string charset = LowerA(v.substr(0, a1));
                std::string bytes   = PercentDecode(v.substr(a2 + 1));
                std::wstring w;
                if (charset == "utf-8" || charset == "utf8")
                {
                    if (!TryUtf8(bytes, w)) w = BytesToWide(bytes, 1252);
                }
                else if (charset == "iso-8859-1" || charset == "latin1")
                    w = BytesToWide(bytes, 28591);
                else
                    w = DecodeSmart(bytes);
                w = Trim(w);
                if (!w.empty()) return w;
            }
        }
        p = lc.find("filename*", p + 9);
    }

    // 2) filename="..." / filename=token
    p = lc.find("filename");
    while (p != std::string::npos)
    {
        size_t q = p + 8;
        if (q < lc.size() && lc[q] == '*') { p = lc.find("filename", q); continue; }
        while (q < lc.size() && (lc[q] == ' ' || lc[q] == '\t')) ++q;
        if (q < lc.size() && lc[q] == '=')
        {
            ++q;
            while (q < cd.size() && (cd[q] == ' ' || cd[q] == '\t')) ++q;
            std::string v;
            if (q < cd.size() && cd[q] == '"')
            {
                ++q;
                while (q < cd.size() && cd[q] != '"')
                {
                    if (cd[q] == '\\' && q + 1 < cd.size()) ++q;
                    v += cd[q++];
                }
            }
            else
            {
                size_t e = cd.find(';', q);
                v = TrimA(cd.substr(q, e == std::string::npos
                                           ? std::string::npos : e - q));
            }
            // Some servers percent-encode filename= (non standard but common)
            if (HasPercentEscapes(v))
            {
                std::wstring w;
                std::string dec = PercentDecode(v);
                if (TryUtf8(dec, w)) { w = Trim(w); if (!w.empty()) return w; }
            }
            std::wstring w = Trim(DecodeSmart(v));
            if (!w.empty()) return w;
        }
        p = lc.find("filename", p + 8);
    }
    return L"";
}

// "bytes 0-0/12345", "bytes */12345", "bytes 0-99/*"
bool ParseContentRange(const std::wstring& crIn, bool& hasRange, UINT64& first,
                       UINT64& last, bool& totalKnown, UINT64& total)
{
    std::wstring cr = Trim(crIn);
    hasRange = false; totalKnown = false; first = last = total = 0;
    if (cr.size() < 6 || _wcsnicmp(cr.c_str(), L"bytes", 5) != 0) return false;
    size_t i = 5;
    while (i < cr.size() && (cr[i] == L' ' || cr[i] == L'=')) ++i;
    if (i >= cr.size()) return false;
    if (cr[i] == L'*') { ++i; }
    else
    {
        wchar_t* e = nullptr;
        first = _wcstoui64(cr.c_str() + i, &e, 10);
        if (!e || *e != L'-') return false;
        last = _wcstoui64(e + 1, &e, 10);
        if (!e) return false;
        i = (size_t)(e - cr.c_str());
        hasRange = true;
    }
    size_t slash = cr.find(L'/', i);
    if (slash == std::wstring::npos) return hasRange;
    std::wstring t = Trim(cr.substr(slash + 1));
    if (!t.empty() && t[0] != L'*')
    {
        total = _wcstoui64(t.c_str(), nullptr, 10);
        totalKnown = true;
    }
    return true;
}

// ===========================================================================
// File-system helpers
// ===========================================================================
bool FileExists(const std::wstring& p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool EnsureDir(const std::wstring& dirIn)
{
    std::wstring d = dirIn;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (d.empty()) return true;
    DWORD a = GetFileAttributesW(d.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t p = d.find_last_of(L"\\/");
    if (p != std::wstring::npos && p > 0)
    {
        std::wstring parent = d.substr(0, p);
        if (!(parent.size() == 2 && parent[1] == L':'))
            EnsureDir(parent);
    }
    return CreateDirectoryW(d.c_str(), nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    std::wstring p = dir;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    return p + name;
}

std::wstring AddSuffix(const std::wstring& name, int k)
{
    size_t dot = name.find_last_of(L'.');
    wchar_t suf[24];
    swprintf_s(suf, L" (%d)", k);
    if (dot == std::wstring::npos || dot == 0)
        return name + suf;
    return name.substr(0, dot) + suf + name.substr(dot);
}

bool HasExtension(const std::wstring& n)
{
    size_t dot = n.find_last_of(L'.');
    return dot != std::wstring::npos && dot > 0 && dot + 1 < n.size() &&
           n.size() - dot <= 10;
}

bool SetFileSize(HANDLE h, UINT64 size)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
}

UINT64 GetFileSize64(HANDLE h)
{
    LARGE_INTEGER li = {};
    if (!GetFileSizeEx(h, &li)) return 0;
    return (UINT64)li.QuadPart;
}

void SetSparse(HANDLE h, bool on)
{
    FILE_SET_SPARSE_BUFFER b;
    b.SetSparse = on ? TRUE : FALSE;
    DWORD r = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, &b, sizeof(b), nullptr, 0, &r, nullptr);
}

bool WriteAt(HANDLE h, UINT64 off, const BYTE* p, DWORD n)
{
    while (n > 0)
    {
        OVERLAPPED ov = {};
        ov.Offset     = (DWORD)(off & 0xFFFFFFFFULL);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD w = 0;
        if (!WriteFile(h, p, n, &w, &ov)) return false;
        if (w == 0) { SetLastError(ERROR_WRITE_FAULT); return false; }
        p += w; n -= w; off += w;
    }
    return true;
}

// ===========================================================================
// URL helpers
// ===========================================================================
struct UrlParts
{
    bool          https = false;
    std::wstring  host, user, pass, object;
    INTERNET_PORT port = 0;
};

std::wstring CleanUrl(const std::wstring& in)
{
    std::wstring u = Trim(in);
    size_t h = u.find(L'#');
    if (h != std::wstring::npos) u.resize(h);
    std::wstring out;
    out.reserve(u.size());
    for (wchar_t c : u)
    {
        if (c == L' ') out += L"%20";
        else if (c == L'\r' || c == L'\n' || c == L'\t') continue;
        else out += c;
    }
    return out;
}

bool CrackUrl(const std::wstring& urlIn, UrlParts& u, bool& schemeOk)
{
    schemeOk = false;
    std::wstring url = CleanUrl(urlIn);
    if (url.empty()) return false;
    URL_COMPONENTS uc = {};
    uc.dwStructSize      = sizeof(uc);
    uc.dwSchemeLength    = 1;
    uc.dwHostNameLength  = 1;
    uc.dwUserNameLength  = 1;
    uc.dwPasswordLength  = 1;
    uc.dwUrlPathLength   = 1;
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
        return false;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) { u.https = true;  schemeOk = true; }
    else if (uc.nScheme == INTERNET_SCHEME_HTTP) { u.https = false; schemeOk = true; }
    if (uc.lpszHostName) u.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    if (uc.lpszUserName) u.user.assign(uc.lpszUserName, uc.dwUserNameLength);
    if (uc.lpszPassword) u.pass.assign(uc.lpszPassword, uc.dwPasswordLength);
    if (uc.lpszUrlPath)  u.object.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) u.object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (u.object.empty() || u.object[0] != L'/') u.object = L"/" + u.object;
    u.port = uc.nPort;
    return !u.host.empty();
}

bool IsFatalNetError(DWORD e)
{
    switch (e)
    {
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
    case ERROR_WINHTTP_LOGIN_FAILURE:
    case ERROR_WINHTTP_REDIRECT_FAILED:
        return true;
    }
    return false;
}

bool IsTransientHttp(DWORD s)
{
    return s == 408 || s == 425 || s == 429 ||
           (s >= 500 && s != 501 && s != 505);
}

// Minimal base64 (preemptive Basic auth for user:pass@host URLs).
std::string Base64(const std::string& in)
{
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) |
                     (unsigned char)in[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];  o += t[v & 63];
    }
    if (i + 1 == in.size())
    {
        unsigned v = (unsigned char)in[i] << 16;
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63]; o += "==";
    }
    else if (i + 2 == in.size())
    {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}

// ===========================================================================
// Job (one run of one download)
// ===========================================================================
struct Seg
{
    UINT64 start  = 0;
    UINT64 end    = 0;      // exclusive
    UINT64 pos    = 0;      // next byte to receive (received, maybe buffered)
    UINT64 done   = 0;      // bytes [start, done) are written to disk
    bool   active = false;  // owned by a worker
};

struct Req
{
    HINTERNET hConn = nullptr;
    HINTERNET hReq  = nullptr;
};

enum Outcome { OUT_OK, OUT_STOPPED, OUT_TRANSIENT, OUT_FATAL, OUT_RANGE_BROKEN };

struct Job
{
    NdmDownload*  dl = nullptr;
    NdmRunOptions opt;
    HINTERNET     hSession = nullptr;
    std::wstring  origUrl;
    std::wstring  url;              // effective URL (after redirects)
    std::wstring  referrer;         // page URL; sent per request per strict-origin-when-cross-origin
    std::wstring  cookieHdr;        // "Cookie: ...\r\n"   (origin of origUrl only)
    bool          ownCookies = false;
    HANDLE        hStop  = nullptr; // user stop (dl->hStopEvent)
    HANDLE        hAbort = nullptr; // internal abort (fatal error)

    bool          rangesOk  = false;
    bool          sizeKnown = false;
    UINT64        total     = 0;
    std::wstring  etag, lastMod;
    bool          singleMode = false;
    bool          oneConn    = false;   // ranges work, but only one connection (after a range error)
    bool          openEnded  = false;
    volatile LONG eofReached = 0;

    std::wstring  destPath, partPath, statePath;
    HANDLE        hFile = INVALID_HANDLE_VALUE;

    SRWLOCK          segLock = SRWLOCK_INIT;
    std::vector<Seg> segs;

    SRWLOCK            reqLock = SRWLOCK_INIT;
    std::vector<Req*>  reqs;
    bool               cancelled = false;
    Req                kept;        // probe response, reused as the first stream
    DWORD              keptStatus = 0;

    volatile LONG alive = 0;
    volatile LONG progressConns = 0;   // connections of this attempt that received data
    SRWLOCK       errLock = SRWLOCK_INIT;
    bool          hasErr = false, errFatal = false, rangeBroken = false;
    std::wstring  err;

    // Rate of this download from the bandwidth allocator (0 = unlimited),
    // enforced with a token bucket shared by its connections.
    NdmHttpEngine::Flow* flow = nullptr;
    volatile LONG rate      = 0;
    SRWLOCK       bucketLock = SRWLOCK_INIT;
    double        tokens    = 0.0;
    ULONGLONG     bucketTick = 0;
};

bool UserStopped(Job* j)
{
    return WaitForSingleObject(j->hStop, 0) == WAIT_OBJECT_0;
}

bool IsStopped(Job* j)
{
    HANDLE hs[2] = { j->hStop, j->hAbort };
    return WaitForMultipleObjects(2, hs, FALSE, 0) != WAIT_TIMEOUT;
}

// Returns true if stopped/aborted during the wait.
bool WaitStop(Job* j, DWORD ms)
{
    HANDLE hs[2] = { j->hStop, j->hAbort };
    return WaitForMultipleObjects(2, hs, FALSE, ms) != WAIT_TIMEOUT;
}

bool RegisterReq(Job* j, Req* r, HINTERNET h)
{
    AcquireSRWLockExclusive(&j->reqLock);
    if (j->cancelled)
    {
        ReleaseSRWLockExclusive(&j->reqLock);
        return false;
    }
    r->hReq = h;
    j->reqs.push_back(r);
    ReleaseSRWLockExclusive(&j->reqLock);
    return true;
}

HINTERNET GetReq(Job* j, Req* r)
{
    AcquireSRWLockShared(&j->reqLock);
    HINTERNET h = r->hReq;
    ReleaseSRWLockShared(&j->reqLock);
    return h;
}

void CloseReq(Job* j, Req* r)
{
    AcquireSRWLockExclusive(&j->reqLock);
    j->reqs.erase(std::remove(j->reqs.begin(), j->reqs.end(), r), j->reqs.end());
    HINTERNET h = r->hReq;
    r->hReq = nullptr;
    ReleaseSRWLockExclusive(&j->reqLock);
    if (h) WinHttpCloseHandle(h);
    if (r->hConn) { WinHttpCloseHandle(r->hConn); r->hConn = nullptr; }
}

// Close every open request handle: this unblocks any thread sitting in
// WinHttpSendRequest / WinHttpReceiveResponse / WinHttpReadData (closing
// the request handle is the documented way to cancel them).
void CancelAll(Job* j)
{
    std::vector<HINTERNET> toClose;
    AcquireSRWLockExclusive(&j->reqLock);
    j->cancelled = true;
    for (Req* r : j->reqs)
        if (r->hReq) { toClose.push_back(r->hReq); r->hReq = nullptr; }
    ReleaseSRWLockExclusive(&j->reqLock);
    for (HINTERNET h : toClose) WinHttpCloseHandle(h);
}

// Hand the kept probe response over to a worker (exactly once).
bool TakeKept(Job* j, Req& out, DWORD& status)
{
    bool ok = false;
    AcquireSRWLockExclusive(&j->reqLock);
    if (!j->cancelled && j->kept.hReq)
    {
        out.hConn = j->kept.hConn;
        out.hReq  = j->kept.hReq;
        j->kept.hConn = nullptr;
        j->kept.hReq  = nullptr;
        std::replace(j->reqs.begin(), j->reqs.end(), &j->kept, &out);
        status = j->keptStatus;
        ok = true;
    }
    ReleaseSRWLockExclusive(&j->reqLock);
    return ok;
}

bool HasKept(Job* j)
{
    AcquireSRWLockShared(&j->reqLock);
    bool k = j->kept.hReq != nullptr;
    ReleaseSRWLockShared(&j->reqLock);
    return k;
}

VOID CALLBACK StopCallback(PVOID ctx, BOOLEAN /*timedOut*/)
{
    CancelAll(reinterpret_cast<Job*>(ctx));
}

void SetJobError(Job* j, const std::wstring& msg, bool fatal, bool rangeBroken = false)
{
    AcquireSRWLockExclusive(&j->errLock);
    if (!j->hasErr || (fatal && !j->errFatal))
    {
        j->hasErr   = true;
        j->errFatal = fatal;
        j->err      = msg;
    }
    if (rangeBroken) j->rangeBroken = true;
    ReleaseSRWLockExclusive(&j->errLock);
}

void AbortJob(Job* j)
{
    SetEvent(j->hAbort);
    CancelAll(j);
}

// ===========================================================================
// HTTP request helpers
// ===========================================================================
std::wstring QueryStr(HINTERNET h, DWORD what)
{
    DWORD len = 0;
    WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return L"";
    std::vector<wchar_t> buf(len / sizeof(wchar_t) + 2, 0);
    DWORD len2 = (DWORD)(buf.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(h, what, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len2,
                             WINHTTP_NO_HEADER_INDEX))
        return L"";
    return std::wstring(buf.data(), len2 / sizeof(wchar_t));
}

// Header value as the original bytes (best effort). WinHTTP widens header
// bytes with the ANSI code page; undo that so UTF-8 names decode correctly.
std::string QueryRawA(HINTERNET h, DWORD what)
{
    std::wstring w = QueryStr(h, what);
    if (w.empty()) return "";
    BOOL usedDef = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, &usedDef);
    if (n > 0 && !usedDef)
    {
        std::string s(n, '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(),
                            &s[0], n, nullptr, nullptr);
        return s;
    }
    bool latin1 = true;
    for (wchar_t c : w) if (c > 0xFF) { latin1 = false; break; }
    if (latin1)
    {
        std::string s;
        for (wchar_t c : w) s += (char)(unsigned char)c;
        return s;
    }
    return WideToUtf8(w);
}

std::wstring QueryFinalUrl(HINTERNET h)
{
    DWORD len = 0;
    WinHttpQueryOption(h, WINHTTP_OPTION_URL, nullptr, &len);
    if (len == 0) return L"";
    std::vector<wchar_t> buf(len / sizeof(wchar_t) + 2, 0);
    DWORD len2 = (DWORD)(buf.size() * sizeof(wchar_t));
    if (!WinHttpQueryOption(h, WINHTTP_OPTION_URL, buf.data(), &len2)) return L"";
    return std::wstring(buf.data());
}

std::wstring HttpErrText(HINTERNET h, DWORD status)
{
    std::wstring t = h ? Trim(QueryStr(h, WINHTTP_QUERY_STATUS_TEXT)) : L"";
    std::wstring m = L"HTTP " + std::to_wstring(status);
    if (!t.empty()) m += L" " + t;
    switch (status)
    {
    case 401: m += L" - authentication required"; break;
    case 403: m += L" - access denied (link may have expired or needs login)"; break;
    case 404: m += L" - file not found on server"; break;
    case 410: m += L" - file no longer available"; break;
    case 416: m += L" - requested range not satisfiable"; break;
    case 429: m += L" - server is rate limiting"; break;
    }
    return m;
}

std::wstring LowerW(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Same scheme + host + port?
bool SameOrigin(const std::wstring& a, const std::wstring& b)
{
    UrlParts ua, ub;
    bool sa = false, sb = false;
    if (!CrackUrl(a, ua, sa) || !CrackUrl(b, ub, sb)) return false;
    return ua.https == ub.https && ua.port == ub.port &&
           LowerW(ua.host) == LowerW(ub.host);
}

// Resolve a Location header against the URL that produced it.
std::wstring ResolveLocation(const std::wstring& base, const std::wstring& locIn)
{
    std::wstring loc = CleanUrl(NoCrLf(locIn));
    if (loc.empty()) return L"";
    size_t colon = loc.find(L':');
    size_t slash = loc.find(L'/');
    if (colon != std::wstring::npos && (slash == std::wstring::npos || colon < slash))
        return loc;                                         // absolute
    UrlParts b;
    bool ok = false;
    if (!CrackUrl(base, b, ok) || !ok) return L"";
    std::wstring scheme = b.https ? L"https:" : L"http:";
    if (loc.compare(0, 2, L"//") == 0) return scheme + loc;  // scheme-relative
    std::wstring origin = scheme + L"//" + b.host;
    if ((b.https && b.port != 443) || (!b.https && b.port != 80))
        origin += L":" + std::to_wstring(b.port);
    if (loc[0] == L'/') return origin + loc;                // host-relative
    std::wstring path = b.object;
    size_t q = path.find_first_of(L"?");
    if (q != std::wstring::npos) path.resize(q);
    if (loc[0] == L'?') return origin + path + loc;
    size_t ls = path.find_last_of(L'/');
    path = (ls == std::wstring::npos) ? L"/" : path.substr(0, ls + 1);
    return origin + path + loc;                             // path-relative
}

// One request, no redirect handling: opens hConn + hReq, sends a GET and
// receives the response headers. Returns 0 on success (status filled),
// otherwise a WinHTTP / Win32 error.
DWORD OpenRequestOnce(Job* j, const std::wstring& url, const std::wstring& rangeHdr,
                      Req& r, DWORD& status)
{
    status = 0;
    UrlParts u;
    bool schemeOk = false;
    if (!CrackUrl(url, u, schemeOk)) return ERROR_WINHTTP_INVALID_URL;
    if (!schemeOk) return ERROR_WINHTTP_UNRECOGNIZED_SCHEME;

    // Connection handles are cheap: the session pools the TCP/TLS sockets
    // per host:port, so keep-alive / TLS session reuse still happens.
    r.hConn = WinHttpConnect(j->hSession, u.host.c_str(), u.port, 0);
    if (!r.hConn) return GetLastError();

    LPCWSTR accept[] = { L"*/*", nullptr };
    DWORD flags = WINHTTP_FLAG_REFRESH | (u.https ? WINHTTP_FLAG_SECURE : 0);
    HINTERNET h = WinHttpOpenRequest(r.hConn, L"GET", u.object.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, accept, flags);
    if (!h)
    {
        DWORD e = GetLastError();
        WinHttpCloseHandle(r.hConn);
        r.hConn = nullptr;
        return e ? e : ERROR_WINHTTP_INTERNAL_ERROR;
    }
    if (!RegisterReq(j, &r, h))
    {
        WinHttpCloseHandle(h);
        WinHttpCloseHandle(r.hConn);
        r.hConn = nullptr;
        return ERROR_WINHTTP_OPERATION_CANCELLED;
    }

    {
        DWORD v = WINHTTP_DISABLE_REDIRECTS;
        if (j->ownCookies) v |= WINHTTP_DISABLE_COOKIES;  // browser cookies only
        WinHttpSetOption(h, WINHTTP_OPTION_DISABLE_FEATURE, &v, sizeof(v));
    }

    // Referer per hop, strict-origin-when-cross-origin relative to THIS
    // request's URL: the full page URL only to the page's own origin, the
    // bare origin elsewhere, nothing on https -> http.
    // Browser cookies belong to the origin the user clicked on; they are
    // never sent to another scheme/host/port (e.g. a CDN redirect).
    std::wstring hdrs;
    std::wstring refVal = NdmReferrerFor(j->referrer, url);
    if (!refVal.empty()) hdrs = L"Referer: " + refVal + L"\r\n";
    if (SameOrigin(url, j->origUrl)) hdrs += j->cookieHdr;
    hdrs += rangeHdr + L"Accept-Encoding: identity\r\n";
    if (!u.user.empty())
        hdrs += L"Authorization: Basic " +
                BytesToWide(Base64(WideToUtf8(u.user + L":" + u.pass)), CP_ACP) + L"\r\n";

    for (int tries = 0; ; ++tries)
    {
        if (WinHttpSendRequest(h, hdrs.c_str(), (DWORD)hdrs.size(),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(h, nullptr))
            break;
        DWORD e = GetLastError();
        if (e == ERROR_WINHTTP_RESEND_REQUEST && tries < 3) continue;
        if (e == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED && tries < 1)
        {
            // Server asks for an optional client certificate: send none.
            WinHttpSetOption(h, WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
                             WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);
            continue;
        }
        CloseReq(j, &r);
        return e ? e : ERROR_WINHTTP_CONNECTION_ERROR;
    }

    DWORD len = sizeof(status);
    if (!WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX))
    {
        DWORD e = GetLastError();
        CloseReq(j, &r);
        return e ? e : ERROR_WINHTTP_INTERNAL_ERROR;
    }
    return 0;
}

bool IsRedirect(DWORD s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

// GET with manual redirect following (max 10 hops). On success r holds the
// final response; WINHTTP_OPTION_URL on it reports the final URL.
DWORD OpenRequest(Job* j, const std::wstring& urlIn, const std::wstring& rangeHdr,
                  Req& r, DWORD& status)
{
    std::wstring url = urlIn;
    for (int hop = 0; ; ++hop)
    {
        DWORD e = OpenRequestOnce(j, url, rangeHdr, r, status);
        if (e || !IsRedirect(status)) return e;
        HINTERNET h = GetReq(j, &r);
        std::wstring loc = h ? QueryStr(h, WINHTTP_QUERY_LOCATION) : L"";
        if (!h) { CloseReq(j, &r); return ERROR_WINHTTP_OPERATION_CANCELLED; }
        if (Trim(loc).empty()) return 0;          // 3xx without target: caller reports it
        CloseReq(j, &r);
        if (hop >= 10) return ERROR_WINHTTP_REDIRECT_FAILED;
        std::wstring next = ResolveLocation(url, loc);
        UrlParts cur, nu;
        bool s1 = false, s2 = false;
        if (next.empty() || !CrackUrl(url, cur, s1) || !CrackUrl(next, nu, s2))
            return ERROR_WINHTTP_INVALID_URL;
        if (!s2) return ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
        if (cur.https && !nu.https) return ERROR_WINHTTP_REDIRECT_FAILED;   // no downgrade
        url = next;
    }
}

Outcome NetFail(Job* j, DWORD e, std::wstring& err)
{
    if (IsStopped(j)) return OUT_STOPPED;
    err = NdmHttpEngine::ErrorText(e);
    return IsFatalNetError(e) ? OUT_FATAL : OUT_TRANSIENT;
}

// ===========================================================================
// Probe
// ===========================================================================
struct ProbeInfo
{
    DWORD        status    = 0;
    bool         rangesOk  = false;
    bool         sizeKnown = false;
    UINT64       total     = 0;
    std::wstring cdName, finalUrl, etag, lastMod;
};

void ReadCommonHeaders(HINTERNET h, ProbeInfo& pi)
{
    std::wstring fu = QueryFinalUrl(h);
    if (!fu.empty()) pi.finalUrl = fu;
    pi.etag    = Trim(QueryStr(h, WINHTTP_QUERY_ETAG));
    pi.lastMod = Trim(QueryStr(h, WINHTTP_QUERY_LAST_MODIFIED));
    std::wstring cd = ParseContentDisposition(QueryRawA(h, WINHTTP_QUERY_CONTENT_DISPOSITION));
    if (!cd.empty()) pi.cdName = cd;
}

void ReadContentLength(HINTERNET h, ProbeInfo& pi)
{
    std::wstring cl = Trim(QueryStr(h, WINHTTP_QUERY_CONTENT_LENGTH));
    if (!cl.empty() && iswdigit(cl[0]))
    {
        pi.total     = _wcstoui64(cl.c_str(), nullptr, 10);
        pi.sizeKnown = true;
    }
}

// Plain GET (no Range). On 200 keeps the response in j->kept for streaming.
Outcome ProbePlain(Job* j, ProbeInfo& pi, std::wstring& err)
{
    DWORD status = 0;
    DWORD e = OpenRequest(j, j->origUrl, L"", j->kept, status);
    if (e) return NetFail(j, e, err);
    HINTERNET h = GetReq(j, &j->kept);
    if (!h) { CloseReq(j, &j->kept); return OUT_STOPPED; }
    pi.status = status;
    ReadCommonHeaders(h, pi);
    pi.rangesOk = false;
    if (status == 200 || status == 203)
    {
        ReadContentLength(h, pi);
        j->keptStatus = status;
        return OUT_OK;
    }
    if (status == 204)
    {
        pi.sizeKnown = true; pi.total = 0;
        CloseReq(j, &j->kept);
        return OUT_OK;
    }
    err = HttpErrText(h, status);
    CloseReq(j, &j->kept);
    return IsTransientHttp(status) ? OUT_TRANSIENT : OUT_FATAL;
}

// Ranged GET probe: "Range: bytes=0-". Learns size, range support,
// filename and final URL in one request (HEAD is unreliable on many CDNs).
// The response (206 for the whole file from byte 0, or a plain 200) is
// kept open: the first connection streams its segment from it, so no extra
// round trip is spent before data starts flowing.
Outcome Probe(Job* j, ProbeInfo& pi, std::wstring& err)
{
    DWORD status = 0;
    DWORD e = OpenRequest(j, j->origUrl, L"Range: bytes=0-\r\n", j->kept, status);
    if (e) return NetFail(j, e, err);
    HINTERNET h = GetReq(j, &j->kept);
    if (!h) { CloseReq(j, &j->kept); return OUT_STOPPED; }

    pi.status = status;
    ReadCommonHeaders(h, pi);

    if (status == 206)
    {
        bool hasRange = false, totalKnown = false;
        UINT64 a = 0, b = 0, t = 0;
        bool ok = ParseContentRange(QueryStr(h, WINHTTP_QUERY_CONTENT_RANGE),
                                    hasRange, a, b, totalKnown, t);
        if (ok && hasRange && a == 0 && totalKnown)
        {
            pi.rangesOk  = true;
            pi.sizeKnown = true;
            pi.total     = t;
            j->keptStatus = 206;     // body = bytes 0.. of the file
            return OUT_OK;
        }
        CloseReq(j, &j->kept);
        // Odd 206 (unknown total / missing Content-Range): stream it plainly.
        return ProbePlain(j, pi, err);
    }
    if (status == 200 || status == 203)
    {
        // Server ignored the Range header: no segmented download.
        // Keep this response; its body is the whole file.
        pi.rangesOk = false;
        ReadContentLength(h, pi);
        j->keptStatus = status;
        return OUT_OK;
    }
    if (status == 204)
    {
        pi.sizeKnown = true; pi.total = 0;
        CloseReq(j, &j->kept);
        return OUT_OK;
    }
    if (status == 416)
    {
        bool hasRange = false, totalKnown = false;
        UINT64 a = 0, b = 0, t = 0;
        ParseContentRange(QueryStr(h, WINHTTP_QUERY_CONTENT_RANGE),
                          hasRange, a, b, totalKnown, t);
        CloseReq(j, &j->kept);
        if (totalKnown && t == 0)
        {
            pi.sizeKnown = true; pi.total = 0; pi.rangesOk = true;
            return OUT_OK;
        }
        return ProbePlain(j, pi, err);
    }

    err = HttpErrText(h, status);
    CloseReq(j, &j->kept);
    return IsTransientHttp(status) ? OUT_TRANSIENT : OUT_FATAL;
}

// ===========================================================================
// State file (.ndmstate) — persists segment progress across restarts
// ===========================================================================
struct StateData
{
    DWORD            id = 0;
    std::wstring     url;
    UINT64           total = 0;
    std::wstring     etag, lastMod;
    int              ranges = -1;       // 1 = saved by a resumable run, 0 = not, -1 = older file
    std::vector<Seg> segs;
};

bool LoadState(const std::wstring& path, StateData& st)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    UINT64 sz = GetFileSize64(h);
    if (sz == 0 || sz > 4 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string raw((size_t)sz, '\0');
    DWORD rd = 0;
    BOOL ok = ReadFile(h, &raw[0], (DWORD)sz, &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    raw.resize(rd);

    std::wstring text;
    if (!TryUtf8(raw, text)) return false;
    if (text.compare(0, 10, L"NDMSTATE 2") != 0) return false;

    size_t p = 0;
    while (p < text.size())
    {
        size_t e = text.find(L'\n', p);
        std::wstring line = text.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        p = (e == std::wstring::npos) ? text.size() : e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        if      (k == L"id")      st.id      = (DWORD)_wcstoui64(v.c_str(), nullptr, 10);
        else if (k == L"url")     st.url     = v;
        else if (k == L"total")   st.total   = _wcstoui64(v.c_str(), nullptr, 10);
        else if (k == L"etag")    st.etag    = v;
        else if (k == L"lastmod") st.lastMod = v;
        else if (k == L"ranges")  st.ranges  = (v == L"1") ? 1 : 0;
        else if (k == L"seg")
        {
            Seg s;
            wchar_t* q = nullptr;
            s.start = _wcstoui64(v.c_str(), &q, 10);
            s.end   = _wcstoui64(q, &q, 10);
            s.pos   = _wcstoui64(q, &q, 10);
            s.done  = s.pos;
            st.segs.push_back(s);
        }
    }
    // Validate
    if (st.segs.empty()) return true;
    for (const Seg& s : st.segs)
        if (s.start > s.pos || s.pos > s.end || s.end > st.total) return false;
    return true;
}

bool SaveState(Job* j)
{
    if (j->statePath.empty()) return false;
    std::wstring t = L"NDMSTATE 2\n";
    t += L"id=" + std::to_wstring(j->dl->id) + L"\n";
    t += L"url=" + j->origUrl + L"\n";
    t += L"total=" + U64ToStr(j->total) + L"\n";
    t += L"etag=" + j->etag + L"\n";
    t += L"lastmod=" + j->lastMod + L"\n";
    t += std::wstring(L"ranges=") + ((j->rangesOk && j->sizeKnown) ? L"1" : L"0") + L"\n";
    AcquireSRWLockShared(&j->segLock);
    // Only bytes that are really on disk count ("done"), not buffered ones.
    for (const Seg& s : j->segs)
        t += L"seg=" + U64ToStr(s.start) + L" " + U64ToStr(s.end) + L" " +
             U64ToStr(s.done) + L"\n";
    ReleaseSRWLockShared(&j->segLock);
    // Everything counted above was written before the snapshot: flush it,
    // so after a power cut the state never claims bytes that were still in
    // the cache (zero-filled holes in the finished file).
    if (j->hFile != INVALID_HANDLE_VALUE) FlushFileBuffers(j->hFile);

    std::string data = WideToUtf8(t);
    std::wstring tmp = j->statePath + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || w != data.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), j->statePath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ===========================================================================
// Speed limiter
// ===========================================================================
// ---- bandwidth allocator (see ndm_http.h for the policy) -------------------
const double kUnlimited   = 1e18;
const double kMinFlowRate = 4096.0;        // never starve a flow completely
const double kProbeRate   = 16384.0;       // headroom floor of a flow that uses little
const double kLowMinTotal = 32768.0;       // no global limit: Low flows together get >= this

double PrioWeight(int p)
{
    return p >= NDM_PRIO_HIGH ? 4.0 : (p <= NDM_PRIO_LOW ? 1.0 : 2.0);
}

// Called with g_flowLock held exclusively.
void RecomputeFlows(ULONGLONG now)
{
    g_flowTick  = now;
    g_flowDirty = false;
    const size_t n = g_flows.size();
    if (n == 0) return;
    const double G = (double)(DWORD)g_speedLimit;

    struct FlowCalc { double w, cap, rate, demand, alloc; bool pinned, low; };
    std::vector<FlowCalc> v(n);
    for (size_t i = 0; i < n; ++i)
    {
        NdmHttpEngine::Flow* f = g_flows[i];
        const NdmDownload* dl = f->dl;
        int   prio = *reinterpret_cast<const volatile int*>(&dl->priority);
        DWORD lim  = *reinterpret_cast<const volatile DWORD*>(&dl->speedLimit);
        DWORD spd  = *reinterpret_cast<const volatile DWORD*>(&dl->speed);
        FlowCalc& c = v[i];
        c.w      = PrioWeight(prio);
        c.low    = prio <= NDM_PRIO_LOW;
        c.cap    = lim ? (double)lim : kUnlimited;
        c.rate   = (double)spd;
        c.pinned = f->pinned;
        if (c.pinned && f->fixed) c.cap = std::min(c.cap, (double)f->fixed);
        // Clearly below its rate for several rounds: limited elsewhere (slow
        // server, retry wait). Young flows are still ramping up.
        double ref = c.pinned ? (double)f->fixed : (double)f->alloc;
        if (now - f->since >= 2000 && ref > 0 && c.rate < 0.8 * ref)
        {
            if (f->under < 1000) ++f->under;
        }
        else
            f->under = 0;
        c.demand = c.cap;
        if (f->under >= 3) c.demand = std::min(c.cap, std::max(c.rate * 1.3, kProbeRate));
        c.alloc = c.cap;
    }

    if (G > 0)
    {
        // Pinned flows (media) take what they were started with.
        double reserved = 0;
        std::vector<size_t> act;
        for (size_t i = 0; i < n; ++i)
        {
            if (v[i].pinned)
            {
                double use = g_flows[i]->fixed ? v[i].demand : v[i].rate;
                reserved += use;
                v[i].alloc = g_flows[i]->fixed ? (double)g_flows[i]->fixed : 0;
            }
            else
                act.push_back(i);
        }
        // Weighted max-min water filling of the rest.
        double rem = std::max(0.0, G - reserved);
        while (!act.empty())
        {
            double W = 0;
            for (size_t i : act) W += v[i].w;
            std::vector<size_t> keep;
            double used = 0;
            for (size_t i : act)
            {
                double share = rem * v[i].w / W;
                if (v[i].demand <= share) { v[i].alloc = v[i].demand; used += v[i].demand; }
                else keep.push_back(i);
            }
            if (keep.size() == act.size())
            {
                for (size_t i : act) v[i].alloc = rem * v[i].w / W;
                break;
            }
            rem = std::max(0.0, rem - used);
            act.swap(keep);
        }
        for (size_t i = 0; i < n; ++i)
            if (!v[i].pinned && v[i].alloc < kMinFlowRate) v[i].alloc = kMinFlowRate;
    }
    else
    {
        // No global limit: Low flows yield to High/Normal flows that are
        // transferring and not merely held back by their own limit.
        double hi = 0;
        bool   hiActive = false;
        int    nLow = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (v[i].low) { if (!v[i].pinned) ++nLow; continue; }
            bool selfCapped = v[i].cap < kUnlimited && v[i].rate >= 0.8 * v[i].cap;
            if (v[i].rate > 1024 && !selfCapped) { hiActive = true; hi += v[i].rate; }
        }
        if (hiActive && nLow > 0)
        {
            double each = std::max(kLowMinTotal, hi / 4.0) / nLow;
            for (size_t i = 0; i < n; ++i)
                if (v[i].low && !v[i].pinned) v[i].alloc = std::min(v[i].cap, each);
        }
    }

    for (size_t i = 0; i < n; ++i)
    {
        NdmHttpEngine::Flow* f = g_flows[i];
        if (f->pinned) { f->alloc = f->fixed; continue; }
        double a = v[i].alloc;
        f->alloc = a >= kUnlimited ? 0 : (DWORD)std::min(a, 2.0e9);
        if (a < kUnlimited && f->alloc == 0) f->alloc = 1;
    }
}

DWORD ThrottleChunk(Job* j)
{
    LONG r = j->rate;
    if (r <= 0) return 0;
    DWORD c = (DWORD)(r / 8);
    if (c < 1024) c = 1024;
    if (c > kReadChunk) c = kReadChunk;
    return c;
}

// Refresh this download's rate from the allocator (monitor loop, ~2x/s).
void UpdateJobRate(Job* j)
{
    if (!j->flow) return;
    DWORD r = NdmHttpEngine::FlowRate(j->flow);
    InterlockedExchange(&j->rate, (LONG)std::min<DWORD>(r, 0x7FFFFFFF));
}

void Throttle(Job* j, UINT64 n)
{
    LONG rate = j->rate;
    if (rate <= 0) return;
    double waitMs = 0;
    AcquireSRWLockExclusive(&j->bucketLock);
    ULONGLONG now = GetTickCount64();
    if (j->bucketTick == 0 || now - j->bucketTick > 5000)
        j->tokens = rate / 4.0;
    else
        j->tokens += (double)(now - j->bucketTick) * rate / 1000.0;
    j->bucketTick = now;
    if (j->tokens > rate / 2.0) j->tokens = rate / 2.0;
    j->tokens -= (double)n;
    if (j->tokens < 0) waitMs = (-j->tokens) * 1000.0 / rate;
    ReleaseSRWLockExclusive(&j->bucketLock);
    if (waitMs > 0)
        WaitStop(j, (DWORD)std::min(waitMs, 2000.0));
}

// ===========================================================================
// Segment workers
// ===========================================================================
// Returns an index of a segment the caller now owns, or -1 if nothing left.
int AcquireSegment(Job* j)
{
    AcquireSRWLockExclusive(&j->segLock);
    int found = -1;
    for (size_t i = 0; i < j->segs.size(); ++i)
    {
        Seg& s = j->segs[i];
        bool todo = j->openEnded ? (j->eofReached == 0) : (s.pos < s.end);
        if (!s.active && todo)
        {
            s.active = true;
            found = (int)i;
            break;
        }
    }
    if (found < 0 && !j->singleMode && !j->oneConn)
    {
        // Dynamic segmentation: steal the second half of the largest
        // remaining active segment.
        int best = -1;
        UINT64 bestRem = 0;
        for (size_t i = 0; i < j->segs.size(); ++i)
        {
            const Seg& s = j->segs[i];
            UINT64 rem = s.end - s.pos;
            if (s.active && rem > bestRem) { bestRem = rem; best = (int)i; }
        }
        if (best >= 0 && bestRem >= 2 * kMinSplit)
        {
            Seg ns;
            UINT64 mid = j->segs[best].pos + bestRem / 2;
            ns.start  = mid;
            ns.pos    = mid;
            ns.done   = mid;
            ns.end    = j->segs[best].end;
            ns.active = true;
            j->segs[best].end = mid;
            j->segs.push_back(ns);
            found = (int)j->segs.size() - 1;
        }
    }
    ReleaseSRWLockExclusive(&j->segLock);
    return found;
}

enum ReadOutcome { RD_DONE, RD_STOPPED, RD_NETFAIL, RD_WRITEFAIL };

// Per-connection write buffer. Holds the received bytes [off, off + fill)
// of one segment that are not on disk yet (seg.done == off).
struct ConnBuf
{
    std::vector<BYTE> data;
    DWORD             fill = 0;
    UINT64            off  = 0;
    bool              progressed = false;   // this connection received data in this attempt
};

bool FlushBuf(Job* j, int idx, ConnBuf& cb, std::wstring& err)
{
    if (cb.fill == 0) return true;
    if (!WriteAt(j->hFile, cb.off, cb.data.data(), cb.fill))
    {
        err = L"Cannot write to disk: " + NdmHttpEngine::ErrorText(GetLastError());
        cb.fill = 0;
        return false;
    }
    AcquireSRWLockExclusive(&j->segLock);
    j->segs[idx].done = cb.off + cb.fill;
    ReleaseSRWLockExclusive(&j->segLock);
    cb.off += cb.fill;
    cb.fill = 0;
    return true;
}

// The connection of segment idx reached its end. If its request is
// open-ended and the directly following segment is untouched and unowned,
// take that segment over and keep streaming on the same connection (no new
// request, no handshake, no TCP slow start).
bool TryAbsorbNext(Job* j, int idx)
{
    bool ok = false;
    AcquireSRWLockExclusive(&j->segLock);
    Seg& s = j->segs[idx];
    for (size_t k = 0; k < j->segs.size(); ++k)
    {
        Seg& n = j->segs[k];
        if ((int)k != idx && !n.active && n.start == s.end && n.pos == n.start &&
            n.end > n.start)
        {
            s.end = n.end;
            n.end = n.start;          // now empty; skipped by everyone
            ok = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&j->segLock);
    return ok;
}

ReadOutcome Pump(Job* j, int idx, HINTERNET h, ConnBuf& cb,
                 std::wstring& err, bool& progressed, bool openRange)
{
    NdmDownload* dl = j->dl;
    AcquireSRWLockShared(&j->segLock);
    cb.off  = j->segs[idx].pos;     // == done: previous data was flushed
    ReleaseSRWLockShared(&j->segLock);
    cb.fill = 0;

    // Every exit path writes what was received: that data is valid, and the
    // state file must never claim bytes that are not on disk.
    auto leave = [&](ReadOutcome ro) -> ReadOutcome
    {
        std::wstring werr;
        if (!FlushBuf(j, idx, cb, werr)) { err = werr; return RD_WRITEFAIL; }
        return ro;
    };

    for (;;)
    {
        if (IsStopped(j)) return leave(RD_STOPPED);

        UINT64 pos, end;
        AcquireSRWLockShared(&j->segLock);
        pos = j->segs[idx].pos;
        end = j->segs[idx].end;
        ReleaseSRWLockShared(&j->segLock);
        if (!j->openEnded && pos >= end)                          // done or stolen
        {
            if (!(openRange && TryAbsorbNext(j, idx))) return leave(RD_DONE);
            continue;
        }

        if (cb.fill == (DWORD)cb.data.size() && !FlushBuf(j, idx, cb, err))
            return RD_WRITEFAIL;

        DWORD want = std::min<DWORD>(kReadChunk, (DWORD)cb.data.size() - cb.fill);
        DWORD lim = ThrottleChunk(j);
        if (lim && want > lim) want = lim;
        if (!j->openEnded && end - pos < want) want = (DWORD)(end - pos);

        // A pause/abort closes the handle, which aborts a blocked read.
        DWORD n = 0;
        if (!WinHttpReadData(h, cb.data.data() + cb.fill, want, &n))
        {
            DWORD e = GetLastError();
            if (IsStopped(j)) return leave(RD_STOPPED);
            err = NdmHttpEngine::ErrorText(e);
            return leave(RD_NETFAIL);
        }

        if (n == 0)   // end of response body
        {
            ReadOutcome ro = leave(RD_DONE);
            if (ro != RD_DONE) return ro;
            AcquireSRWLockExclusive(&j->segLock);
            Seg& s = j->segs[idx];
            bool complete = j->openEnded || s.pos >= s.end;
            if (j->openEnded) { s.end = s.pos; InterlockedExchange(&j->eofReached, 1); }
            ReleaseSRWLockExclusive(&j->segLock);
            if (complete) return RD_DONE;
            err = L"Connection closed by server before the data was complete";
            return RD_NETFAIL;
        }

        // Account the bytes. The segment end may have shrunk meanwhile
        // (another connection stole the rest); the overshoot is dropped.
        DWORD n2 = n;
        AcquireSRWLockExclusive(&j->segLock);
        Seg& s = j->segs[idx];
        if (!j->openEnded)
        {
            UINT64 room = s.end > s.pos ? s.end - s.pos : 0;
            if (n2 > room) n2 = (DWORD)room;
        }
        s.pos += n2;
        if (j->openEnded) s.end = s.pos;   // keep end == pos (unknown size)
        bool segDone = !j->openEnded && s.pos >= s.end;
        ReleaseSRWLockExclusive(&j->segLock);

        cb.fill += n2;
        InterlockedAdd64(reinterpret_cast<volatile LONG64*>(&dl->downloadedSize),
                         (LONG64)n2);
        progressed = true;
        if (!cb.progressed && n2 > 0)
        {
            cb.progressed = true;
            InterlockedIncrement(&j->progressConns);
        }
        Throttle(j, n2);

        if (segDone && !(openRange && TryAbsorbNext(j, idx))) return leave(RD_DONE);
    }
}

enum SegOutcome { SEG_DONE, SEG_STOPPED, SEG_GIVEUP, SEG_FATAL, SEG_RANGE_BROKEN };

SegOutcome FetchSegment(Job* j, int idx, ConnBuf& cb, std::wstring& err)
{
    int failures = 0;
    for (;;)
    {
        if (IsStopped(j)) return SEG_STOPPED;

        UINT64 pos, end;
        AcquireSRWLockShared(&j->segLock);
        pos = j->segs[idx].pos;
        end = j->segs[idx].end;
        ReleaseSRWLockShared(&j->segLock);
        if (j->openEnded ? (j->eofReached != 0) : (pos >= end)) return SEG_DONE;

        Req   r;
        DWORD status = 0;
        DWORD e = 0;
        bool  expect206 = false;

        // The probe response starts at byte 0: the connection whose segment
        // starts there takes it over (no new request / handshake needed).
        if (pos == 0 && TakeKept(j, r, status))
        {
            expect206 = (status == 206);
        }
        else
        {
            std::wstring range;
            if (j->singleMode)
            {
                if (pos > 0)
                {
                    if (j->rangesOk)
                    {
                        range = L"Range: bytes=" + U64ToStr(pos) + L"-\r\n";
                        expect206 = true;
                    }
                    else
                    {
                        // Cannot resume: restart this stream from zero.
                        AcquireSRWLockExclusive(&j->segLock);
                        j->segs[idx].pos  = 0;
                        j->segs[idx].done = 0;
                        if (j->openEnded) j->segs[idx].end = 0;
                        ReleaseSRWLockExclusive(&j->segLock);
                        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
                                              &j->dl->downloadedSize), 0);
                        pos = 0;
                    }
                }
            }
            else
            {
                // Open-ended: the connection can run on into the next
                // untouched segment (see TryAbsorbNext); we simply stop
                // reading where our segment ends.
                range = L"Range: bytes=" + U64ToStr(pos) + L"-\r\n";
                expect206 = true;
            }
            e = OpenRequest(j, j->url, range, r, status);
        }

        bool progressed = false;
        if (e)
        {
            if (IsStopped(j)) return SEG_STOPPED;
            err = NdmHttpEngine::ErrorText(e);
            if (IsFatalNetError(e)) return SEG_FATAL;
        }
        else
        {
            HINTERNET h = GetReq(j, &r);
            bool proceed = false;
            if (!h)
            {
                CloseReq(j, &r);
                return SEG_STOPPED;
            }
            if (expect206 && status == 206)
            {
                bool hasRange = false, tk = false;
                UINT64 a = 0, b = 0, t = 0;
                std::wstring cr = QueryStr(h, WINHTTP_QUERY_CONTENT_RANGE);
                if (!cr.empty() &&
                    (!ParseContentRange(cr, hasRange, a, b, tk, t) ||
                     (hasRange && a != pos) ||
                     (tk && j->sizeKnown && t != j->total)))
                {
                    CloseReq(j, &r);
                    err = L"Server returned a different byte range or file size";
                    return SEG_RANGE_BROKEN;
                }
                proceed = true;
            }
            else if (expect206 && status == 200)
            {
                CloseReq(j, &r);
                err = L"Server stopped honouring byte ranges";
                return SEG_RANGE_BROKEN;
            }
            else if (!expect206 && (status == 200 || status == 203))
            {
                proceed = true;
            }
            else
            {
                err = HttpErrText(h, status);
                CloseReq(j, &r);
                if (!IsTransientHttp(status))
                {
                    // 401/403/404/410/416... on one connection while another
                    // one received data: the file is there, this request
                    // was refused (per-connection token, connection limit
                    // answered with 403...). Leave the segment to the working
                    // connections; the job fails only if none can proceed.
                    LONG others = j->progressConns - (cb.progressed ? 1 : 0);
                    if (others <= 0) return SEG_FATAL;
                    if (j->alive > 1) return SEG_GIVEUP;
                    // The others are done: this is the last connection. The
                    // address works, so retry this request a few times.
                    if (failures >= 3) return SEG_FATAL;
                }
                // Connection limit reached on the server: let the connections
                // that work take over instead of hammering it.
                if ((status == 429 || status == 503) && j->alive > 1) return SEG_GIVEUP;
            }

            if (proceed)
            {
                ReadOutcome ro = Pump(j, idx, h, cb, err, progressed,
                                      expect206 && !j->singleMode);
                CloseReq(j, &r);
                if (ro == RD_DONE)      return SEG_DONE;
                if (ro == RD_STOPPED)   return SEG_STOPPED;
                if (ro == RD_WRITEFAIL) return SEG_FATAL;
                // RD_NETFAIL -> retry below
            }
        }

        // Transient failure: back off and retry.
        if (progressed) failures = 0;
        ++failures;
        // If other connections are still working, give up this one early
        // (server may limit connections per client); they take over the rest.
        if (j->alive > 1 && failures >= 3) return SEG_GIVEUP;
        if (failures > kMaxSegRetries) return SEG_GIVEUP;
        DWORD wait = std::min<DWORD>(30000, 1000u << (failures - 1));
        if (WaitStop(j, wait)) return SEG_STOPPED;
    }
}

DWORD WINAPI WorkerProc(LPVOID param)
{
    Job* j = reinterpret_cast<Job*>(param);
    ConnBuf cb;
    bool decremented = false;
    try { cb.data.resize(kWriteBuf); }
    catch (...) { cb.data.resize(kReadChunk); }   // low memory: small buffer

    while (!IsStopped(j))
    {
        int idx = AcquireSegment(j);
        if (idx < 0) break;

        std::wstring err;
        SegOutcome o = FetchSegment(j, idx, cb, err);

        AcquireSRWLockExclusive(&j->segLock);
        j->segs[idx].active = false;
        ReleaseSRWLockExclusive(&j->segLock);

        if (o == SEG_DONE) continue;
        if (o == SEG_STOPPED) break;
        if (o == SEG_GIVEUP)
        {
            decremented = true;
            if (InterlockedDecrement(&j->alive) > 0)
                return 0;                   // others will finish the work
            SetJobError(j, err.empty() ? L"Connection failed" : err, false);
            AbortJob(j);
            return 0;
        }
        if (o == SEG_RANGE_BROKEN)
        {
            SetJobError(j, err, false, true);
            AbortJob(j);
            break;
        }
        // SEG_FATAL
        SetJobError(j, err.empty() ? L"Download failed" : err, true);
        AbortJob(j);
        break;
    }
    if (!decremented) InterlockedDecrement(&j->alive);
    return 0;
}

// ===========================================================================
// One attempt: probe, prepare files, run workers, finalize
// ===========================================================================
std::wstring ResolveName(const ProbeInfo& pi, const std::wstring& origUrl,
                         const std::wstring& cur)
{
    if (!pi.cdName.empty())
        return NdmHttpEngine::SanitizeFilename(pi.cdName);
    std::wstring fromOrig  = NdmSafeFilename(CleanUrl(origUrl));
    std::wstring fromFinal = pi.finalUrl.empty() ? L"" : NdmSafeFilename(CleanUrl(pi.finalUrl));
    std::wstring pick;
    if (!fromFinal.empty() && fromFinal != L"download" &&
        (HasExtension(fromFinal) || !HasExtension(fromOrig)))
        pick = fromFinal;
    else if (!fromOrig.empty() && fromOrig != L"download")
        pick = fromOrig;
    else
        pick = cur.empty() ? L"download" : cur;
    return NdmHttpEngine::SanitizeFilename(pick);
}

void CloseJobFile(Job* j)
{
    if (j->hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(j->hFile);
        j->hFile = INVALID_HANDLE_VALUE;
    }
}

Outcome Finalize(Job* j, std::wstring& err)
{
    NdmDownload* dl = j->dl;
    UINT64 finalSize = j->total;
    if (j->openEnded)
    {
        AcquireSRWLockShared(&j->segLock);
        finalSize = j->segs.empty() ? 0 : j->segs[0].pos;
        ReleaseSRWLockShared(&j->segLock);
    }
    SetSparse(j->hFile, false);
    if (!SetFileSize(j->hFile, finalSize))
    {
        err = L"Cannot finalize file: " + NdmHttpEngine::ErrorText(GetLastError());
        CloseJobFile(j);
        return OUT_FATAL;
    }
    CloseJobFile(j);

    std::wstring dir  = dl->savePath;
    std::wstring name = NdmHttpEngine::GetSharedString(dl->filename);
    std::wstring target = j->destPath;
    DWORD mvFlags = MOVEFILE_COPY_ALLOWED;
    if (FileExists(target))
    {
        // Never overwrite an existing file unless the caller explicitly
        // confirmed it; otherwise pick a free "name (k).ext".
        if (j->opt.overwriteConfirmed)
            mvFlags |= MOVEFILE_REPLACE_EXISTING;
        else
        {
            for (int k = 1; k < 10000; ++k)
            {
                std::wstring cand = AddSuffix(name, k);
                std::wstring p = JoinPath(dir, cand);
                if (!FileExists(p) && !FileExists(p + kPartExt))
                {
                    target = p;
                    NdmHttpEngine::SetSharedString(dl->filename, cand);
                    if (j->opt.onChange) j->opt.onChange(dl, j->opt.onChangeCtx);
                    break;
                }
            }
        }
    }
    if (!MoveFileExW(j->partPath.c_str(), target.c_str(), mvFlags))
    {
        err = L"Cannot rename temporary file: " +
              NdmHttpEngine::ErrorText(GetLastError());
        return OUT_FATAL;
    }
    DeleteFileW(j->statePath.c_str());
    NdmMarkOfTheWeb(target, dl->url, dl->referrer);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&dl->totalSize), (LONG64)finalSize);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&dl->downloadedSize), (LONG64)finalSize);
    return OUT_OK;
}

// Publishes the real on-disk map (32 parts of the file) and the number of
// connections transferring, for the Properties window.
void PublishFileMap(Job* j)
{
    NdmDownload* dl = j->dl;
    const int kParts = 32;
    if (!j->sizeKnown || j->total == 0 || j->openEnded)
    {
        InterlockedExchange(&dl->fileMapOk, 0);
        return;
    }
    double have[kParts] = {};
    const double part = (double)j->total / kParts;
    LONG conns = 0;
    AcquireSRWLockShared(&j->segLock);
    for (const Seg& sg : j->segs)
    {
        if (sg.active && sg.pos < sg.end) ++conns;
        UINT64 a = sg.start, b = sg.done;
        if (b <= a) continue;
        int i0 = (int)std::min<double>(kParts - 1, (double)a / part);
        int i1 = (int)std::min<double>(kParts - 1, (double)(b - 1) / part);
        for (int i = i0; i <= i1; ++i)
        {
            double lo = std::max((double)a, i * part), hi = std::min((double)b, (i + 1) * part);
            if (hi > lo) have[i] += hi - lo;
        }
    }
    ReleaseSRWLockShared(&j->segLock);
    for (int i = 0; i < kParts; ++i)
        dl->fileMap[i] = (WORD)std::min(1000.0, have[i] * 1000.0 / part + 0.5);
    InterlockedExchange(&dl->activeConns, conns);
    InterlockedExchange(&dl->fileMapOk, 1);
}

// Is the server's file still the one the saved part belongs to? Same size,
// and Last-Modified / ETag agree where both sides have them. An ETag that
// differs while Last-Modified matches is a CDN giving each node its own
// ETag, not a changed file.
bool SameRemoteFile(const StateData& st, const Job* j)
{
    return NdmHttpEngine::SameRemoteFile(st.total, st.etag, st.lastMod,
                                         j->total, j->etag, j->lastMod);
}

UINT64 BytesOnDisk(const StateData& st)
{
    UINT64 n = 0;
    for (const Seg& s : st.segs) n += s.pos - s.start;
    return n;
}

// Below this, restarting a download that can no longer resume is harmless.
constexpr UINT64 kKeepPartMin = 1024 * 1024;

Outcome RunOnce(Job* j, bool forceSingle, std::wstring& err)
{
    NdmDownload* dl = j->dl;

    // ---- 1. probe ---------------------------------------------------------
    ProbeInfo pi;
    Outcome o = Probe(j, pi, err);
    if (o != OUT_OK) { CloseReq(j, &j->kept); return o; }
    // After a range error: keep using byte ranges (so the bytes already on
    // disk are kept), but through one connection only, without splitting.
    j->oneConn = forceSingle && pi.rangesOk && pi.sizeKnown;
    j->url       = pi.finalUrl.empty() ? j->origUrl : pi.finalUrl;
    j->rangesOk  = pi.rangesOk;
    j->sizeKnown = pi.sizeKnown;
    j->total     = pi.sizeKnown ? pi.total : 0;
    j->etag      = pi.etag;
    j->lastMod   = pi.lastMod;
    j->singleMode = !(pi.rangesOk && pi.sizeKnown);
    j->openEnded  = !pi.sizeKnown;
    InterlockedExchange(&dl->resumable, j->singleMode ? 0 : 1);
    InterlockedExchange(&j->eofReached, 0);

    // ---- 2. destination ---------------------------------------------------
    std::wstring dir = dl->savePath;
    if (!EnsureDir(dir))
    {
        err = L"Cannot create folder \"" + dir + L"\": " +
              NdmHttpEngine::ErrorText(GetLastError());
        CloseReq(j, &j->kept);
        return OUT_FATAL;
    }

    std::wstring curName = NdmHttpEngine::SanitizeFilename(
                               NdmHttpEngine::GetSharedString(dl->filename));
    std::wstring dest = JoinPath(dir, curName);

    StateData st;
    bool owned = LoadState(dest + kStateExt, st) && st.id == dl->id &&
                 st.url == j->origUrl && FileExists(dest + kPartExt);

    // The part on disk came from a resumable run, but the server does not
    // accept byte ranges right now: keep the part (never truncate it) and stop
    // with an explanation, instead of silently starting again from zero.
    if (owned && j->singleMode && st.ranges == 1 && BytesOnDisk(st) >= kKeepPartMin &&
        (!pi.sizeKnown || st.total == j->total))
    {
        err = L"The server is not accepting resume requests right now. The " +
              NdmFormatSize(BytesOnDisk(st)) +
              L" already downloaded are kept: try Resume later, or Redownload to start over.";
        CloseReq(j, &j->kept);
        return OUT_FATAL;
    }

    bool resume = false;
    if (owned && !j->singleMode && !st.segs.empty() && SameRemoteFile(st, j))
    {
        HANDLE h = CreateFileW((dest + kPartExt).c_str(),
                               GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            if (GetFileSize64(h) == j->total) { j->hFile = h; resume = true; }
            else CloseHandle(h);
        }
    }

    if (!resume)
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        if (owned)
        {
            if (curName != NdmHttpEngine::GetSharedString(dl->filename))
            {
                NdmHttpEngine::SetSharedString(dl->filename, curName);
                if (j->opt.onChange) j->opt.onChange(dl, j->opt.onChangeCtx);
            }
            h = CreateFileW((dest + kPartExt).c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                err = L"Cannot create file \"" + dest + kPartExt + L"\": " +
                      NdmHttpEngine::ErrorText(GetLastError());
                CloseReq(j, &j->kept);
                return OUT_FATAL;
            }
        }
        else
        {
            // Fresh download: name from server (or the user's name), never
            // overwrite anything (not even another download's .ndmpart)
            // unless the caller confirmed replacing the final file.
            std::wstring base = j->opt.userNamed ? curName
                                                 : ResolveName(pi, j->origUrl, curName);
            DWORD lastErr = 0;
            for (int k = 0; k < 10000 && h == INVALID_HANDLE_VALUE; ++k)
            {
                std::wstring cand = k ? AddSuffix(base, k) : base;
                std::wstring d = JoinPath(dir, cand);
                if (FileExists(d) && !(k == 0 && j->opt.overwriteConfirmed)) continue;
                h = CreateFileW((d + kPartExt).c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    lastErr = GetLastError();
                    if (lastErr == ERROR_FILE_EXISTS || lastErr == ERROR_ALREADY_EXISTS)
                        continue;
                    break;
                }
                curName = cand;
                dest    = d;
            }
            if (h == INVALID_HANDLE_VALUE)
            {
                err = L"Cannot create file in \"" + dir + L"\": " +
                      NdmHttpEngine::ErrorText(lastErr);
                CloseReq(j, &j->kept);
                return OUT_FATAL;
            }
            if (curName != NdmHttpEngine::GetSharedString(dl->filename))
            {
                NdmHttpEngine::SetSharedString(dl->filename, curName);
                if (j->opt.onChange) j->opt.onChange(dl, j->opt.onChangeCtx);
            }
        }
        j->hFile = h;
    }

    j->destPath  = dest;
    j->partPath  = dest + kPartExt;
    j->statePath = dest + kStateExt;

    // ---- 3. segments ------------------------------------------------------
    int conns = dl->numConnections;
    if (conns < 1) conns = 1;
    if (conns > kMaxConns) conns = kMaxConns;
    if (j->oneConn) conns = 1;

    AcquireSRWLockExclusive(&j->segLock);
    j->segs.clear();
    if (resume)
    {
        for (Seg s : st.segs) { s.active = false; if (s.pos < s.end) j->segs.push_back(s); }
        if (j->segs.empty())    // everything already on disk
        {
            Seg s; s.start = s.pos = s.done = s.end = j->total;
            j->segs.push_back(s);
        }
    }
    else if (j->singleMode)
    {
        Seg s;
        s.start = 0; s.pos = 0;
        s.end   = j->sizeKnown ? j->total : 0;
        j->segs.push_back(s);
    }
    else
    {
        UINT64 n = (UINT64)conns;
        UINT64 bySize = j->total / kMinSegSize;
        if (bySize < 1) bySize = 1;
        if (n > bySize) n = bySize;
        UINT64 segSize = j->total / n;
        for (UINT64 i = 0; i < n; ++i)
        {
            Seg s;
            s.start = i * segSize;
            s.end   = (i == n - 1) ? j->total : (i + 1) * segSize;
            s.pos   = s.start;
            s.done  = s.start;
            j->segs.push_back(s);
        }
    }
    UINT64 have = 0, remaining = 0;
    if (resume)
    {
        // bytes on disk = total - bytes still missing
        for (const Seg& s : j->segs) remaining += s.end - s.pos;
        have = j->total - remaining;
    }
    else
    {
        for (const Seg& s : j->segs) remaining += s.end - s.pos;
    }
    ReleaseSRWLockExclusive(&j->segLock);

    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&dl->downloadedSize), (LONG64)have);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&dl->totalSize),
                          (LONG64)(j->sizeKnown ? j->total : 0));
    PublishFileMap(j);

    // ---- 4. file preparation -----------------------------------------------
    if (j->sizeKnown && !resume)
    {
        ULARGE_INTEGER freeBytes = {};
        if (GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < remaining)
        {
            err = L"Not enough disk space (need " + NdmFormatSize(remaining) +
                  L", free " + NdmFormatSize(freeBytes.QuadPart) + L")";
            CloseJobFile(j);
            DeleteFileW(j->partPath.c_str());
            CloseReq(j, &j->kept);
            return OUT_FATAL;
        }
        SetSparse(j->hFile, true);   // avoid NTFS zero-fill stalls on big files
        if (!SetFileSize(j->hFile, j->total))
        {
            DWORD e = GetLastError();
            err = L"Cannot allocate file: " + NdmHttpEngine::ErrorText(e);
            CloseJobFile(j);
            DeleteFileW(j->partPath.c_str());
            CloseReq(j, &j->kept);
            return OUT_FATAL;
        }
    }
    SaveState(j);

    // Empty file / already complete
    if (j->sizeKnown && remaining == 0)
    {
        CloseReq(j, &j->kept);
        return Finalize(j, err);
    }

    // ---- 5. workers -------------------------------------------------------
    int nWorkers = 1;
    if (!j->singleMode)
    {
        UINT64 bySplit = remaining / kMinSplit;
        if (bySplit < 1) bySplit = 1;
        nWorkers = (int)std::min<UINT64>((UINT64)conns, bySplit);
        AcquireSRWLockShared(&j->segLock);
        int incomplete = (int)j->segs.size();
        ReleaseSRWLockShared(&j->segLock);
        if (nWorkers < std::min(incomplete, conns)) nWorkers = std::min(incomplete, conns);
    }
    if (nWorkers < 1) nWorkers = 1;
    // The probe response streams from byte 0: keep it only if a segment
    // still starts there (fresh download), so its connection takes it over.
    {
        bool zeroSeg = false;
        AcquireSRWLockShared(&j->segLock);
        for (const Seg& s : j->segs) if (s.pos == 0 && (j->openEnded || s.end > 0)) zeroSeg = true;
        ReleaseSRWLockShared(&j->segLock);
        if (j->singleMode && j->openEnded) zeroSeg = true;
        if (!zeroSeg) CloseReq(j, &j->kept);
    }

    j->alive = nWorkers;
    j->progressConns = 0;
    UpdateJobRate(j);
    std::vector<HANDLE> threads;
    for (int i = 0; i < nWorkers; ++i)
    {
        HANDLE t = CreateThread(nullptr, 0, WorkerProc, j, 0, nullptr);
        if (t) threads.push_back(t);
        else   InterlockedDecrement(&j->alive);
    }
    if (threads.empty())
    {
        err = L"Cannot create download thread: " + NdmHttpEngine::ErrorText(GetLastError());
        CloseJobFile(j);
        CloseReq(j, &j->kept);
        return OUT_TRANSIENT;
    }

    // ---- 6. monitor: speed + periodic state save ---------------------------
    ULONGLONG lastTick = GetTickCount64(), lastSave = lastTick;
    UINT64    lastBytes = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
    double    ema = 0.0;
    for (;;)
    {
        DWORD w = WaitForMultipleObjects((DWORD)threads.size(), threads.data(),
                                         TRUE, 500);
        ULONGLONG now = GetTickCount64();
        if (now - lastTick >= 400)
        {
            UINT64 cur = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
            UINT64 delta = cur >= lastBytes ? cur - lastBytes : 0;
            double inst = (double)delta * 1000.0 / (double)(now - lastTick);
            ema = (ema <= 0.0) ? inst : ema * 0.6 + inst * 0.4;
            dl->speed = (DWORD)std::min(ema, 4.0e9);
            lastBytes = cur;
            lastTick  = now;
            PublishFileMap(j);
            UpdateJobRate(j);       // limit / priority / share changes apply within ~1 s
        }
        if (now - lastSave >= 2000)
        {
            SaveState(j);
            lastSave = now;
        }
        if (w != WAIT_TIMEOUT) break;
    }
    for (HANDLE t : threads) CloseHandle(t);
    dl->speed = 0;
    CloseReq(j, &j->kept);
    PublishFileMap(j);
    InterlockedExchange(&dl->activeConns, 0);

    // ---- 7. evaluate ------------------------------------------------------
    bool complete = true;
    AcquireSRWLockShared(&j->segLock);
    if (j->openEnded)
        complete = j->eofReached != 0;
    else
        for (const Seg& s : j->segs) if (s.done < s.end) { complete = false; break; }
    ReleaseSRWLockShared(&j->segLock);

    if (complete && !j->rangeBroken)
    {
        if (!j->openEnded) SaveState(j);
        return Finalize(j, err);
    }

    SaveState(j);
    CloseJobFile(j);

    if (UserStopped(j)) return OUT_STOPPED;

    AcquireSRWLockShared(&j->errLock);
    bool hasErr = j->hasErr, fatal = j->errFatal, rb = j->rangeBroken;
    std::wstring e = j->err;
    ReleaseSRWLockShared(&j->errLock);

    if (rb) { err = e; return OUT_RANGE_BROKEN; }
    if (hasErr) { err = e; return fatal ? OUT_FATAL : OUT_TRANSIENT; }
    err = L"Download interrupted";
    return OUT_TRANSIENT;
}

} // namespace

// ===========================================================================
// Public: shared-field helpers
// ===========================================================================
void NdmHttpEngine::LockFields(bool exclusive)
{
    if (exclusive) AcquireSRWLockExclusive(&g_fieldLock);
    else           AcquireSRWLockShared(&g_fieldLock);
}

void NdmHttpEngine::UnlockFields(bool exclusive)
{
    if (exclusive) ReleaseSRWLockExclusive(&g_fieldLock);
    else           ReleaseSRWLockShared(&g_fieldLock);
}

void NdmHttpEngine::PrepareSharedStrings(NdmDownload* dl)
{
    if (!dl) return;
    LockFields(true);
    std::wstring* fields[2] = { &dl->filename, &dl->errorMsg };
    for (std::wstring* s : fields)
    {
        if (s->capacity() >= kSharedCap) continue;
        std::wstring v = *s;
        if (v.size() >= kSharedCap) v.resize(kSharedCap - 1);
        s->resize(kSharedCap, L'\0');   // fixed heap buffer, zero filled
        s->assign(v);                   // shrinks size, keeps capacity
    }
    UnlockFields(true);
}

void NdmHttpEngine::SetSharedString(std::wstring& field, const std::wstring& value)
{
    std::wstring v = value;
    if (v.size() >= kSharedCap) v.resize(kSharedCap - 1);
    LockFields(true);
    // With capacity >= kSharedCap, assign() copies in place: the buffer the
    // UI thread might be reading is never freed and stays NUL-terminated.
    field.assign(v);
    UnlockFields(true);
}

std::wstring NdmHttpEngine::GetSharedString(const std::wstring& field)
{
    LockFields(false);
    std::wstring v = field.c_str();
    UnlockFields(false);
    return v;
}

UINT64 NdmHttpEngine::AtomicRead64(const volatile UINT64* p)
{
    return (UINT64)InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(const_cast<volatile UINT64*>(p)), 0, 0);
}

void NdmHttpEngine::SetSpeedLimit(DWORD bps)
{
    LONG v = (LONG)std::min<DWORD>(bps, 0x7FFFFFFF);
    if (InterlockedExchange(&g_speedLimit, v) != v)
    {
        AcquireSRWLockExclusive(&g_flowLock);
        g_flowDirty = true;
        ReleaseSRWLockExclusive(&g_flowLock);
    }
}

DWORD NdmHttpEngine::GetSpeedLimit()
{
    return (DWORD)g_speedLimit;
}

void NdmHttpEngine::SetRetryPolicy(int maxRetries, int maxDelaySec)
{
    InterlockedExchange(&g_maxRetries, maxRetries < 0 ? 0 : (LONG)std::min(maxRetries, 1000));
    InterlockedExchange(&g_maxDelaySec, maxDelaySec < 1 ? 1 : (LONG)std::min(maxDelaySec, 3600));
}

// ===========================================================================
// Public: bandwidth allocator
// ===========================================================================
NdmHttpEngine::Flow* NdmHttpEngine::RegisterFlow(NdmDownload* dl, int kind)
{
    if (!dl) return nullptr;
    Flow* f = new Flow();
    f->dl    = dl;
    f->kind  = kind;
    f->since = GetTickCount64();
    AcquireSRWLockExclusive(&g_flowLock);
    g_flows.push_back(f);
    g_flowDirty = true;
    ReleaseSRWLockExclusive(&g_flowLock);
    return f;
}

void NdmHttpEngine::UnregisterFlow(Flow* f)
{
    if (!f) return;
    AcquireSRWLockExclusive(&g_flowLock);
    g_flows.erase(std::remove(g_flows.begin(), g_flows.end(), f), g_flows.end());
    g_flowDirty = true;
    ReleaseSRWLockExclusive(&g_flowLock);
    delete f;
}

DWORD NdmHttpEngine::FlowRate(Flow* f)
{
    if (!f) return 0;
    AcquireSRWLockExclusive(&g_flowLock);
    ULONGLONG now = GetTickCount64();
    if (g_flowDirty || now - g_flowTick >= 450) RecomputeFlows(now);
    DWORD r = f->alloc;
    ReleaseSRWLockExclusive(&g_flowLock);
    return r;
}

void NdmHttpEngine::SetFlowFixedRate(Flow* f, DWORD bps)
{
    if (!f) return;
    AcquireSRWLockExclusive(&g_flowLock);
    f->pinned = true;
    f->fixed  = bps;
    f->alloc  = bps;
    g_flowDirty = true;
    ReleaseSRWLockExclusive(&g_flowLock);
}

std::wstring NdmHttpEngine::SanitizeFilename(const std::wstring& in)
{
    std::wstring s = in;
    size_t slash = s.find_last_of(L"/\\");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);

    for (wchar_t& c : s)
        if (c < 32 || c == 0x7F || wcschr(L"<>:\"/\\|?*", c) || NdmIsDeceptiveChar(c)) c = L'_';

    size_t a = s.find_first_not_of(L' ');
    if (a == std::wstring::npos) return L"download";
    s = s.substr(a);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.empty()) return L"download";

    // Reserved DOS device names (CON, NUL, COM1, ...), with or without ext.
    std::wstring stem = s.substr(0, s.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (wchar_t& c : stem) c = (wchar_t)towupper(c);
    static const wchar_t* reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    for (const wchar_t* r : reserved)
        if (stem == r) { s = L"_" + s; break; }

    // Keep room for " (999)" + ".ndmstate.tmp" under the 255 component limit.
    const size_t kMaxLen = 200;
    if (s.size() > kMaxLen)
    {
        size_t dot = s.find_last_of(L'.');
        std::wstring ext = (dot != std::wstring::npos && s.size() - dot <= 16)
                               ? s.substr(dot) : L"";
        s = s.substr(0, kMaxLen - ext.size()) + ext;
    }
    return s;
}

std::wstring NdmHttpEngine::ErrorText(DWORD err)
{
    if (err == ERROR_WINHTTP_REDIRECT_FAILED)
        return L"Redirect refused (https to http downgrade, or too many redirects) (12156)";
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                  FORMAT_MESSAGE_FROM_SYSTEM;
    HMODULE mod = nullptr;
    if (err >= WINHTTP_ERROR_BASE && err <= WINHTTP_ERROR_LAST)
    {
        mod = GetModuleHandleW(L"winhttp.dll");
        if (mod) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    }
    DWORD n = FormatMessageW(flags, mod, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s;
    if (n && buf) s = buf;
    if (buf) LocalFree(buf);
    s = Trim(s);
    while (!s.empty() && s.back() == L'.') s.pop_back();
    if (s.empty())
    {
        switch (err)
        {
        case ERROR_WINHTTP_TIMEOUT:             s = L"The connection timed out"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:   s = L"Server name could not be resolved"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT:      s = L"Cannot connect to server"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR:    s = L"Connection reset or aborted"; break;
        case ERROR_WINHTTP_OPERATION_CANCELLED: s = L"Operation cancelled"; break;
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: s = L"Unsupported URL scheme (only http/https)"; break;
        case ERROR_WINHTTP_INVALID_URL:         s = L"Invalid URL"; break;
        case ERROR_WINHTTP_SECURE_FAILURE:      s = L"Secure connection failed (certificate or TLS error)"; break;
        case ERROR_WINHTTP_REDIRECT_FAILED:     s = L"Redirect refused (https to http, or too many redirects)"; break;
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: s = L"Invalid server response"; break;
        default:                                s = L"Error"; break;
        }
    }
    return s + L" (" + std::to_wstring(err) + L")";
}

// ===========================================================================
// Public: Run
// ===========================================================================
// One WinHTTP session per run: it owns the socket pool (keep-alive and
// TLS session reuse across segments of this download). Automatic proxy
// = system/IE settings + WPAD (Windows 8.1+), fallback for older systems.
static HINTERNET OpenSession(const std::wstring& ua)
{
    HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return nullptr;
    // resolve (no limit), connect, send, receive (per read: stall detection)
    WinHttpSetTimeouts(s, 0, 20000, 30000, 30000);
    // The default per-server connection cap would serialize segments.
    DWORD v = 64;
    WinHttpSetOption(s, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &v, sizeof(v));
    WinHttpSetOption(s, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &v, sizeof(v));
    // Redirects are followed manually (see OpenRequest) so cookies and
    // referrer never leak to another origin and https never downgrades.
    v = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &v, sizeof(v));
    // TLS 1.0-1.3 (1.3 flag is rejected on systems without it).
    v = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /*TLS1_3*/;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &v, sizeof(v)))
    {
        v &= ~0x00002000u;
        WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &v, sizeof(v));
    }
    // HTTP/2 stays off (WinHTTP default): parallel h2 streams would share
    // one TCP connection, which defeats multi-connection downloading.
    // Content decoding stays off too: byte ranges refer to raw bytes.
    return s;
}

NdmRunResult NdmHttpEngine::Run(NdmDownload* dl, const NdmRunOptions& opt)
{
    if (!dl) return NDM_RUN_ERROR;
    PrepareSharedStrings(dl);

    if (!dl->hStopEvent)
        dl->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (opt.setStatus) dl->status = NDM_STATUS_DOWNLOADING;
    SetSharedString(dl->errorMsg, L"");
    dl->speed = 0;

    Job j;
    j.dl     = dl;
    j.opt    = opt;
    j.hStop  = dl->hStopEvent;
    j.hAbort = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    j.origUrl = CleanUrl(dl->url);

    std::wstring ref = NoCrLf(dl->referrer);
    std::wstring ck  = NoCrLf(dl->cookies);
    std::wstring ua  = NoCrLf(dl->userAgent);
    if (ua.empty()) ua = kDefaultUA;
    j.referrer = ref;
    if (!ck.empty())
    {
        j.cookieHdr = L"Cookie: " + ck + L"\r\n";
        j.ownCookies = true;
    }

    int errKind = NDM_ERRKIND_FATAL;
    auto finish = [&](NdmRunResult r, const std::wstring& msg) -> NdmRunResult
    {
        if (r == NDM_RUN_ERROR) SetSharedString(dl->errorMsg, msg);
        else                    SetSharedString(dl->errorMsg, L"");
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&dl->errorKind),
                            r == NDM_RUN_ERROR ? errKind : NDM_ERRKIND_NONE);
        dl->speed = 0;
        if (opt.setStatus)
            dl->status = (r == NDM_RUN_COMPLETED) ? NDM_STATUS_COMPLETED
                       : (r == NDM_RUN_STOPPED)   ? NDM_STATUS_PAUSED
                                                  : NDM_STATUS_ERROR;
        return r;
    };

    if (!j.hStop || !j.hAbort)
    {
        if (j.hAbort) CloseHandle(j.hAbort);
        return finish(NDM_RUN_ERROR, L"Cannot create synchronization objects");
    }
    if (j.origUrl.empty())
    {
        CloseHandle(j.hAbort);
        return finish(NDM_RUN_ERROR, L"Empty URL");
    }

    j.hSession = OpenSession(ua);
    if (!j.hSession)
    {
        std::wstring m = L"Cannot initialize WinHTTP: " + ErrorText(GetLastError());
        CloseHandle(j.hAbort);
        return finish(NDM_RUN_ERROR, m);
    }

    // Cancel blocking WinHTTP calls as soon as the stop event fires.
    HANDLE hWait = nullptr;
    RegisterWaitForSingleObject(&hWait, j.hStop, StopCallback, &j, INFINITE,
                                WT_EXECUTEONLYONCE);

    // This run's share of the bandwidth (registered for the whole run).
    NdmFlowScope flow(dl, NDM_KIND_HTTP);
    j.flow = flow.get();

    NdmRunResult res = NDM_RUN_ERROR;
    std::wstring lastErr;
    int  attempts    = 0;
    bool forceSingle = false;
    const int maxRetries = (int)g_maxRetries;                 // 0 = unlimited
    const DWORD maxDelay = (DWORD)g_maxDelaySec * 1000u;
    for (;;)
    {
        // reset per-attempt state (all workers of the previous attempt exited)
        ResetEvent(j.hAbort);
        AcquireSRWLockExclusive(&j.reqLock);
        j.cancelled = UserStopped(&j);
        ReleaseSRWLockExclusive(&j.reqLock);
        AcquireSRWLockExclusive(&j.errLock);
        j.hasErr = j.errFatal = j.rangeBroken = false;
        j.err.clear();
        ReleaseSRWLockExclusive(&j.errLock);

        if (UserStopped(&j)) { res = NDM_RUN_STOPPED; break; }

        UINT64 before = AtomicRead64(&dl->downloadedSize);
        std::wstring err;
        Outcome o = RunOnce(&j, forceSingle, err);
        CloseJobFile(&j);

        if (o == OUT_OK)      { res = NDM_RUN_COMPLETED; break; }
        if (o == OUT_STOPPED || UserStopped(&j)) { res = NDM_RUN_STOPPED; break; }
        if (o == OUT_RANGE_BROKEN && !forceSingle)
        {
            forceSingle = true;          // fall back to one connection
            continue;
        }
        if (o == OUT_FATAL || o == OUT_RANGE_BROKEN)
        {
            lastErr = err; res = NDM_RUN_ERROR; break;
        }
        // transient
        if (AtomicRead64(&dl->downloadedSize) > before) attempts = 0;
        ++attempts;
        if (maxRetries > 0 && attempts > maxRetries)
        {
            lastErr = err; res = NDM_RUN_ERROR; errKind = NDM_ERRKIND_TRANSIENT; break;
        }
        SetSharedString(dl->errorMsg, L"Retrying (" + std::to_wstring(attempts) +
                                      (maxRetries > 0 ? L"/" + std::to_wstring(maxRetries) : L"") +
                                      L"): " + err);
        DWORD wait = std::min<DWORD>(maxDelay, 3000u << std::min(attempts - 1, 12));
        if (WaitForSingleObject(j.hStop, wait) == WAIT_OBJECT_0)
        {
            res = NDM_RUN_STOPPED; break;
        }
    }

    if (hWait) UnregisterWaitEx(hWait, INVALID_HANDLE_VALUE);  // waits for callback
    CloseReq(&j, &j.kept);
    WinHttpCloseHandle(j.hSession);
    CloseHandle(j.hAbort);
    return finish(res, lastErr);
}

// ===========================================================================
// Public: temp-file cleanup
// ===========================================================================
void NdmHttpEngine::DeleteTempFiles(const NdmDownload* dl)
{
    if (!dl) return;
    std::wstring name = SanitizeFilename(GetSharedString(dl->filename));
    std::wstring dest = JoinPath(dl->savePath, name);
    StateData st;
    if (LoadState(dest + kStateExt, st) && st.id == dl->id &&
        st.url == CleanUrl(dl->url))
    {
        DeleteFileW((dest + kPartExt).c_str());
        DeleteFileW((dest + kStateExt).c_str());
        DeleteFileW((dest + kStateExt + std::wstring(L".tmp")).c_str());
    }
}

// ===========================================================================
// Public: address refresh support
// ===========================================================================
std::wstring NdmHttpEngine::NormalizeUrl(const std::wstring& url)
{
    return CleanUrl(url);
}

bool NdmHttpEngine::SameRemoteFile(UINT64 totalA, const std::wstring& etagA, const std::wstring& lmA,
                                   UINT64 totalB, const std::wstring& etagB, const std::wstring& lmB)
{
    if (totalA != totalB) return false;
    const bool lmBoth   = !lmA.empty() && !lmB.empty();
    const bool etagBoth = !etagA.empty() && !etagB.empty();
    if (lmBoth && lmA != lmB) return false;
    if (etagBoth && etagA != etagB && !lmBoth) return false;
    return true;
}

bool NdmHttpEngine::ProbeUrl(const std::wstring& url, const std::wstring& referrer,
                             const std::wstring& cookies, const std::wstring& userAgent,
                             ProbeResult& out)
{
    out = ProbeResult();
    Job j;
    j.origUrl  = CleanUrl(url);
    j.referrer = NoCrLf(referrer);
    std::wstring ck = NoCrLf(cookies);
    if (!ck.empty()) { j.cookieHdr = L"Cookie: " + ck + L"\r\n"; j.ownCookies = true; }
    std::wstring ua = NoCrLf(userAgent);
    if (ua.empty()) ua = kDefaultUA;
    if (j.origUrl.empty())
    {
        out.netError = ERROR_WINHTTP_INVALID_URL;
        out.message  = ErrorText(out.netError);
        return false;
    }
    j.hStop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    j.hAbort   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    j.hSession = (j.hStop && j.hAbort) ? OpenSession(ua) : nullptr;
    if (!j.hSession)
    {
        out.netError = GetLastError() ? GetLastError() : ERROR_WINHTTP_INTERNAL_ERROR;
        out.message  = L"Cannot initialize WinHTTP: " + ErrorText(out.netError);
    }
    else
    {
        Req r;
        DWORD status = 0;
        DWORD e = OpenRequest(&j, j.origUrl, L"Range: bytes=0-0\r\n", r, status);
        if (e)
        {
            out.netError = e;
            out.message  = ErrorText(e);
        }
        else
        {
            HINTERNET h = GetReq(&j, &r);
            out.status = status;
            if (h)
            {
                ProbeInfo pi;
                ReadCommonHeaders(h, pi);
                out.etag     = pi.etag;
                out.lastMod  = pi.lastMod;
                out.finalUrl = pi.finalUrl.empty() ? j.origUrl : pi.finalUrl;
                if (status == 206)
                {
                    bool hasRange = false, tk = false;
                    UINT64 a = 0, b = 0, t = 0;
                    if (ParseContentRange(QueryStr(h, WINHTTP_QUERY_CONTENT_RANGE), hasRange, a, b, tk, t) &&
                        hasRange && a == 0 && tk)
                    {
                        out.rangesOk = true; out.sizeKnown = true; out.total = t;
                    }
                }
                else if (status == 200 || status == 203)
                {
                    ReadContentLength(h, pi);            // body is never read
                    out.sizeKnown = pi.sizeKnown; out.total = pi.total;
                }
                else
                    out.message = HttpErrText(h, status);
            }
            CloseReq(&j, &r);
        }
        WinHttpCloseHandle(j.hSession);
    }
    if (j.hStop)  CloseHandle(j.hStop);
    if (j.hAbort) CloseHandle(j.hAbort);
    return out.netError == 0 && (out.status == 200 || out.status == 203 || out.status == 206);
}

bool NdmHttpEngine::ReadStateInfo(const NdmDownload* dl, UINT64& total, std::wstring& etag,
                                  std::wstring& lastMod, UINT64& bytesOnDisk)
{
    total = 0; bytesOnDisk = 0; etag.clear(); lastMod.clear();
    if (!dl) return false;
    std::wstring dest = JoinPath(dl->savePath, SanitizeFilename(GetSharedString(dl->filename)));
    StateData st;
    if (!LoadState(dest + kStateExt, st) || st.id != dl->id || st.url != CleanUrl(dl->url) ||
        !FileExists(dest + kPartExt))
        return false;
    total = st.total; etag = st.etag; lastMod = st.lastMod;
    bytesOnDisk = BytesOnDisk(st);
    return true;
}

bool NdmHttpEngine::RebindState(const NdmDownload* dl, const std::wstring& newUrl)
{
    if (!dl) return false;
    std::wstring dest = JoinPath(dl->savePath, SanitizeFilename(GetSharedString(dl->filename)));
    std::wstring path = dest + kStateExt;
    StateData st;
    if (!FileExists(path)) return true;                          // nothing on disk yet
    if (!LoadState(path, st) || st.id != dl->id || st.url != CleanUrl(dl->url))
        return true;                                             // not ours: leave it alone

    std::string raw;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        UINT64 sz = GetFileSize64(h);
        raw.resize((size_t)sz);
        DWORD rd = 0;
        BOOL ok = sz ? ReadFile(h, &raw[0], (DWORD)sz, &rd, nullptr) : TRUE;
        CloseHandle(h);
        if (!ok || rd != sz) return false;
    }
    std::wstring text;
    if (!TryUtf8(raw, text)) return false;
    std::wstring out;
    bool replaced = false;
    size_t p = 0;
    while (p < text.size())
    {
        size_t e = text.find(L'\n', p);
        std::wstring line = text.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p + 1);
        p = (e == std::wstring::npos) ? text.size() : e + 1;
        if (!replaced && line.compare(0, 4, L"url=") == 0)
        {
            line = L"url=" + CleanUrl(newUrl) + L"\n";
            replaced = true;
        }
        out += line;
    }
    if (!replaced) return false;
    std::string data = WideToUtf8(out);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || w != data.size() ||
        !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
