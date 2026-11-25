// ---------------------------------------------------------------------------
// ndm_torrent.cpp -- BitTorrent / magnet engine on top of the bundled aria2c
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
//  * One shared aria2c.exe per NDM session, started lazily, hidden
//    (CREATE_NO_WINDOW) inside a kill-on-close job object (+ --stop-with-process)
//    so it can never outlive NDM. JSON-RPC on 127.0.0.1:<free port> with a
//    random secret, spoken over WinHTTP. The secret is never on the command
//    line (readable by any process of the session): aria2c gets it from a
//    --conf-path file in %LOCALAPPDATA%\Devnetra\NDM\run\ (DACL: the user +
//    SYSTEM only), deleted as soon as the RPC endpoint answers.
//  * Session file %APPDATA%\Devnetra\NDM\aria2.session (--input-file /
//    --save-session) restores downloads that were running when NDM died; they
//    are paused at start-up and adopted by NdmTorrentRun (matched by info hash
//    + folder).
//  * Pause / stop = forceRemove (files and the .aria2 control file stay), resume
//    = re-add with continue=true; aria2 picks up from the control file. Torrent
//    metadata is cached in %APPDATA%\Devnetra\NDM\torrents\<hash>.torrent so a
//    resumed magnet starts instantly.
// ---------------------------------------------------------------------------
#define NDM_HTTP_IMPL               // keep wininet.h out (conflicts with winhttp.h)
#include "ndm_torrent.h"
#include "ndm_torrent_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <memory>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
// ===========================================================================
// Small helpers
// ===========================================================================
std::string W2U(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring U2W(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)   // not UTF-8: fall back to the ANSI code page
    {
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

void Set64(volatile UINT64* p, UINT64 v) { InterlockedExchange64((volatile LONG64*)p, (LONG64)v); }

bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

std::wstring DirOf(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring NormDir(std::wstring d)
{
    for (auto& c : d) if (c == L'/') c = L'\\';
    while (d.size() > 3 && d.back() == L'\\') d.pop_back();
    for (auto& c : d) c = (wchar_t)towlower(c);
    return d;
}

std::wstring AppDataDir()
{
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)) || !buf[0])
        return std::wstring();
    std::wstring d = std::wstring(buf) + L"\\Devnetra";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\NDM";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return DirOf(std::wstring(buf, n));
}

std::wstring Aria2Path()
{
    std::wstring exe = ExeDir();
#ifdef NDM_DEV_BUILD
    // dev builds only: <repo>\third_party (release builds embed no build paths)
    std::wstring repo = DirOf(DirOf(DirOf(U2W(__FILE__))));     // <repo>\src\torrent\x.cpp
#endif
    const std::wstring cands[] = {
        exe + L"\\tools\\aria2c.exe",
        exe + L"\\aria2c.exe",
#ifdef NDM_DEV_BUILD
        repo.empty() ? std::wstring() : repo + L"\\third_party\\aria2c.exe",
#endif
    };
    for (const auto& c : cands)
        if (!c.empty() && c[0] != L'\\' && FileExists(c)) return c;
    return std::wstring();
}

