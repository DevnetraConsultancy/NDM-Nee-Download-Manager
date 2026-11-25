#include "ndm_common.h"
#include <cwchar>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cwctype>

// ---------------------------------------------------------------------------
// NdmFormatSize — human-readable byte count
// ---------------------------------------------------------------------------
std::wstring NdmFormatSize(UINT64 bytes)
{
    wchar_t buf[64];

    if (bytes == 0)
    {
        return L"0 B";
    }
    else if (bytes < 1024ULL)
    {
        swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
    }
    else if (bytes < 1024ULL * 1024ULL)
    {
        double kb = (double)bytes / 1024.0;
        swprintf_s(buf, L"%.2f KB", kb);
    }
    else if (bytes < 1024ULL * 1024ULL * 1024ULL)
    {
        double mb = (double)bytes / (1024.0 * 1024.0);
        swprintf_s(buf, L"%.2f MB", mb);
    }
    else
    {
        double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
        swprintf_s(buf, L"%.2f GB", gb);
    }

    return std::wstring(buf);
}

// ---------------------------------------------------------------------------
// NdmFormatSpeed — human-readable transfer speed
// ---------------------------------------------------------------------------
std::wstring NdmFormatSpeed(DWORD bytesPerSec)
{
    wchar_t buf[64];

    if (bytesPerSec == 0)
    {
        return L"0 B/s";
    }
    else if (bytesPerSec < 1024UL)
    {
        swprintf_s(buf, L"%lu B/s", (unsigned long)bytesPerSec);
    }
    else if (bytesPerSec < 1024UL * 1024UL)
    {
        double kb = (double)bytesPerSec / 1024.0;
        swprintf_s(buf, L"%.1f KB/s", kb);
    }
    else
    {
        double mb = (double)bytesPerSec / (1024.0 * 1024.0);
        swprintf_s(buf, L"%.1f MB/s", mb);
    }

    return std::wstring(buf);
}

// ---------------------------------------------------------------------------
// NdmFormatTime — human-readable time remaining
// ---------------------------------------------------------------------------
std::wstring NdmFormatTime(DWORD seconds)
{
    if (seconds == 0)
        return L"\u221E";   // ∞ — cannot estimate

    wchar_t buf[64];

    if (seconds < 60)
    {
        swprintf_s(buf, L"%us", (unsigned)seconds);
    }
    else if (seconds < 3600)
    {
        DWORD m = seconds / 60;
        DWORD s = seconds % 60;
        swprintf_s(buf, L"%um %02us", (unsigned)m, (unsigned)s);
    }
    else
    {
        DWORD h = seconds / 3600;
        DWORD m = (seconds % 3600) / 60;
        swprintf_s(buf, L"%uh %02um", (unsigned)h, (unsigned)m);
    }

    return std::wstring(buf);
}

// ---------------------------------------------------------------------------
// NdmSafeFilename — derive a safe local filename from a URL
// ---------------------------------------------------------------------------
static int NdmHexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring NdmSafeFilename(const std::wstring& url)
{
    // Strip fragment and query string
    std::wstring path = url;

    size_t h = path.find(L'#');
    if (h != std::wstring::npos)
        path = path.substr(0, h);

    size_t q = path.find(L'?');
    if (q != std::wstring::npos)
        path = path.substr(0, q);

    // Drop trailing slashes, then take the last path component
    // (but never the host part of "scheme://host").
    size_t schemeEnd = path.find(L"://");
    size_t pathStart = (schemeEnd == std::wstring::npos) ? 0
                       : path.find(L'/', schemeEnd + 3);
    if (schemeEnd != std::wstring::npos && pathStart == std::wstring::npos)
        return L"download";                       // no path at all
    while (path.size() > pathStart + 1 && (path.back() == L'/' || path.back() == L'\\'))
        path.pop_back();

    size_t slash = path.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        path = path.substr(slash + 1);

    // URL-decode %XX sequences as UTF-8 bytes (falls back to Latin-1)
    std::string bytes;
    bytes.reserve(path.size());
    bool nonAscii = false;
    for (size_t i = 0; i < path.size(); ++i)
    {
        wchar_t c = path[i];
        if (c == L'%' && i + 2 < path.size())
        {
            int hi = NdmHexVal(path[i + 1]);
            int lo = NdmHexVal(path[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                bytes += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (c < 0x80)
            bytes += (char)c;
        else
        {
            // already-decoded non-ASCII char: encode as UTF-8
            char tmp[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, &path[i], 1, tmp, sizeof(tmp),
                                        nullptr, nullptr);
            bytes.append(tmp, n > 0 ? n : 0);
            nonAscii = true;
        }
    }
    (void)nonAscii;

    std::wstring decoded;
    if (!bytes.empty())
    {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    bytes.data(), (int)bytes.size(), nullptr, 0);
        UINT cp = CP_UTF8;
        DWORD fl = MB_ERR_INVALID_CHARS;
        if (n <= 0) { cp = 28591; fl = 0;
            n = MultiByteToWideChar(cp, fl, bytes.data(), (int)bytes.size(), nullptr, 0); }
        if (n > 0)
        {
            decoded.resize(n);
            MultiByteToWideChar(cp, fl, bytes.data(), (int)bytes.size(), &decoded[0], n);
        }
    }

    // Replace characters forbidden in Windows filenames
    const std::wstring forbidden = L"<>:\"/\\|?*";
    for (wchar_t& c : decoded)
    {
        if (forbidden.find(c) != std::wstring::npos || (unsigned)c < 32 || c == 0x7F ||
            NdmIsDeceptiveChar(c))
            c = L'_';
    }

    // Trim leading spaces, trailing spaces and dots
    size_t start = decoded.find_first_not_of(L' ');
    if (start == std::wstring::npos)
        return L"download";
    decoded = decoded.substr(start);
    while (!decoded.empty() && (decoded.back() == L' ' || decoded.back() == L'.'))
        decoded.pop_back();
    if (decoded.empty())
        return L"download";

    // Reserved device names
    std::wstring stem = decoded.substr(0, decoded.find(L'.'));
    for (wchar_t& c : stem) c = (wchar_t)towupper(c);
    static const wchar_t* reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    for (const wchar_t* r : reserved)
        if (stem == r) { decoded = L"_" + decoded; break; }

    // Length limit (keep extension)
    if (decoded.size() > 200)
    {
        size_t dot = decoded.find_last_of(L'.');
        std::wstring ext = (dot != std::wstring::npos && decoded.size() - dot <= 16)
                               ? decoded.substr(dot) : L"";
        decoded = decoded.substr(0, 200 - ext.size()) + ext;
    }

    return decoded;
}

// ---------------------------------------------------------------------------
// NdmMarkOfTheWeb
// ---------------------------------------------------------------------------
#include <objbase.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")

static std::string NdmToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Only http(s) URLs are recorded; strip CR/LF and user:pass@ from them.
static std::wstring NdmMotwUrl(const std::wstring& u)
{
    if (!(_wcsnicmp(u.c_str(), L"http://", 7) == 0 || _wcsnicmp(u.c_str(), L"https://", 8) == 0))
        return std::wstring();
    std::wstring r;
    for (wchar_t c : u) if (c != L'\r' && c != L'\n') r += c;
    size_t s = r.find(L"://") + 3;
    size_t e = r.find_first_of(L"/?#", s);
    size_t at = r.rfind(L'@', e == std::wstring::npos ? r.size() : e);
    if (at != std::wstring::npos && at >= s) r.erase(s, at + 1 - s);
    return r;
}

bool NdmMarkOfTheWeb(const std::wstring& filePath,
                     const std::wstring& sourceUrl,
                     const std::wstring& referrer)
{
    if (filePath.empty()) return false;
    const std::wstring src = NdmMotwUrl(sourceUrl);
    const std::wstring ref = NdmMotwUrl(referrer);

    bool ok = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAttachmentExecute* ae = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&ae))))
    {
        // {6C8F6F5B-8A39-4C2E-9D3E-2B0F6E0E4E11} — NDM client id (per-app prompts/policy)
        static const GUID kNdmGuid = { 0x6c8f6f5b, 0x8a39, 0x4c2e, { 0x9d, 0x3e, 0x2b, 0x0f, 0x6e, 0x0e, 0x4e, 0x11 } };
        ae->SetClientGuid(kNdmGuid);
        ae->SetLocalPath(filePath.c_str());
        if (!src.empty()) ae->SetSource(src.c_str());
        if (!ref.empty()) ae->SetReferrer(ref.c_str());
        HRESULT hr = ae->Save();   // writes Zone.Identifier per policy (may also AV-scan)
        ok = SUCCEEDED(hr);
        ae->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    // Policy may make Save() succeed without writing a zone (e.g. no source
    // URL for a magnet torrent): only trust it when the stream exists.
    if (ok && GetFileAttributesW((filePath + L":Zone.Identifier").c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // Fallback: write the ADS directly.
    std::string body = "[ZoneTransfer]\r\nZoneId=3\r\n";
    if (!ref.empty()) body += "ReferrerUrl=" + NdmToUtf8(ref) + "\r\n";
    if (!src.empty()) body += "HostUrl=" + NdmToUtf8(src) + "\r\n";
    HANDLE h = CreateFileW((filePath + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // e.g. FAT32: no streams
    DWORD wr = 0;
    BOOL w = WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);
    return w && wr == body.size();
}

// ---------------------------------------------------------------------------
// URL classification
// ---------------------------------------------------------------------------
static bool NdmHasPrefixI(const std::wstring& s, const wchar_t* pre)
{
    size_t n = wcslen(pre);
    return s.size() > n && _wcsnicmp(s.c_str(), pre, n) == 0;
}

static bool NdmNoCtlOrSpace(const std::wstring& s)
{
    for (wchar_t c : s)
        if (c <= 0x20 || c == 0x7F) return false;
    return true;
}

bool NdmIsHttpUrl(const std::wstring& s)
{
    return s.size() < 32768 && NdmNoCtlOrSpace(s) &&
           (NdmHasPrefixI(s, L"http://") || NdmHasPrefixI(s, L"https://"));
}

bool NdmIsNetOrMagnetUrl(const std::wstring& s)
{
    if (NdmIsHttpUrl(s)) return true;
    return s.size() < 65536 && NdmNoCtlOrSpace(s) && NdmHasPrefixI(s, L"magnet:?");
}

// ---------------------------------------------------------------------------
// NdmIsElevated
// ---------------------------------------------------------------------------
bool NdmIsElevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te = {};
    DWORD got = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &got) && te.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

// ---------------------------------------------------------------------------
// DPAPI helpers (queue.xml secrets)
// ---------------------------------------------------------------------------
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

static const char kNdmEntropy[] = "NDM-queue-secret-v1";

std::wstring NdmProtectString(const std::wstring& plain)
{
    if (plain.empty()) return std::wstring();
    std::string u = NdmToUtf8(plain);
    DATA_BLOB in  = { (DWORD)u.size(), reinterpret_cast<BYTE*>(&u[0]) };
    DATA_BLOB ent = { (DWORD)(sizeof(kNdmEntropy) - 1), (BYTE*)kNdmEntropy };
    DATA_BLOB out = {};
    std::wstring b64;
    if (CryptProtectData(&in, L"NDM", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    {
        DWORD n = 0;
        if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 nullptr, &n) && n)
        {
            b64.resize(n);
            if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                     &b64[0], &n))
                b64.resize(n);
            else
                b64.clear();
        }
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    SecureZeroMemory(&u[0], u.size());
    return b64;
}

bool NdmUnprotectString(const std::wstring& b64, std::wstring& plain)
{
    plain.clear();
    if (b64.empty()) return true;
    DWORD n = 0;
    if (!CryptStringToBinaryW(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, nullptr, &n,
                              nullptr, nullptr) || !n)
        return false;
    std::vector<BYTE> bin(n);
    if (!CryptStringToBinaryW(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, bin.data(), &n,
                              nullptr, nullptr))
        return false;
    DATA_BLOB in  = { n, bin.data() };
    DATA_BLOB ent = { (DWORD)(sizeof(kNdmEntropy) - 1), (BYTE*)kNdmEntropy };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    if (out.cbData)
    {
        int wn = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(out.pbData),
                                     (int)out.cbData, nullptr, 0);
        if (wn > 0)
        {
            plain.resize((size_t)wn);
            MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(out.pbData),
                                (int)out.cbData, &plain[0], wn);
        }
        SecureZeroMemory(out.pbData, out.cbData);
    }
    LocalFree(out.pbData);
    return true;
}