std::string RandomHex(int bytes)
{
    std::vector<unsigned char> b(bytes);
    if (BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        for (auto& x : b) x = (unsigned char)(GetTickCount64() * 2654435761u >> 13);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (unsigned char x : b) { s += hx[x >> 4]; s += hx[x & 15]; }
    return s;
}

std::string Sha1Hex(const char* data, size_t len)
{
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return out;
    unsigned char h[20] = {};
    if (BCryptHash(alg, nullptr, 0, (PUCHAR)data, (ULONG)len, h, 20) == 0)
    {
        static const char* hx = "0123456789abcdef";
        for (unsigned char x : h) { out += hx[x >> 4]; out += hx[x & 15]; }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string Base64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size())
    {
        unsigned v = (unsigned char)in[i] << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    }
    else if (i + 2 == in.size())
    {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}

std::string JStr(const std::string& s)      // JSON string literal
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o + "\"";
}
std::string JStr(const std::wstring& s) { return JStr(W2U(s)); }

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (UINT64)sz.QuadPart <= maxBytes;
    if (ok)
    {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const std::string& data)
{
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// ===========================================================================
// Minimal JSON reader
// ===========================================================================
struct JVal
{
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* get(const char* k) const
    {
        if (t != Obj) return nullptr;
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k) const
    {
        const JVal* v = get(k);
        if (!v) return std::string();
        if (v->t == Str) return v->s;
        if (v->t == Num) { char nb[32]; sprintf_s(nb, "%.0f", v->n); return nb; }
        return std::string();
    }
    UINT64 u64(const char* k) const { return _strtoui64(str(k).c_str(), nullptr, 10); }
};

struct JParser
{
    const char* p; const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    static void PutUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 63)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(unsigned& v)
    {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            char c = *p++; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& out)
    {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"')
        {
            if (*p == '\\')
            {
                if (++p >= e) return false;
                char c = *p++;
                switch (c)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u')
                    {
                        p += 2; unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    PutUtf8(out, cp);
                    break;
                }
                default: out += c;
                }
            }
            else out += *p++;
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool value(JVal& v, int depth = 0)
    {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        char c = *p;
        if (c == '{')
        {
            v.t = JVal::Obj; ++p; ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;)
            {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                ++p;
                v.o.emplace_back(std::move(k), JVal());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[')
        {
            v.t = JVal::Arr; ++p; ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;)
            {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::Str; return string(v.s); }
        if (e - p >= 4 && !strncmp(p, "true", 4))  { v.t = JVal::Bool; v.b = true;  p += 4; return true; }
        if (e - p >= 5 && !strncmp(p, "false", 5)) { v.t = JVal::Bool; v.b = false; p += 5; return true; }
        if (e - p >= 4 && !strncmp(p, "null", 4))  { v.t = JVal::Null; p += 4; return true; }
        char* end = nullptr;
        std::string num;
        while (p < e && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
            num += *p++;
        if (num.empty()) return false;
        v.t = JVal::Num; v.n = strtod(num.c_str(), &end);
        return true;
    }
};

bool JsonParse(const std::string& s, JVal& v)
{
    JParser jp{ s.data(), s.data() + s.size() };
    return jp.value(v);
}

// ===========================================================================
// Bencode (for .torrent files)
// ===========================================================================
struct BVal
{
    enum T { Int, Str, List, Dict } t = Int;
    long long i = 0;
    std::string s;
    std::vector<BVal> l;
    std::vector<std::pair<std::string, BVal>> d;
    size_t start = 0, end = 0;               // byte span in the source
    const BVal* get(const char* k) const
    {
        if (t != Dict) return nullptr;
        for (auto& kv : d) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

bool BDecode(const std::string& src, size_t& pos, BVal& v, int depth = 0)
{
    if (depth > 64 || pos >= src.size()) return false;
    v.start = pos;
    char c = src[pos];
    if (c == 'i')
    {
        size_t e = src.find('e', pos);
        if (e == std::string::npos) return false;
        v.t = BVal::Int;
        v.i = _strtoi64(src.substr(pos + 1, e - pos - 1).c_str(), nullptr, 10);
        pos = e + 1;
    }
    else if (c >= '0' && c <= '9')
    {
        size_t colon = src.find(':', pos);
        if (colon == std::string::npos || colon - pos > 12) return false;
        unsigned long long len = _strtoui64(src.substr(pos, colon - pos).c_str(), nullptr, 10);
        if (len > src.size() - colon - 1) return false;
        v.t = BVal::Str;
        v.s = src.substr(colon + 1, (size_t)len);
        pos = colon + 1 + (size_t)len;
    }
    else if (c == 'l')
    {
        v.t = BVal::List; ++pos;
        while (pos < src.size() && src[pos] != 'e')
        {
            v.l.emplace_back();
            if (!BDecode(src, pos, v.l.back(), depth + 1)) return false;
        }
        if (pos >= src.size()) return false;
        ++pos;
    }
    else if (c == 'd')
    {
        v.t = BVal::Dict; ++pos;
        while (pos < src.size() && src[pos] != 'e')
        {
            BVal k;
            if (!BDecode(src, pos, k, depth + 1) || k.t != BVal::Str) return false;
            v.d.emplace_back(k.s, BVal());
            if (!BDecode(src, pos, v.d.back().second, depth + 1)) return false;
        }
        if (pos >= src.size()) return false;
        ++pos;
    }
    else return false;
    v.end = pos;
    return true;
}

std::wstring BStrW(const BVal* plain, const BVal* utf8)
{
    if (utf8 && utf8->t == BVal::Str) return U2W(utf8->s);
    if (plain && plain->t == BVal::Str) return U2W(plain->s);
    return std::wstring();
}

// ===========================================================================
// aria2 process + JSON-RPC
// ===========================================================================
const char* kTrackers =
    "udp://tracker.opentrackr.org:1337/announce,"
    "udp://open.stealth.si:80/announce,"
    "udp://tracker.torrent.eu.org:451/announce,"
    "udp://exodus.desync.com:6969/announce,"
    "udp://open.demonii.com:1337/announce,"
    "udp://tracker.openbittorrent.com:6969/announce,"
    "https://tracker.opentrackr.org:443/announce";

// %LOCALAPPDATA%\Devnetra\NDM\run with a protected DACL: current user +
// SYSTEM, full control, nothing inherited. "" on failure.
std::wstring PrivateRunDir()
{
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)) || !buf[0])
        return std::wstring();
    std::wstring dir = std::wstring(buf) + L"\\Devnetra";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\NDM";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\run";

    // SDDL with the current user's SID
    std::wstring sddl;
    {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return std::wstring();
        std::vector<BYTE> tu(256);
        DWORD got = 0;
        bool ok = GetTokenInformation(tok, TokenUser, tu.data(), (DWORD)tu.size(), &got) != FALSE;
        if (!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            tu.resize(got);
            ok = GetTokenInformation(tok, TokenUser, tu.data(), (DWORD)tu.size(), &got) != FALSE;
        }
        CloseHandle(tok);
        if (!ok) return std::wstring();
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid, &sid)) return std::wstring();
        sddl = std::wstring(L"O:") + sid + L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)";
        LocalFree(sid);
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return std::wstring();
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    bool ok = CreateDirectoryW(dir.c_str(), &sa) != FALSE;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD a = GetFileAttributesW(dir.c_str());
        ok = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
        if (ok) {
            // re-apply the protected DACL (an older / foreign folder)
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
                 SetNamedSecurityInfoW(&dir[0], SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
        }
    }
    LocalFree(sd);
    return ok ? dir : std::wstring();
}

// Writes "rpc-secret=<secret>" to a new random file in the private run dir.
std::wstring WriteSecretConf(const std::string& secret)
{
    std::wstring dir = PrivateRunDir();
    if (dir.empty()) return std::wstring();
    // stale files of a crashed session
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\aria2-*.conf").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    std::wstring path = dir + L"\\aria2-" + U2W(RandomHex(8)) + L".conf";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);   // inherits the DACL
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::string body = "rpc-secret=" + secret + "\n";
    DWORD wr = 0;
    BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr) && wr == body.size();
    SecureZeroMemory(&body[0], body.size());
    CloseHandle(h);
    if (!ok) { DeleteFileW(path.c_str()); return std::wstring(); }
    return path;
}

struct RpcResult
{
    bool        ok = false;      // a JSON-RPC "result" was received
    bool        transport = false;   // HTTP / process failure (not an aria2 error)
    JVal        result;
    int         code = 0;        // aria2 error code
    std::string message;
};

class Aria2
{
public:
    static Aria2& I() { static Aria2 a; return a; }

    bool Ensure(std::wstring& err)
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (m_exiting) { err = L"NDM is closing."; return false; }
        if (m_proc && WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT && m_port) return true;
        StopLocked();
        std::wstring exe = Aria2Path();
        if (exe.empty()) { err = L"aria2c.exe is missing. Reinstall NDM to restore the torrent engine."; return false; }
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (StartLocked(exe, err)) return true;
            StopLocked();
        }
        if (err.empty()) err = L"The torrent engine (aria2c) failed to start.";
        return false;
    }

    // params = JSON text of the parameters after the token ("" for none)
    RpcResult Call(const char* method, const std::string& params, DWORD timeoutMs = 8000)
    {
        RpcResult r;
        std::shared_ptr<Conn> conn;        // keeps the WinHTTP handles open for this call
        std::string secret;
        INTERNET_PORT port = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(m_mx);
            conn = m_conn; secret = m_secret; port = m_port;
        }
        if (!conn || !conn->conn || !port) { r.transport = true; r.message = "engine not running"; return r; }
        HINTERNET hConn = conn->conn;

        std::string body = "{\"jsonrpc\":\"2.0\",\"id\":\"ndm\",\"method\":\"" + std::string(method) +
                           "\",\"params\":[" + JStr("token:" + secret) +
                           (params.empty() ? "" : "," + params) + "]}";
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", L"/jsonrpc", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hReq) { r.transport = true; return r; }
        WinHttpSetTimeouts(hReq, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
        const wchar_t* hdr = L"Content-Type: application/json\r\n";
        BOOL ok = WinHttpSendRequest(hReq, hdr, (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(),
                                     (DWORD)body.size(), 0) &&
                  WinHttpReceiveResponse(hReq, nullptr);
        std::string resp;
        if (ok)
        {
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hReq, &avail) || !avail) break;
                size_t old = resp.size();
                resp.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(hReq, &resp[old], avail, &got)) { ok = FALSE; break; }
                resp.resize(old + got);
                if (resp.size() > 64u * 1024 * 1024) { ok = FALSE; break; }
            }
        }
        WinHttpCloseHandle(hReq);
        JVal j;
        if (!ok || resp.empty() || !JsonParse(resp, j)) { r.transport = true; r.message = "no response"; return r; }
        if (const JVal* e = j.get("error"))
        {
            const JVal* c = e->get("code");
            r.code = c && c->t == JVal::Num ? (int)c->n : -1;
            r.message = e->str("message");
            return r;
        }
        if (const JVal* res = j.get("result")) { r.result = *res; r.ok = true; }
        return r;
    }

    bool Alive()
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        return m_proc && WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT;
    }

    void ApplySpeedLimit(DWORD bps)
    {
        if (bps == m_appliedLimit) return;
        RpcResult r = Call("aria2.changeGlobalOption",
                           "{\"max-overall-download-limit\":" + JStr(std::to_string(bps)) + "}");
        if (r.ok) m_appliedLimit = bps;
    }

    // gids created for metadata-only fetches (never adopted by a download)
    void MarkMeta(const std::string& gid, bool on)
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        if (on) m_meta.insert(gid); else m_meta.erase(gid);
    }
    bool IsMeta(const std::string& gid)
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        return m_meta.count(gid) != 0;
    }

    bool Exiting()
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        return m_exiting;
    }

    // Final stop at NDM exit: saves the session, shuts aria2 down gracefully
    // (it flushes its control files) and refuses any restart afterwards.
    void Stop()
    {
        std::lock_guard<std::recursive_mutex> lk(m_mx);
        m_exiting = true;
        if (m_proc && m_port)
        {
            Call("aria2.saveSession", "", 2000);
            Call("aria2.forceShutdown", "", 2000);
            WaitForSingleObject(m_proc, 3000);
        }
        StopLocked();
    }

    const std::wstring& SessionDir() { return m_appDir; }

private:
    Aria2() = default;

    // WinHTTP session + connection, shared by every in-flight Call(): a
    // concurrent Ensure()/StopLocked() only drops its reference, the handles
    // close when the last call using them returns.
    struct Conn
    {
        HINTERNET sess = nullptr, conn = nullptr;
        ~Conn()
        {
            if (conn) WinHttpCloseHandle(conn);
            if (sess) WinHttpCloseHandle(sess);
        }
    };

    static INTERNET_PORT FreePort()
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 0;
        INTERNET_PORT port = 0;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET)
        {
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            int len = sizeof(a);
            if (bind(s, (sockaddr*)&a, sizeof(a)) == 0 && getsockname(s, (sockaddr*)&a, &len) == 0)
                port = ntohs(a.sin_port);
            closesocket(s);
        }
        WSACleanup();
        return port;
    }

    bool StartLocked(const std::wstring& exe, std::wstring& err)
    {
        m_appDir = AppDataDir();
        if (m_appDir.empty()) { err = L"Cannot locate the application data folder."; return false; }
        std::wstring session = m_appDir + L"\\aria2.session";
        if (!FileExists(session))
        {
            HANDLE h = CreateFileW(session.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        m_port = FreePort();
        if (!m_port) { err = L"No free local port for the torrent engine."; return false; }
        m_secret = RandomHex(16);
        // The secret goes into a private conf file, never onto the command line.
        m_confFile = WriteSecretConf(m_secret);
        if (m_confFile.empty()) { err = L"Cannot prepare the torrent engine (private run folder)."; return false; }

        std::wstring cmd = L"\"" + exe + L"\"";
        auto arg = [&](const std::wstring& a) { cmd += L" " + a; };
        auto qarg = [&](const wchar_t* k, const std::wstring& v) { cmd += L" "; cmd += k; cmd += L"=\"" + v + L"\""; };
        qarg(L"--conf-path", m_confFile);           // only rpc-secret=...; replaces any user aria2.conf
        arg(L"--enable-rpc=true");
        arg(L"--rpc-listen-all=false");
        arg(L"--rpc-listen-port=" + std::to_wstring(m_port));
        arg(L"--rpc-max-request-size=32M");
        arg(L"--rpc-save-upload-metadata=false");   // .torrent bytes live in our cache
        arg(L"--stop-with-process=" + std::to_wstring(GetCurrentProcessId()));
        qarg(L"--input-file", session);
        qarg(L"--save-session", session);
        arg(L"--save-session-interval=30");
        qarg(L"--dht-file-path", m_appDir + L"\\aria2-dht.dat");
        qarg(L"--dht-file-path6", m_appDir + L"\\aria2-dht6.dat");
        arg(L"--quiet=true");
        arg(L"--console-log-level=error");
        arg(L"--max-concurrent-downloads=32");      // NDM's queue decides concurrency
        arg(L"--continue=true");
        arg(L"--file-allocation=falloc");
        arg(L"--enable-dht=true");
        arg(L"--enable-dht6=true");
        arg(L"--enable-peer-exchange=true");
        arg(L"--bt-enable-lpd=true");
        arg(L"--bt-max-peers=128");
        arg(L"--bt-request-peer-speed-limit=10M");
        arg(L"--max-overall-upload-limit=64K");
        arg(L"--seed-time=0");
        arg(L"--bt-save-metadata=false");
        arg(L"--bt-load-saved-metadata=true");
        arg(L"--bt-remove-unselected-file=false");
        arg(L"--bt-tracker-connect-timeout=10");
        arg(L"--bt-tracker-timeout=20");
        arg(L"--follow-torrent=mem");
        arg(L"--auto-save-interval=30");
        arg(L"--disk-cache=32M");
        arg(L"--summary-interval=0");
        arg(L"--bt-tracker=" + U2W(kTrackers));
        arg(L"--user-agent=NDM/" + U2W(NDM_VERSION_STR));
        arg(L"--peer-agent=NDM/" + U2W(NDM_VERSION_STR));

        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::wstring dir = DirOf(exe);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

        // Put the child into the job atomically at creation (Windows 10+), so
        // NDM exiting at any moment can never leave an orphaned aria2c behind.
        std::vector<char> attrBuf;
        bool inJob = false;
        if (m_job)
        {
            SIZE_T sz = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
            attrBuf.resize(sz);
            auto* al = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
            if (sz && InitializeProcThreadAttributeList(al, 1, 0, &sz))
            {
                if (UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &m_job, sizeof(HANDLE),
                                              nullptr, nullptr))
                {
                    si.lpAttributeList = al;
                    inJob = true;
                }
                else DeleteProcThreadAttributeList(al);
            }
        }
        BOOL created = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                      flags | (inJob ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                      dir.c_str(), (STARTUPINFOW*)&si, &pi);
        if (!created && inJob)          // older Windows: no job-list attribute
        {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            si.lpAttributeList = nullptr;
            inJob = false;
            created = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                     dir.c_str(), (STARTUPINFOW*)&si, &pi);
        }
        else if (inJob) DeleteProcThreadAttributeList(si.lpAttributeList);
        if (!created)
        {
            err = L"Cannot start aria2c.exe: " + NdmHttpEngine::ErrorText(GetLastError());
            return false;
        }
        if (m_job && !inJob && !AssignProcessToJobObject(m_job, pi.hProcess))
        {
            TerminateProcess(pi.hProcess, 1);     // never run it untracked
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            err = L"Cannot start aria2c.exe (job object).";
            return false;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        m_proc = pi.hProcess;

        {
            auto c = std::make_shared<Conn>();
            c->sess = WinHttpOpen(L"NDM-torrent", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
            if (c->sess) c->conn = WinHttpConnect(c->sess, L"127.0.0.1", m_port, 0);
            if (!c->conn) { err = L"Cannot open the local RPC connection."; return false; }
            m_conn = c;
        }

        // wait for the RPC endpoint (up to ~6 s)
        for (int i = 0; i < 60; ++i)
        {
            if (WaitForSingleObject(m_proc, 100) != WAIT_TIMEOUT) break;   // exited (port taken?)
            RpcResult r = Call("aria2.getVersion", "", 1000);
            if (r.ok)
            {
                DeleteConf();                 // aria2 has read it: the secret leaves the disk
                // restored session entries must not run untracked: pause them
                // until NdmTorrentRun adopts them.
                Call("aria2.forcePauseAll", "");
                m_appliedLimit = 0;
                return true;
            }
        }
        err = L"The torrent engine did not respond.";
        return false;
    }

    void DeleteConf()
    {
        if (m_confFile.empty()) return;
        DeleteFileW(m_confFile.c_str());
        m_confFile.clear();
    }

    void StopLocked()
    {
        m_conn.reset();                       // in-flight calls keep their own reference
        DeleteConf();
        if (m_proc)
        {
            if (WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT) TerminateProcess(m_proc, 1);
            CloseHandle(m_proc); m_proc = nullptr;
        }
        if (m_job) { CloseHandle(m_job); m_job = nullptr; }   // kill-on-close
        m_port = 0;
    }

    std::recursive_mutex m_mx;
    HANDLE        m_proc = nullptr, m_job = nullptr;
    std::shared_ptr<Conn> m_conn;
    INTERNET_PORT m_port = 0;
    std::string   m_secret;
    std::wstring  m_confFile;
    bool          m_exiting = false;
    std::wstring  m_appDir;
    DWORD         m_appliedLimit = 0;
    std::set<std::string> m_meta;
};

// ---------------------------------------------------------------------------
// RPC helpers
// ---------------------------------------------------------------------------
const char* kStatusKeys =
    "[\"gid\",\"status\",\"totalLength\",\"completedLength\",\"downloadSpeed\",\"connections\","
    "\"numSeeders\",\"errorCode\",\"errorMessage\",\"followedBy\",\"infoHash\",\"dir\",\"bittorrent\"]";

std::string FirstFollowed(const JVal& st)
{
    const JVal* f = st.get("followedBy");
    if (f && f->t == JVal::Arr && !f->a.empty() && f->a[0].t == JVal::Str) return f->a[0].s;
    return std::string();
}

std::wstring TorrentName(const JVal& st)
{
    const JVal* bt = st.get("bittorrent");
    const JVal* info = bt ? bt->get("info") : nullptr;
    return info ? U2W(info->str("name")) : std::wstring();
}

std::wstring ErrorFor(int code, const std::string& msg)
{
    switch (code)
    {
    case 2:  return L"Timed out while connecting to peers or trackers.";
    case 3:  return L"The torrent could not be found.";
    case 6:  return L"Network problem: no connection to peers or trackers.";
    case 9:  return L"Not enough disk space.";
    case 12: return L"This torrent is already being downloaded.";
    case 13: return L"A file with the same name already exists.";
    case 15: case 16: return L"Cannot create or open the file. Check the save folder.";
    case 17: return L"Disk I/O error while writing the file.";
    case 18: return L"Cannot create the save folder.";
    case 19: return L"Name resolution failed (DNS).";
    case 26: case 27: return L"The torrent file or magnet link is invalid.";
    case 28: return L"aria2 rejected an option.";
    }
    std::wstring m = U2W(msg);
    return m.empty() ? L"Torrent download failed (aria2 error " + std::to_wstring(code) + L")."
                     : L"Torrent error: " + m;
}

bool StatusOf(const std::string& gid, JVal& st, RpcResult* raw = nullptr)
{
    RpcResult r = Aria2::I().Call("aria2.tellStatus", JStr(gid) + "," + kStatusKeys);
    if (raw) *raw = r;
    if (!r.ok) return false;
    st = r.result;
    return true;
}

void RemoveGid(const std::string& gid, bool forced = true)
{
    Aria2& a = Aria2::I();
    a.Call(forced ? "aria2.forceRemove" : "aria2.remove", JStr(gid), 4000);
    for (int i = 0; i < 30; ++i)          // wait until it left the active list
    {
        JVal st;
        if (!StatusOf(gid, st)) break;
        std::string s = st.str("status");
        if (s != "active" && s != "waiting" && s != "paused") break;
        Sleep(100);
    }
    a.Call("aria2.removeDownloadResult", JStr(gid), 3000);
}

// Mark-of-the-Web on every finished, selected file of a torrent (aria2's file
// list). Only paths inside the download folder are touched.
void MarkTorrentFiles(const std::string& gid, const std::wstring& dir,
                      const std::wstring& source, const std::wstring& referrer)
{
    RpcResult r = Aria2::I().Call("aria2.getFiles", JStr(gid), 8000);
    if (!r.ok || r.result.t != JVal::Arr) return;
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(NormDir(dir).c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (!n || n >= sizeof(full) / sizeof(full[0])) return;
    std::wstring root = full;
    if (root.empty()) return;
    if (root.back() != L'\\') root += L'\\';
    size_t marked = 0;
    for (const JVal& f : r.result.a)
    {
        if (marked >= 100000) break;
        if (f.str("selected") == "false") continue;
        std::wstring p = U2W(f.str("path"));
        if (p.empty() || p.rfind(L"[METADATA]", 0) == 0) continue;
        if (f.u64("length") != f.u64("completedLength")) continue;
        for (auto& c : p) if (c == L'/') c = L'\\';
        n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
        if (!n || n >= sizeof(full) / sizeof(full[0])) continue;
        std::wstring fp = full;
        if (fp.size() <= root.size() || _wcsnicmp(fp.c_str(), root.c_str(), root.size()) != 0) continue;
        DWORD attr = GetFileAttributesW(fp.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            continue;
        NdmMarkOfTheWeb(fp, source, referrer);
        ++marked;
    }
}
// Find a non-metadata download of hash in folder; returns gid + status.
std::string FindByHash(const std::string& hash, const std::wstring& dir, std::string& status)
{
    Aria2& a = Aria2::I();
    const char* keys = "[\"gid\",\"status\",\"infoHash\",\"dir\",\"followedBy\"]";
    std::wstring want = NormDir(dir);
    RpcResult lists[3] = {
        a.Call("aria2.tellActive", keys),
        a.Call("aria2.tellWaiting", std::string("0,1000,") + keys),
        a.Call("aria2.tellStopped", std::string("0,1000,") + keys),
    };
    for (auto& r : lists)
    {
        if (!r.ok || r.result.t != JVal::Arr) continue;
        for (auto& e : r.result.a)
        {
            if (_stricmp(e.str("infoHash").c_str(), hash.c_str()) != 0) continue;
            std::string gid = e.str("gid");
            if (a.IsMeta(gid)) continue;
            if (!FirstFollowed(e).empty()) continue;            // a finished metadata step
            if (NormDir(U2W(e.str("dir"))) != want) continue;
            status = e.str("status");
            return gid;
        }
    }
    return std::string();
}

std::wstring CacheDir()
{
    std::wstring d = AppDataDir();
    if (d.empty()) return d;
    d += L"\\torrents";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool IsHash(const std::string& h)
{
    if (h.size() != 40) return false;
    for (char c : h) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

std::wstring UrlDecode(const std::wstring& s)
{
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i)
    {
        wchar_t c = s[i];
        if (c == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2]))
        {
            bytes += (char)wcstol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        else if (c == L'+') bytes += ' ';
        else bytes += W2U(std::wstring(1, c));
    }
    return U2W(bytes);
}

std::wstring MagnetParam(const std::wstring& magnet, const wchar_t* key)
{
    size_t q = magnet.find(L'?');
    if (q == std::wstring::npos) return std::wstring();
    std::wstring k = std::wstring(key) + L"=";
    size_t p = q + 1;
    while (p < magnet.size())
    {
        size_t amp = magnet.find(L'&', p);
        std::wstring part = magnet.substr(p, amp == std::wstring::npos ? std::wstring::npos : amp - p);
        if (part.size() > k.size() && _wcsnicmp(part.c_str(), k.c_str(), k.size()) == 0)
            return UrlDecode(part.substr(k.size()));
        if (amp == std::wstring::npos) break;
        p = amp + 1;
    }
    return std::wstring();
}

bool IsHttpUrl(const std::wstring& s)
{
    return _wcsnicmp(s.c_str(), L"http://", 7) == 0 || _wcsnicmp(s.c_str(), L"https://", 8) == 0;
}

std::wstring LocalPathOf(const std::wstring& s)
{
    if (_wcsnicmp(s.c_str(), L"file:///", 8) == 0) return UrlDecode(s.substr(8));
    return s;
}

} // namespace

// ===========================================================================
// Internal API
// ===========================================================================
bool NdmTorrentParse(const std::string& bytes, NdmTorrentInfo& out)
{
    out = NdmTorrentInfo();
    BVal root;
    size_t pos = 0;
    if (!BDecode(bytes, pos, root) || root.t != BVal::Dict) return false;
    const BVal* info = root.get("info");
    if (!info || info->t != BVal::Dict) return false;
    out.infoHash = Sha1Hex(bytes.data() + info->start, info->end - info->start);
    out.name = BStrW(info->get("name"), info->get("name.utf-8"));
    const BVal* files = info->get("files");
    if (files && files->t == BVal::List)
    {
        out.multiFile = true;
        for (const BVal& f : files->l)
        {
            if (f.t != BVal::Dict) return false;
            NdmTorrentFile tf;
            const BVal* len = f.get("length");
            tf.size = len && len->t == BVal::Int && len->i > 0 ? (UINT64)len->i : 0;
            const BVal* path = f.get("path.utf-8");
            if (!path || path->t != BVal::List) path = f.get("path");
            if (path && path->t == BVal::List)
                for (const BVal& seg : path->l)
                    if (seg.t == BVal::Str)
                    {
                        if (!tf.path.empty()) tf.path += L"\\";
                        tf.path += U2W(seg.s);
                    }
            const BVal* attr = f.get("attr");
            tf.padding = (attr && attr->t == BVal::Str && attr->s.find('p') != std::string::npos) ||
                         tf.path.rfind(L".pad\\", 0) == 0 || tf.path.find(L"_____padding_file_") != std::wstring::npos;
            out.totalSize += tf.padding ? 0 : tf.size;
            out.files.push_back(tf);
        }
    }
    else
    {
        const BVal* len = info->get("length");
        NdmTorrentFile tf;
        tf.path = out.name;
        tf.size = len && len->t == BVal::Int && len->i > 0 ? (UINT64)len->i : 0;
        out.totalSize = tf.size;
        out.files.push_back(tf);
    }
    if (out.name.empty()) out.name = L"torrent-" + U2W(out.infoHash.substr(0, 8));
    return !out.infoHash.empty();
}

std::string NdmTorrentMagnetHash(const std::wstring& magnet)
{
    if (_wcsnicmp(magnet.c_str(), L"magnet:", 7) != 0) return std::string();
    // xt may appear several times (xt, xt.1, ...); take the first btih
    size_t p = 0;
    for (;;)
    {
        p = magnet.find(L"urn:btih:", p);
        if (p == std::wstring::npos) return std::string();
        p += 9;
        size_t e = magnet.find_first_of(L"&", p);
        std::string v = W2U(magnet.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p));
        if (v.size() == 40 && IsHash(v))
        {
            for (auto& c : v) c = (char)tolower((unsigned char)c);
            return v;
        }
        if (v.size() == 32)          // base32
        {
            std::string bits, out;
            unsigned buf = 0; int n = 0;
            bool ok = true;
            for (char c : v)
            {
                c = (char)toupper((unsigned char)c);
                int val = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= '2' && c <= '7') ? c - '2' + 26 : -1;
                if (val < 0) { ok = false; break; }
                buf = (buf << 5) | (unsigned)val; n += 5;
                if (n >= 8) { n -= 8; out += (char)((buf >> n) & 0xFF); }
            }
            if (ok && out.size() == 20)
            {
                static const char* hx = "0123456789abcdef";
                std::string h;
                for (unsigned char x : out) { h += hx[x >> 4]; h += hx[x & 15]; }
                return h;
            }
        }
    }
}