// ---------------------------------------------------------------------------
// Deceptive (bidi / invisible) characters in file names
// ---------------------------------------------------------------------------
bool NdmIsDeceptiveChar(wchar_t c)
{
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0x061C || c == 0xFEFF;
}

// ---------------------------------------------------------------------------
// Referrer policy: strict-origin-when-cross-origin
// ---------------------------------------------------------------------------
namespace {
struct NdmOrigin
{
    bool         https     = false;
    std::wstring host;          // lower case ("[v6]" kept bracketed)
    unsigned     port      = 0;
    size_t       authStart = 0; // index just after "://"
    size_t       userEnd   = 0; // index just after "user:pass@" (== authStart when none)
};

bool NdmParseOrigin(const std::wstring& u, NdmOrigin& o)
{
    if (u.empty() || u.size() >= 32768) return false;
    for (wchar_t c : u) if (c <= 0x20 || c == 0x7F) return false;
    if (_wcsnicmp(u.c_str(), L"https://", 8) == 0)     { o.https = true;  o.authStart = 8; }
    else if (_wcsnicmp(u.c_str(), L"http://", 7) == 0) { o.https = false; o.authStart = 7; }
    else return false;
    size_t e = u.find_first_of(L"/?#\\", o.authStart);
    if (e == std::wstring::npos) e = u.size();
    std::wstring auth = u.substr(o.authStart, e - o.authStart);
    size_t at = auth.rfind(L'@');
    o.userEnd = o.authStart + (at == std::wstring::npos ? 0 : at + 1);
    std::wstring hp = at == std::wstring::npos ? auth : auth.substr(at + 1);
    std::wstring host, port;
    if (!hp.empty() && hp[0] == L'[') {
        size_t rb = hp.find(L']');
        if (rb == std::wstring::npos) return false;
        host = hp.substr(0, rb + 1);
        if (rb + 1 < hp.size()) {
            if (hp[rb + 1] != L':') return false;
            port = hp.substr(rb + 2);
        }
    } else {
        size_t colon = hp.find(L':');
        host = hp.substr(0, colon);
        if (colon != std::wstring::npos) port = hp.substr(colon + 1);
    }
    while (!host.empty() && host.back() == L'.') host.pop_back();
    if (host.empty()) return false;
    for (wchar_t& c : host) c = (wchar_t)towlower(c);
    o.host = host;
    o.port = o.https ? 443u : 80u;
    if (!port.empty()) {
        unsigned long v = 0;
        for (wchar_t c : port) {
            if (c < L'0' || c > L'9') return false;
            v = v * 10 + (unsigned long)(c - L'0');
            if (v > 65535) return false;
        }
        o.port = (unsigned)v;
    }
    return true;
}
} // namespace