std::wstring NdmTorrentMagnetName(const std::wstring& magnet)
{
    return MagnetParam(magnet, L"dn");
}

bool NdmTorrentLoadSource(const std::wstring& src, std::string& bytes, std::wstring& err, HANDLE hCancel)
{
    bytes.clear();
    if (!IsHttpUrl(src))
    {
        if (!ReadWholeFile(LocalPathOf(src), bytes, 32u * 1024 * 1024))
        {
            err = L"Cannot read the .torrent file.";
            return false;
        }
        return true;
    }
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4095;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(src.c_str(), 0, 0, &uc)) { err = L"Invalid .torrent address."; return false; }
    std::wstring obj = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) NDM/1.0",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"NDM/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r)
    {
        WinHttpSetTimeouts(r, 10000, 10000, 15000, 15000);
        DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;   // never downgrade https
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr))
        {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
            if (code == 200)
            {
                ok = true;
                for (;;)
                {
                    if (hCancel && WaitForSingleObject(hCancel, 0) == WAIT_OBJECT_0) { ok = false; err = L"Cancelled."; break; }
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                    if (!avail) break;
                    size_t old = bytes.size();
                    bytes.resize(old + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(r, &bytes[old], avail, &got)) { ok = false; break; }
                    bytes.resize(old + got);
                    if (bytes.size() > 32u * 1024 * 1024) { ok = false; break; }
                }
            }
            else err = L"The server answered HTTP " + std::to_wstring(code) + L" for the .torrent file.";
        }
        else err = L"Cannot download the .torrent file: " + NdmHttpEngine::ErrorText(GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok && err.empty()) err = L"Cannot download the .torrent file.";
    return ok && !bytes.empty();
}