std::wstring NdmReferrerFor(const std::wstring& referrer, const std::wstring& target)
{
    NdmOrigin r, t;
    if (!NdmParseOrigin(referrer, r) || !NdmParseOrigin(target, t)) return std::wstring();
    if (r.https && !t.https) return std::wstring();                 // https page -> http: nothing
    if (r.https == t.https && r.port == t.port && r.host == t.host) {
        // same origin: full referrer minus credentials and fragment
        std::wstring full = referrer.substr(0, r.authStart) + referrer.substr(r.userEnd);
        size_t hash = full.find(L'#');
        if (hash != std::wstring::npos) full.resize(hash);
        if (full.size() > 4096) full.resize(4096);
        return full;
    }
    std::wstring o = std::wstring(r.https ? L"https://" : L"http://") + r.host;
    if (r.port != (r.https ? 443u : 80u)) o += L":" + std::to_wstring(r.port);
    return o + L"/";
}

// ---------------------------------------------------------------------------
// Browser request hand-off (token in LPARAM, never a heap pointer)
// ---------------------------------------------------------------------------
#include <mutex>
#include <memory>

namespace {
std::mutex& NdmReqLock() { static std::mutex m; return m; }
std::map<LPARAM, std::unique_ptr<NdmBrowserRequest>>& NdmReqMap()
{
    static std::map<LPARAM, std::unique_ptr<NdmBrowserRequest>> m;
    return m;
}
LONG64 g_ndmReqNext = 0;
} // namespace

bool NdmPostBrowserRequest(HWND hWnd, NdmBrowserRequest* req)
{
    std::unique_ptr<NdmBrowserRequest> own(req);
    if (!own || !hWnd) return false;
    LPARAM token = 0;
    {
        std::lock_guard<std::mutex> g(NdmReqLock());
        auto& m = NdmReqMap();
        if (m.size() >= 4096) return false;            // main window is not draining: drop
        if (g_ndmReqNext == 0) {
            // random-ish start: a token is not a guessable small number
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            g_ndmReqNext = ((q.QuadPart ^ ((LONG64)GetCurrentProcessId() << 24)) & 0x3FFFFFFFFFFFLL) | 0x100000;
        }
        token = (LPARAM)(++g_ndmReqNext);
        m[token] = std::move(own);
    }
    if (PostMessageW(hWnd, WM_NDM_BROWSER_MSG, 0, token)) return true;
    std::lock_guard<std::mutex> g(NdmReqLock());
    NdmReqMap().erase(token);
    return false;
}

NdmBrowserRequest* NdmTakeBrowserRequest(LPARAM token)
{
    std::lock_guard<std::mutex> g(NdmReqLock());
    auto& m = NdmReqMap();
    auto it = m.find(token);
    if (it == m.end()) return nullptr;
    NdmBrowserRequest* r = it->second.release();
    m.erase(it);
    return r;
}

void NdmDiscardBrowserRequests()
{
    std::lock_guard<std::mutex> g(NdmReqLock());
    NdmReqMap().clear();
}