bool NdmTorrentCacheGet(const std::string& hash, std::string& bytes)
{
    if (!IsHash(hash)) return false;
    std::wstring d = CacheDir();
    if (d.empty() || !ReadWholeFile(d + L"\\" + U2W(hash) + L".torrent", bytes, 32u * 1024 * 1024)) return false;
    NdmTorrentInfo ti;
    return NdmTorrentParse(bytes, ti) && ti.infoHash == hash;
}

void NdmTorrentCachePut(const std::string& hash, const std::string& bytes)
{
    if (!IsHash(hash) || bytes.empty()) return;
    std::wstring d = CacheDir();
    if (!d.empty()) WriteWholeFile(d + L"\\" + U2W(hash) + L".torrent", bytes);
}

bool NdmTorrentFetchMetadata(const std::wstring& magnet, HANDLE hCancel, std::string& bytes, std::wstring& err)
{
    std::string hash = NdmTorrentMagnetHash(magnet);
    if (hash.empty()) { err = L"This magnet link has no BitTorrent info hash."; return false; }
    if (NdmTorrentCacheGet(hash, bytes)) return true;

    Aria2& a = Aria2::I();
    if (!a.Ensure(err)) return false;

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"NDM_meta_" + U2W(hash);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring saved = dir + L"\\" + U2W(hash) + L".torrent";
    DeleteFileW(saved.c_str());

    std::string opts = "{\"dir\":" + JStr(dir) +
                       ",\"bt-metadata-only\":\"true\",\"bt-save-metadata\":\"true\",\"follow-torrent\":\"false\"}";
    RpcResult r;
    for (int i = 0; i < 10; ++i)         // a just-removed duplicate may linger briefly
    {
        r = a.Call("aria2.addUri", "[" + JStr(magnet) + "]," + opts);
        if (r.ok || r.transport) break;
        if (hCancel && WaitForSingleObject(hCancel, 300) == WAIT_OBJECT_0) break;
        if (!hCancel) Sleep(300);
    }
    if (!r.ok || r.result.t != JVal::Str)
    {
        err = r.transport ? L"The torrent engine is not responding." : ErrorFor(r.code, r.message);
        RemoveDirectoryW(dir.c_str());
        return false;
    }
    std::string gid = r.result.s;
    a.MarkMeta(gid, true);

    bool ok = false;
    for (;;)
    {
        if (hCancel && WaitForSingleObject(hCancel, 400) == WAIT_OBJECT_0) { err = L"Cancelled."; break; }
        if (!hCancel) Sleep(400);
        JVal st;
        RpcResult raw;
        if (!StatusOf(gid, st, &raw))
        {
            if (!a.Alive()) { err = L"The torrent engine stopped."; break; }
            continue;
        }
        std::string s = st.str("status");
        if (s == "complete")
        {
            for (int i = 0; i < 20 && !FileExists(saved); ++i) Sleep(100);
            ok = ReadWholeFile(saved, bytes, 32u * 1024 * 1024);
            NdmTorrentInfo ti;
            ok = ok && NdmTorrentParse(bytes, ti);
            if (ok) NdmTorrentCachePut(hash, bytes);
            else err = L"The received metadata is invalid.";
            break;
        }
        if (s == "error" || s == "removed")
        {
            err = ErrorFor(atoi(st.str("errorCode").c_str()), st.str("errorMessage"));
            break;
        }
    }
    RemoveGid(gid);
    a.MarkMeta(gid, false);
    DeleteFileW(saved.c_str());
    // aria2 may leave a control file for the metadata pseudo-download
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    RemoveDirectoryW(dir.c_str());
    return ok;
}

void NdmTorrentShutdown()
{
    Aria2::I().Stop();
}

// ===========================================================================
// Public API
// ===========================================================================
bool NdmTorrentIsTorrentLink(const std::wstring& s)
{
    if (s.size() >= 7 && _wcsnicmp(s.c_str(), L"magnet:", 7) == 0) return true;
    return s.size() >= 8 && _wcsicmp(s.c_str() + s.size() - 8, L".torrent") == 0;
}

NdmRunResult NdmTorrentRun(NdmDownload* dl, const NdmRunOptions& opt)
{
    if (!dl->hStopEvent) dl->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE hStop = dl->hStopEvent;
    auto stopped = [&]() { return WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0; };
    auto fail = [&](const std::wstring& msg) {
        NdmHttpEngine::SetSharedString(dl->errorMsg, msg);
        if (opt.setStatus) dl->status = NDM_STATUS_ERROR;
        dl->speed = 0;
        return NDM_RUN_ERROR;
    };
    if (opt.setStatus) dl->status = NDM_STATUS_DOWNLOADING;
    NdmHttpEngine::SetSharedString(dl->errorMsg, L"");
    dl->speed = 0;

    const std::wstring url = dl->url;
    const bool magnet = _wcsnicmp(url.c_str(), L"magnet:", 7) == 0;
    std::wstring dir = dl->savePath;
    if (dir.empty()) return fail(L"No save folder.");
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    auto setName = [&](const std::wstring& n) {
        if (n.empty()) return;
        std::wstring clean = NdmHttpEngine::SanitizeFilename(n);
        if (clean.empty() || clean == NdmHttpEngine::GetSharedString(dl->filename)) return;
        NdmHttpEngine::SetSharedString(dl->filename, clean);
        if (opt.onChange) opt.onChange(dl, opt.onChangeCtx);
    };

    // ---- source: info hash + (optional) .torrent bytes ---------------------
    std::string hash, bytes;
    NdmTorrentInfo info;
    bool haveInfo = false;
    if (magnet)
    {
        hash = NdmTorrentMagnetHash(url);
        if (hash.empty()) return fail(L"This magnet link has no BitTorrent info hash.");
        if (NdmTorrentCacheGet(hash, bytes)) haveInfo = NdmTorrentParse(bytes, info);
        if (!haveInfo) { bytes.clear(); setName(NdmTorrentMagnetName(url)); }
    }
    else
    {
        // The .torrent is cached by its info hash after the first fetch and
        // the hash is stored on the item: a resume uses the cached bytes, so
        // it works even when the source link has expired meanwhile.
        std::string known = W2U(NdmHttpEngine::GetSharedString(dl->torrentHash));
        if (IsHash(known) && NdmTorrentCacheGet(known, bytes) && NdmTorrentParse(bytes, info))
        {
            haveInfo = true;
            hash = info.infoHash;
        }
        else
        {
            std::wstring err;
            bytes.clear();
            if (!NdmTorrentLoadSource(url, bytes, err, hStop))
            {
                if (stopped()) return NDM_RUN_STOPPED;
                return fail(err);
            }
            if (!NdmTorrentParse(bytes, info)) return fail(L"The .torrent file is invalid.");
            haveInfo = true;
            hash = info.infoHash;
            NdmTorrentCachePut(hash, bytes);
        }
    }
    if (U2W(hash) != NdmHttpEngine::GetSharedString(dl->torrentHash))
    {
        NdmHttpEngine::LockFields(true);
        dl->torrentHash = U2W(hash);
        NdmHttpEngine::UnlockFields(true);
        if (opt.onChange) opt.onChange(dl, opt.onChangeCtx);       // persist it
    }
    if (haveInfo)
    {
        setName(info.name);
        if (!dl->totalSize) Set64(&dl->totalSize, info.totalSize);
    }

    Aria2& a = Aria2::I();
    std::wstring err;
    // NDM is exiting (engine already shut down): stop, do not fail. The queue
    // signals hStop right after the engine shutdown.
    auto exitStop = [&]() {
        WaitForSingleObject(hStop, 20000);
        dl->speed = 0;
        return NDM_RUN_STOPPED;
    };
    if (!a.Ensure(err)) return a.Exiting() ? exitStop() : fail(err);
    if (stopped()) return NDM_RUN_STOPPED;

    // ---- adopt a download restored from the session, or add a new one ------
    std::string gid, status;
    gid = FindByHash(hash, dir, status);
    if (!gid.empty())
    {
        if (status == "paused") a.Call("aria2.unpause", JStr(gid));
        else if (status != "active" && status != "waiting") { RemoveGid(gid); gid.clear(); }
    }
    int engineRestarts = 0;
    auto addNew = [&]() -> bool {
        std::string o = "{\"dir\":" + JStr(dir) + ",\"pause\":\"false\"";
        if (!dl->torrentFiles.empty()) o += ",\"select-file\":" + JStr(dl->torrentFiles);
        if (haveInfo)
        {
            // data on disk without a control file -> verify pieces instead of re-downloading
            std::wstring target = dir + L"\\" + info.name;
            bool data = FileExists(target), ctl = FileExists(target + L".aria2");
            o += std::string(",\"check-integrity\":\"") + (data && !ctl ? "true" : "false") + "\"";
        }
        else if (magnet)
            o += ",\"bt-save-metadata\":\"true\"";         // copied into the cache below
        o += "}";
        RpcResult r;
        for (int i = 0; i < 12; ++i)
        {
            r = bytes.empty() ? a.Call("aria2.addUri", "[" + JStr(url) + "]," + o)
                              : a.Call("aria2.addTorrent", JStr(Base64(bytes)) + ",[]," + o, 20000);
            if (r.ok || r.transport) break;
            // duplicate (the add dialog's metadata fetch is still winding down)
            if (WaitForSingleObject(hStop, 300) == WAIT_OBJECT_0) return false;
        }
        if (!r.ok || r.result.t != JVal::Str)
        {
            err = r.transport ? L"The torrent engine is not responding." : ErrorFor(r.code, r.message);
            return false;
        }
        gid = r.result.s;
        a.Call("aria2.saveSession", "");
        return true;
    };
    if (gid.empty() && !addNew())
    {
        if (stopped()) return NDM_RUN_STOPPED;
        return fail(err);
    }

    // ---- poll -------------------------------------------------------------
    bool metaStage = magnet && !haveInfo;
    int  missing = 0, metaDoneWaits = 0;
    // This download's share of the bandwidth, applied live per gid
    // (aria2.changeOption max-download-limit; 0 = unlimited).
    NdmFlowScope flow(dl, NDM_KIND_TORRENT);
    std::string limitGid;
    DWORD limitApplied = 0;
    struct PeersReset { NdmDownload* d; ~PeersReset() { InterlockedExchange(&d->activeConns, 0); } } peersReset{ dl };
    for (;;)
    {
        if (WaitForSingleObject(hStop, 500) == WAIT_OBJECT_0)
        {
            RemoveGid(gid);
            a.Call("aria2.saveSession", "");
            dl->speed = 0;
            return NDM_RUN_STOPPED;
        }
        a.ApplySpeedLimit(NdmHttpEngine::GetSpeedLimit());
        {
            DWORD want = flow.Rate();
            bool changed = want != limitApplied &&
                           (want == 0 || limitApplied == 0 ||
                            (want > limitApplied ? want - limitApplied : limitApplied - want) > limitApplied / 20);
            if (gid != limitGid || changed)
            {
                RpcResult lr = a.Call("aria2.changeOption",
                                      JStr(gid) + ",{\"max-download-limit\":" + JStr(std::to_string(want)) + "}");
                if (lr.ok) { limitGid = gid; limitApplied = want; }
            }
        }

        JVal st;
        RpcResult raw;
        if (!StatusOf(gid, st, &raw))
        {
            if (raw.transport && !a.Alive())
            {
                // aria2 died: restart it and re-add (control file resumes)
                if (a.Exiting()) return exitStop();
                if (++engineRestarts > 3 || !a.Ensure(err)) return fail(err.empty() ? L"The torrent engine stopped." : err);
                if (NdmTorrentCacheGet(hash, bytes)) haveInfo = NdmTorrentParse(bytes, info);
                gid = FindByHash(hash, dir, status);
                if (!gid.empty() && status == "paused") a.Call("aria2.unpause", JStr(gid));
                if (gid.empty() && !addNew()) return fail(err);
                metaStage = magnet && !haveInfo;
                continue;
            }
            if (++missing > 20) return fail(L"The torrent engine lost this download.");
            continue;
        }
        missing = 0;

        std::string s = st.str("status");
        std::string next = FirstFollowed(st);
        if (!next.empty())
        {
            // magnet metadata done -> the real download
            if (metaStage)
            {
                std::wstring saved = dir + L"\\" + U2W(hash) + L".torrent";
                std::string tb;
                if (ReadWholeFile(saved, tb, 32u * 1024 * 1024) && NdmTorrentParse(tb, info) && info.infoHash == hash)
                {
                    NdmTorrentCachePut(hash, tb);
                    haveInfo = true;
                    DeleteFileW(saved.c_str());
                }
                metaStage = false;
            }
            a.Call("aria2.removeDownloadResult", JStr(gid));
            gid = next;
            continue;
        }

        if (!metaStage)
        {
            std::wstring n = TorrentName(st);
            if (!n.empty()) setName(n);
            UINT64 total = st.u64("totalLength");
            if (total) Set64(&dl->totalSize, total);
            Set64(&dl->downloadedSize, st.u64("completedLength"));
        }
        dl->speed = (DWORD)st.u64("downloadSpeed");
        // Peers connected right now: a live fact, never written into the
        // persisted connection setting (numConnections).
        int peers = atoi(st.str("connections").c_str());
        InterlockedExchange(&dl->activeConns, peers > 0 ? peers : 0);

        if (s == "complete")
        {
            if (metaStage && ++metaDoneWaits < 10) continue;   // followedBy shows up on the next poll
            if (dl->totalSize) Set64(&dl->downloadedSize, NdmHttpEngine::AtomicRead64(&dl->totalSize));
            dl->speed = 0;
            MarkTorrentFiles(gid, dir, url, dl->referrer);
            a.Call("aria2.removeDownloadResult", JStr(gid));
            a.Call("aria2.saveSession", "");
            if (opt.setStatus) dl->status = NDM_STATUS_COMPLETED;
            return NDM_RUN_COMPLETED;
        }
        if (s == "error")
        {
            std::wstring msg = ErrorFor(atoi(st.str("errorCode").c_str()), st.str("errorMessage"));
            a.Call("aria2.removeDownloadResult", JStr(gid));
            a.Call("aria2.saveSession", "");
            return fail(msg);
        }
        if (s == "removed")
        {
            a.Call("aria2.removeDownloadResult", JStr(gid));
            if (stopped()) return NDM_RUN_STOPPED;
            return fail(L"The download was removed from the torrent engine.");
        }
        if (s == "paused") a.Call("aria2.unpause", JStr(gid));
    }
}
