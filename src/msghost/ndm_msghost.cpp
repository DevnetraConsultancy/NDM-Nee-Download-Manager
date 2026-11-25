// ---------------------------------------------------------------------------
// ndm_msghost.cpp — browser extension <-> NDMan.exe bridge
//
// See ndm_msghost.h for the overall picture. Message protocol (UTF-8 JSON,
// 4-byte little-endian length prefix on both the stdio and the pipe leg):
//
//   -> {"id":7,"action":"ping"}
//   <- {"id":7,"status":"ok","app":"NDM","version":"1.0.5","running":true,"capture":true}
//   "capture": automatic browser capture is on in NDM Options (registry
//   mirror BrowserMonitor; answered by the relay too when NDMan is down).
//
//   -> {"id":8,"action":"addDownload","url":"https://..","filename":"a.zip",
//       "referrer":"..","cookies":"a=b; c=d","userAgent":"..","mime":"..",
//       "fileSize":12345,"silent":false}
//   <- {"id":8,"status":"accepted"}            (or "status":"error","error":..)
//
//   -> {"id":9,"action":"addDownloads","referrer":"..","userAgent":"..",
//       "cookies":"..","silent":true,
//       "links":[{"url":"..","filename":"..","cookies":"..","mime":"..",
//                 "fileSize":0,"referrer":".."}, "https://plain-string", ...]}
//   <- {"id":9,"status":"accepted","count":N,"rejected":M}
//
//   -> {"id":10,"action":"show"}   (bring NDM main window to front)
//   <- {"id":10,"status":"ok"}
//
//   -> {"id":11,"action":"getMedia","pageUrl":"https://www.youtube.com/watch?v=..",
//       "cookies":"..","userAgent":"..","referrer":".."}
//   <- {"id":11,"status":"ok","title":"..","thumbnail":"..","duration":635,
//       "formats":[{"label":"Best (1080p MP4)","kind":"video","format":"bv*+ba/b",
//                   "ext":"mp4","height":1080,"filesize":142000000,"approx":true},
//                  {"label":"720p MP4","kind":"video","format":"bv*[height<=720]+ba/b[height<=720]/b",...},
//                  {"label":"Best Audio (Opus 160 kbps)","kind":"audio","format":"251","ext":"webm",...},
//                  {"label":"Audio M4A 130 kbps","kind":"audio","format":"140","ext":"m4a",...},
//                  {"label":"MP3 160 kbps (converted)","kind":"audio","format":"x:mp3:160|251/ba/b","ext":"mp3",...},
//                  {"label":"MP4","kind":"video","url":"https://direct/file.mp4",...}]}   (direct file)
//   Audio rows are the site's own audio-only tracks (exact format id, kept
//   as sent; best = abr x codec weight, see CodecWeight) and MP3 encodes at
//   bitrates not above the source (Mp3Rates). A format "x:<m4a|mp3>[:<kbps>]|
//   <selector>" downloads <selector> and extracts the audio (yt-dlp -x; m4a
//   copies AAC, mp3 encodes at <kbps>, see ndm_media.cpp). "filesize" is
//   yt-dlp's (exact, or "approx":true for filesize_approx), 0 = unknown.
//   <- {"id":11,"status":"error","error":"no_media","detail":"<yt-dlp message>"}
//      (+ "timedOut":true when yt-dlp hit the time limit; not cached)
//   Runs "yt-dlp -J" in NDMan.exe (worker thread, ~5-20 s), cached 5 min per
//   page + cookie identity ("cookieStore" + cookies). pageUrl must be http(s).
//   Optional "cookieList":[{name,value,domain,hostOnly,path,secure,httpOnly,
//   expirationDate}] (exact browser cookies for pageUrl) becomes yt-dlp's
//   cookies.txt; the plain "cookies" header string is the fallback.
//
//   addDownload for a video picked from that list adds:
//       "kind":"media","pageUrl":"..","mediaFormat":"<format>","qualityLabel":"1080p MP4",
//       "cookieList":[...]
//   ("url" = pageUrl). Entries with "url" are sent as normal downloads.
//
// "id" is optional and echoed back verbatim (number or string).
// When NDMan.exe is not running, NDMMsgHost.exe answers "ping" itself with
// "running":false, and starts NDMan.exe ("NDMan.exe -tray") for any other
// action. Errors: bad_json, missing_action, unknown_action, bad_url,
// no_links, ndm_not_ready, post_failed, ndm_unreachable, reply_too_large,
// no_media.
// ---------------------------------------------------------------------------
#include "ndm_msghost.h"
#include "../common/ndm_common.h"
#ifndef NDM_MSGHOST_STANDALONE
#include "../media/ndm_media.h"
#endif

#include <sddl.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

// Chrome: host -> browser max 1 MB; browser -> host max 64 MiB (we accept 16).
constexpr DWORD kMaxOutMsg  = 1024 * 1024;
constexpr DWORD kMaxInMsg   = 16u * 1024u * 1024u;
constexpr int   kMaxClients = 16;
constexpr size_t kMaxLinks  = 5000;
constexpr size_t kMaxCookies = 400;

// ===========================================================================
// UTF-8 <-> UTF-16
// ===========================================================================
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
    return w;
}

void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// ===========================================================================
// Minimal but complete JSON reader (RFC 8259) + string escaper
// ===========================================================================
struct JVal
{
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool                                       b = false;
    double                                     n = 0.0;
    std::string                                s;        // UTF-8
    std::vector<JVal>                          arr;
    std::vector<std::pair<std::string, JVal>>  obj;

    const JVal* Get(const char* key) const
    {
        if (type != Obj) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string GetStr(const char* key) const
    {
        const JVal* v = Get(key);
        return (v && v->type == Str) ? v->s : std::string();
    }
    bool HasStr(const char* key) const
    {
        const JVal* v = Get(key);
        return v && v->type == Str;
    }
};

class JParser
{
public:
    JParser(const char* p, size_t n) : p_(p), end_(p + n) {}

    bool Parse(JVal& out)
    {
        Ws();
        if (!Value(out, 0)) return false;
        Ws();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;

    void Ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
    bool Lit(const char* lit)
    {
        size_t n = strlen(lit);
        if ((size_t)(end_ - p_) < n || memcmp(p_, lit, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool Value(JVal& v, int depth)
    {
        if (depth > 64 || p_ >= end_) return false;
        switch (*p_) {
        case '{': return Object(v, depth);
        case '[': return Array(v, depth);
        case '"': v.type = JVal::Str; return String(v.s);
        case 't': v.type = JVal::Bool; v.b = true;  return Lit("true");
        case 'f': v.type = JVal::Bool; v.b = false; return Lit("false");
        case 'n': v.type = JVal::Null; return Lit("null");
        default:  v.type = JVal::Num; return Number(v.n);
        }
    }

    bool Object(JVal& v, int depth)
    {
        v.type = JVal::Obj;
        ++p_;                       // '{'
        Ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            Ws();
            if (p_ >= end_ || *p_ != '"') return false;
            std::string key;
            if (!String(key)) return false;
            Ws();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            Ws();
            JVal child;
            if (!Value(child, depth + 1)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            Ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }

    bool Array(JVal& v, int depth)
    {
        v.type = JVal::Arr;
        ++p_;                       // '['
        Ws();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            Ws();
            JVal child;
            if (!Value(child, depth + 1)) return false;
            v.arr.push_back(std::move(child));
            Ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool Hex4(unsigned& out)
    {
        if (end_ - p_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool String(std::string& out)
    {
        ++p_;                       // opening quote
        out.clear();
        while (p_ < end_) {
            unsigned char c = (unsigned char)*p_++;
            if (c == '"') return true;
            if (c < 0x20) return false;          // raw control char not allowed
            if (c != '\\') { out += (char)c; continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                unsigned cu = 0;
                if (!Hex4(cu)) return false;
                if (cu >= 0xD800 && cu <= 0xDBFF) {
                    // High surrogate: must be followed by \uDC00-\uDFFF
                    unsigned lo = 0;
                    const char* save = p_;
                    if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            AppendUtf8(out, 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00));
                            break;
                        }
                    }
                    p_ = save;
                    AppendUtf8(out, 0xFFFD);     // lone high surrogate
                } else if (cu >= 0xDC00 && cu <= 0xDFFF) {
                    AppendUtf8(out, 0xFFFD);     // lone low surrogate
                } else {
                    AppendUtf8(out, cu);
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool Number(double& out)
    {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        if (p_ >= end_) return false;
        if (*p_ == '0') ++p_;
        else if (*p_ >= '1' && *p_ <= '9') { while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_; }
        else return false;
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        std::string tmp(s, p_);
        out = strtod(tmp.c_str(), nullptr);
        return true;
    }
};

bool ParseJson(const std::string& text, JVal& out)
{
    JParser parser(text.data(), text.size());
    return parser.Parse(out) && out.type == JVal::Obj;
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
    return out;
}

// '"id":<v>,' prefix for a reply (empty when the request had no usable id)
std::string IdPrefix(const JVal* req)
{
    if (!req) return std::string();
    const JVal* id = req->Get("id");
    if (!id) return std::string();
    if (id->type == JVal::Str) return "\"id\":" + JsonEscape(id->s) + ",";
    if (id->type == JVal::Num && std::isfinite(id->n)) {
        char buf[64];
        if (id->n == std::floor(id->n) && std::fabs(id->n) < 9.0e15)
            sprintf_s(buf, "%.0f", id->n);
        else
            sprintf_s(buf, "%.17g", id->n);
        return std::string("\"id\":") + buf + ",";
    }
    return std::string();
}

std::string ErrorReply(const JVal* req, const char* code)
{
    return "{" + IdPrefix(req) + "\"status\":\"error\",\"error\":\"" + code + "\"}";
}

// ===========================================================================
// Length-prefixed message I/O
// ===========================================================================
// Overlapped (stop-aware) when ev/stop are given, synchronous otherwise.
// Loops until the full count is transferred (ReadFile on pipes may return
// partial data).
bool IoExact(HANDLE h, bool isWrite, void* buf, DWORD len, HANDLE ev, HANDLE stop)
{
    BYTE* p = static_cast<BYTE*>(buf);
    while (len > 0) {
        DWORD done = 0;
        if (!ev) {
            BOOL ok = isWrite ? WriteFile(h, p, len, &done, nullptr)
                              : ReadFile(h, p, len, &done, nullptr);
            if (!ok || done == 0) return false;
        } else {
            OVERLAPPED ov = {};
            ov.hEvent = ev;
            ResetEvent(ev);
            BOOL ok = isWrite ? WriteFile(h, p, len, nullptr, &ov)
                              : ReadFile(h, p, len, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE waits[2] = { ev, stop };
            DWORD w = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(h, &ov, &done, FALSE) || done == 0) return false;
        }
        p   += done;
        len -= done;
    }
    return true;
}

bool ReadMsg(HANDLE h, std::string& msg, DWORD maxLen, HANDLE ev = nullptr, HANDLE stop = nullptr)
{
    uint32_t len = 0;
    if (!IoExact(h, false, &len, 4, ev, stop)) return false;
    if (len > maxLen) return false;               // cannot resync: drop link
    try {
        msg.assign(len, '\0');
    } catch (...) {
        return false;                             // out of memory: drop link
    }
    if (len == 0) return true;
    return IoExact(h, false, &msg[0], len, ev, stop);
}

bool WriteMsg(HANDLE h, const std::string& msg, HANDLE ev = nullptr, HANDLE stop = nullptr)
{
    if (msg.size() > kMaxInMsg) return false;
    // Single buffer so the length and body go out in one write.
    std::string frame;
    try {
        frame.assign(4 + msg.size(), '\0');
    } catch (...) {
        return false;
    }
    uint32_t len = (uint32_t)msg.size();
    memcpy(&frame[0], &len, 4);
    if (!msg.empty()) memcpy(&frame[4], msg.data(), msg.size());
    return IoExact(h, true, &frame[0], (DWORD)frame.size(), ev, stop);
}

bool HasCtlOrSpace(const std::string& u)
{
    for (unsigned char c : u)
        if (c <= 0x20 || c == 0x7F) return true;
    return false;
}

bool IsSupportedUrl(const std::string& u)
{
    auto starts = [&](const char* pre) {
        size_t n = strlen(pre);
        return u.size() > n && _strnicmp(u.c_str(), pre, n) == 0;
    };
    if (HasCtlOrSpace(u)) return false;
    // ftp:// is not accepted: the engine (WinHTTP) has no FTP.
    return ((starts("http://") || starts("https://")) && u.size() < 32768) ||
           (starts("magnet:?") && u.size() < 65536);
}

// http(s) only: what yt-dlp may be pointed at.
bool IsHttpUrl(const std::string& u)
{
    return !HasCtlOrSpace(u) && u.size() < 8192 &&
           ((u.size() > 7 && _strnicmp(u.c_str(), "http://", 7) == 0) ||
            (u.size() > 8 && _strnicmp(u.c_str(), "https://", 8) == 0));
}

UINT64 ToSize(const JVal* v)
{
    if (!v) return 0;
    if (v->type == JVal::Num && std::isfinite(v->n) && v->n > 0 && v->n < 1.8e19)
        return (UINT64)v->n;
    if (v->type == JVal::Str) return _strtoui64(v->s.c_str(), nullptr, 10);
    return 0;
}

bool ToBool(const JVal* v, bool def)
{
    if (!v) return def;
    if (v->type == JVal::Bool) return v->b;
    if (v->type == JVal::Num)  return v->n != 0;
    return def;
}

// Automatic browser capture switch (NDM Options -> Browser). NDMan mirrors
// its ndm.ini setting to HKCU\Software\Devnetra\NDM\BrowserMonitor when
// options are applied; a missing value means on (the default). Read from
// the registry so the relay can answer while NDMan.exe is not running.
bool CaptureEnabled()
{
    DWORD on = 1, sz = sizeof(on);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Devnetra\\NDM", L"BrowserMonitor",
                     RRF_RT_REG_DWORD, nullptr, &on, &sz) != ERROR_SUCCESS)
        return true;
    return on != 0;
}

// Ping reply ("{...}"): "capture" tells the extension whether NDM takes
// automatic captures right now, so it can leave downloads alone otherwise.
std::string PingReply(const JVal* req, bool running)
{
    return "{" + IdPrefix(req) + "\"status\":\"ok\",\"app\":\"NDM\",\"version\":\"" NDM_VERSION_STR
           "\",\"running\":" + (running ? "true" : "false") +
           ",\"capture\":" + (CaptureEnabled() ? "true" : "false") + "}";
}

// ===========================================================================
// Embedded server state
// ===========================================================================
std::atomic<HWND>    g_mainWnd { nullptr };
HANDLE               g_serverThread = nullptr;
HANDLE               g_stopEvent    = nullptr;
std::mutex           g_clientsLock;
std::vector<HANDLE>  g_clientThreads;

// Wait (up to timeoutMs) for the NDM main window to exist: the pipe server
// starts before the window is created, and a relay may connect right after
// launching NDMan.
HWND WaitMainWindow(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    for (;;) {
        HWND h = g_mainWnd.load();
        if (h && IsWindow(h)) return h;
        h = FindWindowW(NDM_WINDOW_CLASS, nullptr);
        if (h) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return h;
        }
        if (GetTickCount() - start >= timeoutMs) return nullptr;
        if (g_stopEvent && WaitForSingleObject(g_stopEvent, 100) == WAIT_OBJECT_0) return nullptr;
        if (!g_stopEvent) Sleep(100);
    }
}

// Queued in-process; only a token is posted (see NdmPostBrowserRequest).
bool PostRequest(HWND hWnd, NdmBrowserRequest* req)
{
    return NdmPostBrowserRequest(hWnd, req);      // takes ownership either way
}

// Lower-case host of an http(s) URL ("" when there is none).
std::string HostOfUrl(const std::string& url)
{
    size_t p = url.find("://");
    if (p == std::string::npos) return std::string();
    p += 3;
    size_t e = url.find_first_of("/?#", p);
    std::string h = url.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t at = h.rfind('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == '[') {                       // [IPv6]:port
        size_t rb = h.find(']');
        h = rb == std::string::npos ? std::string() : h.substr(0, rb + 1);
    } else {
        size_t colon = h.find(':');
        if (colon != std::string::npos) h = h.substr(0, colon);
    }
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    return h;
}

bool CookieFieldOk(const std::string& s, size_t maxLen)
{
    if (s.size() > maxLen) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;          // tab / CR / LF would break the file
    return true;
}

// Structured browser cookies ({name,value,domain,hostOnly,path,secure,httpOnly,
// expirationDate}) -> Netscape cookies.txt lines (UTF-8, no header) for yt-dlp.
// Only cookies whose domain matches `host` are kept (they were read for that
// exact URL by the extension).
std::string CookieJarFromList(const JVal* list, const std::string& host)
{
    std::string out;
    if (!list || list->type != JVal::Arr || host.empty()) return out;
    size_t n = 0;
    for (const JVal& c : list->arr) {
        if (c.type != JVal::Obj || n >= kMaxCookies) continue;
        std::string name = c.GetStr("name"), value = c.GetStr("value");
        std::string domain = c.GetStr("domain"), path = c.GetStr("path");
        if (!CookieFieldOk(name, 4096) || !CookieFieldOk(value, 16384) ||
            !CookieFieldOk(path, 4096) || name.find_first_of(" ;=") != std::string::npos)
            continue;
        if (name.empty() && value.empty()) continue;
        bool hostOnly = ToBool(c.Get("hostOnly"), false);
        bool secure   = ToBool(c.Get("secure"), false);
        bool httpOnly = ToBool(c.Get("httpOnly"), false);
        for (auto& ch : domain) ch = (char)tolower((unsigned char)ch);
        while (!domain.empty() && domain[0] == '.') domain.erase(0, 1);
        if (domain.empty() || domain.size() > 253 ||
            domain.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_") != std::string::npos)
            continue;
        // domain-match: host == domain, or (not host-only) host ends with "." + domain
        bool match = host == domain ||
                     (!hostOnly && host.size() > domain.size() &&
                      host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
                      host[host.size() - domain.size() - 1] == '.');
        if (!match) continue;
        if (path.empty() || path[0] != '/') path = "/";
        long long exp = 0;                                  // 0 = session cookie
        const JVal* e = c.Get("expirationDate");
        if (e && e->type == JVal::Num && std::isfinite(e->n) && e->n > 0 && e->n < 3.0e11)
            exp = (long long)e->n;
        char expBuf[32];
        sprintf_s(expBuf, "%lld", exp);
        if (httpOnly) out += "#HttpOnly_";
        out += hostOnly ? domain : "." + domain;
        out += hostOnly ? "\tFALSE\t" : "\tTRUE\t";
        out += path;
        out += secure ? "\tTRUE\t" : "\tFALSE\t";
        out += expBuf;
        out += "\t" + name + "\t" + value + "\n";
        ++n;
    }
    return out;
}

void FillCommon(NdmBrowserRequest& r, const JVal& src)
{
    if (src.HasStr("url"))       r.url       = Utf8ToWide(src.GetStr("url"));
    if (src.HasStr("filename"))  r.filename  = Utf8ToWide(src.GetStr("filename").substr(0, 1024));
    if (src.HasStr("referrer") && IsSupportedUrl(src.GetStr("referrer")))
                                 r.referrer  = Utf8ToWide(src.GetStr("referrer"));
    if (src.HasStr("cookies"))   r.cookies   = Utf8ToWide(src.GetStr("cookies"));
    if (src.HasStr("userAgent")) r.userAgent = Utf8ToWide(src.GetStr("userAgent").substr(0, 1024));
    if (src.HasStr("mime"))      r.mime      = Utf8ToWide(src.GetStr("mime").substr(0, 200));
    if (src.Get("fileSize"))     r.fileSize  = ToSize(src.Get("fileSize"));
    if (src.Get("silent"))       r.silent    = ToBool(src.Get("silent"), r.silent);
    if (src.HasStr("kind"))      r.kind      = src.GetStr("kind") == "media" ? NDM_KIND_MEDIA : NDM_KIND_HTTP;
    // yt-dlp input: http(s) pages only (never file:, data:, ...).
    std::string page = src.GetStr("pageUrl");
    if (!page.empty() && IsHttpUrl(page)) r.pageUrl = Utf8ToWide(page);
    else if (src.HasStr("pageUrl"))       r.pageUrl.clear();
    if (src.HasStr("mediaFormat"))  r.mediaFormat  = Utf8ToWide(src.GetStr("mediaFormat").substr(0, 3200));
    if (src.HasStr("qualityLabel")) r.qualityLabel = Utf8ToWide(src.GetStr("qualityLabel").substr(0, 100));
    if (r.kind == NDM_KIND_MEDIA && r.pageUrl.empty() && src.HasStr("url") && IsHttpUrl(src.GetStr("url")))
        r.pageUrl = r.url;
    if (r.kind == NDM_KIND_MEDIA) {
        if (const JVal* cl = src.Get("cookieList")) {
            std::string page8 = page.empty() ? src.GetStr("url") : page;
            r.cookieJar = Utf8ToWide(CookieJarFromList(cl, HostOfUrl(page8)));
        }
    } else {
        r.cookieJar.clear();
    }
}

#ifndef NDM_MSGHOST_STANDALONE
// ===========================================================================
// getMedia — yt-dlp -J -> short quality list
// ===========================================================================
double NumOf(const JVal* v)
{
    if (!v) return 0;
    if (v->type == JVal::Num && std::isfinite(v->n)) return v->n;
    if (v->type == JVal::Str) return atof(v->s.c_str());
    return 0;
}

bool IsNone(const JVal& f, const char* key)        // codec explicitly "none"
{
    return f.GetStr(key) == "none";
}

// filesize, else filesize_approx, else tbr * duration. *approx set when estimated.
double SizeOf(const JVal& f, double duration, bool* approx)
{
    double s = NumOf(f.Get("filesize"));
    if (s > 0) return s;
    s = NumOf(f.Get("filesize_approx"));
    if (s > 0) { *approx = true; return s; }
    double tbr = NumOf(f.Get("tbr"));
    if (tbr > 0 && duration > 0) { *approx = true; return tbr * 125.0 * duration; }
    return 0;
}

std::string Upper(std::string s)
{
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string FmtEntry(const std::string& label, const std::string& format, const std::string& url,
                     const std::string& ext, int height, double size, bool approx, const char* kind)
{
    std::string o = "{\"label\":" + JsonEscape(label) + ",\"kind\":\"" + kind + "\"";
    if (!format.empty()) o += ",\"format\":" + JsonEscape(format);
    if (!url.empty())    o += ",\"url\":" + JsonEscape(url);
    o += ",\"ext\":" + JsonEscape(ext) + ",\"height\":" + std::to_string(height);
    char buf[48];
    sprintf_s(buf, "%.0f", size > 0 ? size : 0.0);
    o += std::string(",\"filesize\":") + buf + ",\"approx\":" + (approx ? "true" : "false") + "}";
    return o;
}

// ---------------------------------------------------------------------------
// Audio rows: the site's own audio-only tracks (real codec and bitrate, no
// conversion) and MP3 encodes at honest bitrates.
// ---------------------------------------------------------------------------
std::string LowerA(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Codec family of an audio format: "opus" "aac" "vorbis" "mp3" "flac" "alac"
// "ac3" "eac3" "pcm", or "" when neither yt-dlp nor the address says.
std::string AudioFamily(const JVal& f)
{
    const std::string ac = LowerA(f.GetStr("acodec")), ext = LowerA(f.GetStr("ext"));
    auto starts = [&](const char* p) { return ac.compare(0, strlen(p), p) == 0; };
    if (starts("opus")) return "opus";
    if (starts("mp4a.40.34") || starts("mp4a.6b") || starts("mp4a.69") || starts("mp3")) return "mp3";
    if (starts("mp4a") || starts("aac")) return "aac";
    if (starts("vorbis")) return "vorbis";
    if (starts("flac")) return "flac";
    if (starts("alac")) return "alac";
    if (starts("ec-3") || starts("eac3")) return "eac3";
    if (starts("ac-3") || starts("ac3")) return "ac3";
    if (starts("pcm") || starts("wav")) return "pcm";
    if (!ac.empty() && ac != "unknown") return "";
    // No codec in the metadata: the container, or X's ".../mp4a/128000/..." address.
    const std::string url = LowerA(f.GetStr("url"));
    if (url.find("/mp4a/") != std::string::npos || ext == "m4a" || ext == "aac") return "aac";
    if (ext == "mp3") return "mp3";
    if (ext == "opus") return "opus";
    if (ext == "flac") return "flac";
    return "";
}

// How good a codec sounds per bit, relative to AAC (1.0). Best audio =
// highest abr x weight, so at similar bitrates Opus > Vorbis > AAC > MP3:
// Opus 160 (208) beats AAC 128 (128), AAC 256 (256) beats Opus 160 (208).
// Lossless always wins.
double CodecWeight(const std::string& fam)
{
    if (fam == "opus")   return 1.3;
    if (fam == "vorbis") return 1.1;
    if (fam == "aac")    return 1.0;
    if (fam == "eac3")   return 0.9;
    if (fam == "mp3" || fam == "ac3") return 0.8;
    if (fam == "flac" || fam == "alac" || fam == "pcm") return 4.0;
    return 0.9;
}

// kbps of an audio-only format: abr, else (audio only) tbr; 0 = unknown.
double AudioKbps(const JVal& f)
{
    double k = NumOf(f.Get("abr"));
    if (k <= 0) k = NumOf(f.Get("tbr"));
    return k > 0 && k < 100000 ? k : 0;
}

// yt-dlp's own size only: filesize (exact), else filesize_approx (*approx), else 0.
double ReportedSize(const JVal& f, bool* approx)
{
    *approx = false;
    double s = NumOf(f.Get("filesize"));
    if (s > 0) return s;
    s = NumOf(f.Get("filesize_approx"));
    if (s > 0) { *approx = true; return s; }
    return 0;
}

// A format id we can hand to -f as is (no selector syntax inside it).
bool PlainFormatId(const std::string& id)
{
    if (id.empty() || id.size() > 100) return false;
    for (char c : id)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

// MP3 bitrates for a source of kbps: the highest standard rate not above it
// (2 kbps tolerance: YouTube's "128k" AAC reports 129.8), plus 128 when the
// source reaches it, plus 320 for sources of 256 kbps and more:
// 160 -> 160, 128; 128 -> 128; 256 -> 320, 256, 128; 106 -> 96. Same rule as
// mp3Rates() in background.js.
std::vector<int> Mp3Rates(double kbps)
{
    std::vector<int> out;
    if (kbps <= 0) return out;
    if (kbps >= 256) out.push_back(320);
    static const int kStd[] = { 256, 192, 160, 128, 96, 64, 48, 32 };
    for (int b : kStd) if (b <= kbps + 2) { out.push_back(b); break; }
    if (kbps + 2 >= 128 && std::find(out.begin(), out.end(), 128) == out.end()) out.push_back(128);
    return out;
}

std::string FamilyName(const std::string& fam, const std::string& ext)
{
    if (fam == "aac")    return "M4A";
    if (fam == "opus")   return "Opus";
    if (fam == "vorbis") return "Vorbis";
    if (fam == "mp3")    return "MP3";
    if (fam == "flac")   return "FLAC";
    if (fam == "alac")   return "ALAC";
    if (fam == "eac3")   return "E-AC-3";
    if (fam == "ac3")    return "AC-3";
    if (fam == "pcm")    return "WAV";
    return Upper(ext.empty() ? std::string("audio") : ext);
}

// "Opus 160 kbps", "M4A 5.1 384 kbps", "MP4" (nothing known but the container).
std::string AudioDesc(const JVal& f)
{
    std::string d = FamilyName(AudioFamily(f), f.GetStr("ext"));
    const int ch = (int)NumOf(f.Get("audio_channels"));
    if (ch == 6) d += " 5.1"; else if (ch == 8) d += " 7.1"; else if (ch > 2) d += " " + std::to_string(ch) + "ch";
    const double k = AudioKbps(f);
    if (k > 0) d += " " + std::to_string((long long)std::llround(k)) + " kbps";
    return d;
}

// Rows for the original audio tracks (best first) and the MP3 encodes.
// `audio`: audio-only formats; `muxed`: the best video format, for sites
// with no separate audio (its audio track is what -x extracts).
void AppendAudioRows(std::vector<std::string>& out, const std::vector<const JVal*>& audio, const JVal* muxed)
{
    // Original language only (YouTube auto-dubs: language_preference 10 =
    // original, 5 = default, -10 = audio description).
    double maxPref = -1e9;
    for (const JVal* f : audio) {
        const JVal* p = f->Get("language_preference");
        maxPref = std::max(maxPref, p && p->type == JVal::Num ? p->n : -1.0);
    }
    struct Track { const JVal* f; std::string fam; double kbps, score; bool exact; };
    std::vector<Track> tracks;
    for (const JVal* f : audio) {
        const JVal* p = f->Get("language_preference");
        const double pref = p && p->type == JVal::Num ? p->n : -1.0;
        if (maxPref >= 5 && pref < maxPref) continue;
        if (!PlainFormatId(f->GetStr("format_id"))) continue;
        Track t{ f, AudioFamily(*f), AudioKbps(*f), 0, NumOf(f->Get("filesize")) > 0 };
        t.score = t.kbps * CodecWeight(t.fam);
        tracks.push_back(t);
    }
    // Tracks nothing is known about (YouTube's HLS audio copies: no codec, no
    // bitrate) only when there is nothing better to show.
    if (std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.kbps > 0; }))
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [](const Track& t) { return t.kbps <= 0; }), tracks.end());
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.exact && !b.exact;
    });
    // One row per codec / bitrate / layout (YouTube lists near-identical copies).
    std::vector<Track> rows;
    for (const Track& t : tracks) {
        const std::string desc = AudioDesc(*t.f);
        bool dup = false;
        for (const Track& r : rows) if (AudioDesc(*r.f) == desc) { dup = true; break; }
        if (!dup) rows.push_back(t);
        if (rows.size() >= 4) break;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const JVal& f = *rows[i].f;
        const std::string id = f.GetStr("format_id"), ext = LowerA(f.GetStr("ext"));
        // As the site sends it (.m4a AAC, .webm Opus, ...): -f <id>, no
        // post-processing. AAC in an MP4 / HLS container: "x:m4a" copies the
        // AAC stream into .m4a (yt-dlp -x uses -acodec copy for AAC), no re-encode.
        std::string fmt = id, outExt = ext.empty() ? std::string("m4a") : ext;
        if (rows[i].fam == "aac" && ext != "m4a" && ext != "aac") { fmt = "x:m4a|" + id; outExt = "m4a"; }
        bool approx = false;
        const double size = ReportedSize(f, &approx);
        const std::string desc = AudioDesc(f);
        out.push_back(FmtEntry(i == 0 ? "Best Audio (" + desc + ")" : "Audio " + desc, fmt, "", outExt, 0, size, approx, "audio"));
    }
    // MP3 encodes from the highest-bitrate track, never above its bitrate.
    const Track* src = nullptr;
    for (const Track& t : tracks)
        if (!src || t.kbps > src->kbps || (t.kbps == src->kbps && t.score > src->score)) src = &t;
    std::string from;
    double kbps = 0;
    if (src) { from = src->f->GetStr("format_id") + "/ba/b"; kbps = src->kbps; }
    else if (muxed) {
        // No separate audio: the video file's own track.
        from = "ba/b";
        kbps = NumOf(muxed->Get("abr"));
        const std::string fam = AudioFamily(*muxed);
        out.push_back(FmtEntry(fam == "aac" || fam.empty() ? "Audio M4A" : "Audio M4A (converted)",
                               "x:m4a|ba/b", "", "m4a", 0, 0, false, "audio"));
    } else {
        return;
    }
    const std::vector<int> rates = Mp3Rates(kbps);
    for (int b : rates)
        out.push_back(FmtEntry("MP3 " + std::to_string(b) + " kbps (converted)", "x:mp3:" + std::to_string(b) + "|" + from,
                               "", "mp3", 0, 0, false, "audio"));
    if (rates.empty())
        out.push_back(FmtEntry("MP3 (converted)", "x:mp3|" + from, "", "mp3", 0, 0, false, "audio"));
}

// Builds the "ok" reply ("{...}", without id) from yt-dlp's JSON; empty = nothing usable.
std::string BuildMediaReply(const JVal& root)
{
    const JVal* info = &root;
    const JVal* entries = root.Get("entries");
    if (root.GetStr("_type") == "playlist" && entries && entries->type == JVal::Arr) {
        info = nullptr;
        for (const JVal& e : entries->arr) if (e.type == JVal::Obj) { info = &e; break; }
        if (!info) return std::string();
    }
    const double duration = NumOf(info->Get("duration"));
    std::string head = "\"status\":\"ok\",\"title\":" + JsonEscape(info->GetStr("title")) +
                       ",\"thumbnail\":" + JsonEscape(info->GetStr("thumbnail")) +
                       ",\"duration\":" + std::to_string((long long)duration) +
                       ",\"extractor\":" + JsonEscape(info->GetStr("extractor_key"));

    std::vector<const JVal*> fmts;
    const JVal* fl = info->Get("formats");
    if (fl && fl->type == JVal::Arr) { for (const JVal& f : fl->arr) if (f.type == JVal::Obj) fmts.push_back(&f); }
    else if (info->HasStr("url")) fmts.push_back(info);

    std::vector<std::string> out;

    // Direct media file (generic extractor on a .mp4/.mp3 URL): plain download.
    const JVal* direct = info->Get("direct");
    if (direct && direct->type == JVal::Bool && direct->b && info->HasStr("url")) {
        bool approx = false;
        std::string ext = info->GetStr("ext");
        int h = (int)NumOf(info->Get("height"));
        static const char* kAudioExt[] = { "mp3", "m4a", "aac", "flac", "wav", "ogg", "oga", "opus", "wma", "weba" };
        bool isAudio = IsNone(*info, "vcodec");
        for (const char* a : kAudioExt) if (ext == a) isAudio = true;
        std::string label = isAudio ? "Audio " + Upper(ext) : h > 0 ? std::to_string(h) + "p " + Upper(ext) : Upper(ext);
        out.push_back(FmtEntry(label, "", info->GetStr("url"), ext, h, SizeOf(*info, duration, &approx), approx,
                               isAudio ? "audio" : "video"));
    } else {
        std::vector<const JVal*> video, audio;
        bool videoNoHeight = false;
        for (const JVal* f : fmts) {
            std::string ext = f->GetStr("ext"), proto = f->GetStr("protocol");
            if (ext == "mhtml" || proto == "mhtml") continue;              // storyboards
            if (f->GetStr("format_id").find("drc") != std::string::npos) continue;
            bool vNone = IsNone(*f, "vcodec"), aNone = IsNone(*f, "acodec");
            int h = (int)NumOf(f->Get("height"));
            if (!vNone && h > 0)            video.push_back(f);
            else if (vNone && !aNone)       audio.push_back(f);
            else if (!vNone && h <= 0)      videoNoHeight = true;
        }
        // Best audio: prefer AAC/m4a, stereo (what -S acodec:aac,channels:2 picks), then bitrate.
        const JVal* bestA = nullptr;
        auto aScore = [](const JVal* f) {
            std::string ac = f->GetStr("acodec"), ext = f->GetStr("ext");
            bool aac = ac.compare(0, 4, "mp4a") == 0 || ext == "m4a";
            bool stereo = NumOf(f->Get("audio_channels")) <= 2;      // -S channels:2
            return (aac ? 1e7 : 0) + (stereo ? 1e6 : 0) + NumOf(f->Get("abr")) + NumOf(f->Get("tbr")) * 1e-3;
        };
        for (const JVal* f : audio) if (!bestA || aScore(f) > aScore(bestA)) bestA = f;

        std::vector<int> heights;
        for (const JVal* f : video) {
            int h = (int)NumOf(f->Get("height"));
            if (std::find(heights.begin(), heights.end(), h) == heights.end()) heights.push_back(h);
        }
        std::sort(heights.rbegin(), heights.rend());
        if (heights.size() > 7) heights.resize(7);

        bool first = true;
        for (int h : heights) {
            const JVal* best = nullptr;
            auto vScore = [](const JVal* f) {
                bool avc = f->GetStr("vcodec").compare(0, 3, "avc") == 0;
                return (avc ? 1e9 : 0) + NumOf(f->Get("fps")) * 1e6 + NumOf(f->Get("tbr"));
            };
            for (const JVal* f : video)
                if ((int)NumOf(f->Get("height")) == h && (!best || vScore(f) > vScore(best))) best = f;
            if (!best) continue;
            bool approx = false;
            double size = SizeOf(*best, duration, &approx);
            bool hasAudio = best->Get("acodec") && !IsNone(*best, "acodec");
            bool mp4 = best->GetStr("ext") == "mp4";
            if (!hasAudio && bestA) {
                double as = SizeOf(*bestA, duration, &approx);
                if (size > 0) size += as;
                mp4 = mp4 && (bestA->GetStr("ext") == "m4a" || bestA->GetStr("ext") == "mp4");
            }
            std::string hs = std::to_string(h);
            std::string name = hs + "p " + (mp4 ? "MP4" : "MKV");
            // Top entry: the best video merged with the best audio (yt-dlp's -S
            // keeps H.264/AAC first so the result stays MP4 where possible).
            std::string sel = first ? std::string("bv*+ba/b")
                                    : "bv*[height<=" + hs + "]+ba/b[height<=" + hs + "]/b";
            out.push_back(FmtEntry(first ? "Best (" + name + ")" : name, sel, "", mp4 ? "mp4" : "mkv",
                                   h, size, approx, "video"));
            first = false;
        }
        if (heights.empty() && (videoNoHeight || (!fmts.empty() && audio.empty())))
            out.push_back(FmtEntry("Best quality", "bv*+ba/b", "", "mp4", 0, 0, false, "video"));
        if (!audio.empty() || !out.empty()) {
            // The muxed fallback: the best video format that carries audio.
            const JVal* muxed = nullptr;
            for (const JVal* f : video)
                if (f->Get("acodec") && !IsNone(*f, "acodec") &&
                    (!muxed || NumOf(f->Get("height")) > NumOf(muxed->Get("height")))) muxed = f;
            static const JVal kNone;
            AppendAudioRows(out, audio, muxed ? muxed : &kNone);
        }
    }
    if (out.empty()) return std::string();
    std::string r = "{" + head + ",\"formats\":[";
    for (size_t i = 0; i < out.size(); ++i) { if (i) r += ','; r += out[i]; }
    return r + "]}";
}

struct MediaCacheEntry
{
    std::mutex  run;          // held while yt-dlp runs (dedupes concurrent asks)
    bool        done = false;
    bool        ok   = false;
    bool        timedOut = false; // yt-dlp ran into the timeout: never cached
    DWORD       when = 0;
    std::string body;         // reply without id: "{...}"
};
std::mutex g_mediaLock;
std::map<std::string, std::shared_ptr<MediaCacheEntry>> g_mediaCache;

std::string GetMediaReply(const JVal& req, const std::string& id)
{
    std::string page = req.GetStr("pageUrl");
    if (page.empty()) page = req.GetStr("url");
    if (!IsHttpUrl(page)) return ErrorReply(&req, "bad_url");

    // Cache per page AND per cookie identity (store / incognito / container,
    // plus the cookies themselves): a private-window probe never answers a
    // normal-window request and vice versa.
    const std::string jar = CookieJarFromList(req.Get("cookieList"), HostOfUrl(page));
    const std::string ident = req.GetStr("cookieStore").substr(0, 128) + "|" +
        std::to_string(std::hash<std::string>()(req.GetStr("cookies") + "\n" + jar));
    const std::string key = ident + "|" + page;

    std::shared_ptr<MediaCacheEntry> e;
    {
        std::lock_guard<std::mutex> g(g_mediaLock);
        DWORD now = GetTickCount();
        for (auto it = g_mediaCache.begin(); it != g_mediaCache.end();) {
            const auto& c = it->second;
            DWORD ttl = c->ok ? 5 * 60000 : 30000;
            if (c->done && (c->timedOut || now - c->when > ttl)) it = g_mediaCache.erase(it); else ++it;
        }
        if (g_mediaCache.size() > 256) g_mediaCache.clear();
        auto& slot = g_mediaCache[key];
        if (!slot) slot = std::make_shared<MediaCacheEntry>();
        e = slot;
    }
    std::lock_guard<std::mutex> run(e->run);
    if (!e->done) {
        NdmMediaProbeArgs a;
        a.pageUrl   = Utf8ToWide(page);
        a.cookies   = Utf8ToWide(req.GetStr("cookies"));
        a.cookieJar = Utf8ToWide(jar);
        a.userAgent = Utf8ToWide(req.GetStr("userAgent").substr(0, 1024));
        if (IsHttpUrl(req.GetStr("referrer"))) a.referrer = Utf8ToWide(req.GetStr("referrer"));
        a.timeoutMs = 60000;
        a.hAbort    = g_stopEvent;
        std::string json, body;
        std::wstring err;
        const ULONGLONG started = GetTickCount64();
        if (NdmMediaProbe(a, json, err)) {
            JVal root;
            if (ParseJson(json, root)) body = BuildMediaReply(root);
            if (body.empty()) err = L"No downloadable video was found on this page.";
        }
        if (!body.empty()) {
            e->ok   = true;
            e->body = body;
        } else {
            // A probe that used up its whole time budget timed out (yt-dlp was
            // killed): say so ("timedOut", additive) instead of only "no_media".
            e->timedOut = GetTickCount64() - started + 250 >= a.timeoutMs &&
                          WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0;
            int n = WideCharToMultiByte(CP_UTF8, 0, err.c_str(), (int)err.size(), nullptr, 0, nullptr, nullptr);
            std::string u((size_t)(n > 0 ? n : 0), '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, err.c_str(), (int)err.size(), &u[0], n, nullptr, nullptr);
            e->body = "{\"status\":\"error\",\"error\":\"no_media\",\"detail\":" + JsonEscape(u) +
                      (e->timedOut ? ",\"timedOut\":true}" : "}");
        }
        e->done = true;
        e->when = GetTickCount();
    }
    return "{" + id + e->body.substr(1);
}
#endif // NDM_MSGHOST_STANDALONE

} // namespace

#ifndef NDM_MSGHOST_STANDALONE
std::string NdmMsgHost::MediaReplyFromJson(const std::string& ytdlpJson)
{
    JVal root;
    return ParseJson(ytdlpJson, root) ? BuildMediaReply(root) : std::string();
}
#endif

// ===========================================================================
// HandleMessage
// ===========================================================================
std::string NdmMsgHost::HandleMessage(const std::string& json)
{
    JVal req;
    if (!ParseJson(json, req)) return ErrorReply(nullptr, "bad_json");

    const std::string action = req.GetStr("action");
    const std::string id     = IdPrefix(&req);
    if (action.empty()) return ErrorReply(&req, "missing_action");

    if (action == "ping") return PingReply(&req, true);

    if (action == "show" || action == "launch") {
        HWND hWnd = WaitMainWindow(10000);
        if (!hWnd) return ErrorReply(&req, "ndm_not_ready");
        ShowWindowAsync(hWnd, IsIconic(hWnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hWnd);
        return "{" + id + "\"status\":\"ok\"}";
    }

    if (action == "addDownload") {
        std::string url = req.GetStr("url");
        if (!IsSupportedUrl(url)) return ErrorReply(&req, "bad_url");

        // Automatic capture switched off in NDM Options -> tell the extension so
        // the browser keeps the download. Explicit "Download with NDM" still works.
        if (req.GetStr("source") == "capture" && !CaptureEnabled())
            return ErrorReply(&req, "disabled");

        HWND hWnd = WaitMainWindow(10000);
        if (!hWnd) return ErrorReply(&req, "ndm_not_ready");

        NdmBrowserRequest* r = new NdmBrowserRequest();
        FillCommon(*r, req);
        r->origin = NDM_ORIGIN_EXTERNAL;
        r->autoCapture = req.GetStr("source") == "capture";
        if (r->kind == NDM_KIND_MEDIA && r->pageUrl.empty()) {
            delete r;
            return ErrorReply(&req, "bad_url");
        }
        if (!PostRequest(hWnd, r)) return ErrorReply(&req, "post_failed");
        return "{" + id + "\"status\":\"accepted\"}";
    }

#ifndef NDM_MSGHOST_STANDALONE
    if (action == "getMedia") return GetMediaReply(req, id);
#endif

    if (action == "addDownloads") {
        // Batch from "Download all links": every link is queued with
        // silent=true unless the message says otherwise, so NDM does not
        // pop one dialog per link (the extension shows its own picker first).
        const JVal* links = req.Get("links");
        if (!links || links->type != JVal::Arr || links->arr.empty())
            return ErrorReply(&req, "no_links");

        HWND hWnd = WaitMainWindow(10000);
        if (!hWnd) return ErrorReply(&req, "ndm_not_ready");

        NdmBrowserRequest base;
        base.silent = true;
        FillCommon(base, req);
        base.url.clear();
        base.filename.clear();
        base.fileSize = 0;
        base.mime.clear();

        size_t accepted = 0, rejected = 0;
        for (const JVal& item : links->arr) {
            if (accepted >= kMaxLinks) { ++rejected; continue; }
            std::unique_ptr<NdmBrowserRequest> r(new NdmBrowserRequest(base));
            if (item.type == JVal::Str) {
                if (!IsSupportedUrl(item.s)) { ++rejected; continue; }
                r->url = Utf8ToWide(item.s);
            } else if (item.type == JVal::Obj) {
                if (!IsSupportedUrl(item.GetStr("url"))) { ++rejected; continue; }
                FillCommon(*r, item);
                if (r->kind == NDM_KIND_MEDIA && r->pageUrl.empty()) { ++rejected; continue; }
            } else {
                ++rejected;
                continue;
            }
            if (PostRequest(hWnd, r.release())) ++accepted;
            else ++rejected;
        }
        if (accepted == 0) return ErrorReply(&req, rejected ? "bad_url" : "no_links");
        return "{" + id + "\"status\":\"accepted\",\"count\":" + std::to_string(accepted) +
               ",\"rejected\":" + std::to_string(rejected) + "}";
    }

    return ErrorReply(&req, "unknown_action");
}

// ===========================================================================
// Pipe server (embedded in NDMan.exe)
// ===========================================================================
namespace {

// SID of the user a token / process runs as ("" on failure).
bool TokenUserSid(HANDLE tok, std::vector<BYTE>& sid)
{
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    if (!need || need > 4096) return false;
    std::vector<BYTE> buf(need);
    if (!GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) return false;
    PSID s = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    if (!IsValidSid(s)) return false;
    DWORD len = GetLengthSid(s);
    sid.assign(static_cast<BYTE*>(s), static_cast<BYTE*>(s) + len);
    return true;
}

bool ProcessUserSid(DWORD pid, std::vector<BYTE>& sid, HANDLE* hProcOut = nullptr)
{
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    HANDLE tok = nullptr;
    bool ok = OpenProcessToken(hp, TOKEN_QUERY, &tok) && TokenUserSid(tok, sid);
    if (tok) CloseHandle(tok);
    if (ok && hProcOut) *hProcOut = hp;
    else CloseHandle(hp);
    return ok;
}

bool OwnUserSid(std::vector<BYTE>& sid)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool ok = TokenUserSid(tok, sid);
    CloseHandle(tok);
    return ok;
}

bool SameSid(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
{
    return !a.empty() && a.size() == b.size() &&
           EqualSid(const_cast<BYTE*>(a.data()), const_cast<BYTE*>(b.data()));
}

DWORD OwnSessionId()
{
    DWORD s = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &s)) s = 0;
    return s;
}

// Server side: the connected client must be a process of our own user in our
// own session (the DACL already admits only the owner and SYSTEM).
bool ClientIsTrusted(HANDLE hPipe)
{
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(hPipe, &pid) || pid == 0) return false;
    std::vector<BYTE> mine, theirs;
    if (!OwnUserSid(mine) || !ProcessUserSid(pid, theirs)) return false;
    if (!SameSid(mine, theirs)) return false;
    DWORD sess = 0;
    return ProcessIdToSessionId(pid, &sess) && sess == OwnSessionId();
}

} // namespace

std::wstring NdmMsgHost::PipeName()
{
    std::vector<BYTE> sid;
    if (!OwnUserSid(sid)) return std::wstring();
    LPWSTR str = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(sid.data()), &str) || !str) return std::wstring();
    std::wstring name = NDM_PIPE_PREFIX + std::wstring(str) + L"-" + std::to_wstring(OwnSessionId());
    LocalFree(str);
    return name;
}

struct PipeClientCtx { HANDLE hPipe; };

DWORD WINAPI NdmMsgHost::PipeClientThread(LPVOID param)
{
    std::unique_ptr<PipeClientCtx> ctx(static_cast<PipeClientCtx*>(param));
    HANDLE hPipe = ctx->hPipe;
    HANDLE ev    = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (ev) {
        try {
            std::string msg;
            while (ReadMsg(hPipe, msg, kMaxInMsg, ev, g_stopEvent)) {
                std::string reply;
                try {
                    reply = HandleMessage(msg);
                } catch (...) {
                    reply = ErrorReply(nullptr, "internal");
                }
                if (reply.size() > kMaxOutMsg) reply = ErrorReply(nullptr, "reply_too_large");
                if (!WriteMsg(hPipe, reply, ev, g_stopEvent)) break;
            }
        } catch (...) {
            // out of memory etc.: drop this client
        }
        CloseHandle(ev);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}

// Returns false if (waitAll) some client thread did not exit in time.
static bool ReapClientThreads(bool waitAll)
{
    bool allExited = true;
    std::lock_guard<std::mutex> lock(g_clientsLock);
    for (auto it = g_clientThreads.begin(); it != g_clientThreads.end();) {
        bool exited = WaitForSingleObject(*it, waitAll ? 5000 : 0) == WAIT_OBJECT_0;
        if (exited || waitAll) {
            if (!exited) allExited = false;
            CloseHandle(*it);
            it = g_clientThreads.erase(it);
        } else {
            ++it;
        }
    }
    return allExited;
}

DWORD WINAPI NdmMsgHost::PipeServerThread(LPVOID /*param*/)
{
    // SYSTEM + owner (the user NDMan runs as) only. No mandatory label: NDMan
    // is never elevated, so the default medium label lets the medium relay in
    // and keeps low-integrity (sandboxed) processes out.
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &psd, nullptr))
        return 1;                                   // never fall back to a default DACL
    SECURITY_ATTRIBUTES sa = { sizeof(sa), psd, FALSE };

    const std::wstring name = PipeName();
    HANDLE connEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool   first  = true;

    while (connEv && !name.empty() && WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        ReapClientThreads(false);
        {
            // Do not even create an instance while all client slots are busy.
            std::unique_lock<std::mutex> lock(g_clientsLock);
            if ((int)g_clientThreads.size() >= kMaxClients) {
                lock.unlock();
                if (WaitForSingleObject(g_stopEvent, 200) == WAIT_OBJECT_0) break;
                continue;
            }
        }

        DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                         (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE hPipe = CreateNamedPipeW(
            name.c_str(), openMode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            kMaxClients, 64 * 1024, 64 * 1024, 0, &sa);
        if (hPipe == INVALID_HANDLE_VALUE) {
            // Name squatted / another instance alive / all instances busy: retry later.
            if (WaitForSingleObject(g_stopEvent, first ? 2000 : 300) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;

        OVERLAPPED ov = {};
        ov.hEvent = connEv;
        ResetEvent(connEv);
        bool connected = false;
        if (ConnectNamedPipe(hPipe, &ov)) {
            connected = true;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = { connEv, g_stopEvent };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                DWORD dummy = 0;
                if (w == WAIT_OBJECT_0) {
                    connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE) != FALSE;
                } else {
                    CancelIoEx(hPipe, &ov);
                    GetOverlappedResult(hPipe, &ov, &dummy, TRUE);
                }
            }
        }

        if (!connected || !ClientIsTrusted(hPipe)) {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }

        bool spawned = false;
        {
            std::lock_guard<std::mutex> lock(g_clientsLock);
            if ((int)g_clientThreads.size() < kMaxClients) {
                PipeClientCtx* ctx = new (std::nothrow) PipeClientCtx{ hPipe };
                HANDLE hT = ctx ? CreateThread(nullptr, 0, PipeClientThread, ctx, 0, nullptr) : nullptr;
                if (hT) { g_clientThreads.push_back(hT); spawned = true; }
                else    delete ctx;
            }
        }
        if (!spawned) {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }

    if (connEv) CloseHandle(connEv);
    LocalFree(psd);
    return 0;
}
void NdmMsgHost::StartListening()
{
    if (g_serverThread) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) return;
    g_serverThread = CreateThread(nullptr, 0, PipeServerThread, nullptr, 0, nullptr);
    if (!g_serverThread) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

void NdmMsgHost::StopListening()
{
    if (!g_serverThread) return;
    SetEvent(g_stopEvent);                  // unblocks connect + all client I/O
    WaitForSingleObject(g_serverThread, 5000);
    CloseHandle(g_serverThread);
    g_serverThread = nullptr;
    // A straggler still references g_stopEvent: leak it rather than close it.
    if (ReapClientThreads(true)) CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
}

void NdmMsgHost::SetMainWindow(HWND hWnd)
{
    g_mainWnd.store(hWnd);
}

// ===========================================================================
// Standalone relay — NDMMsgHost.exe
// ===========================================================================
#ifdef NDM_MSGHOST_STANDALONE

namespace {

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

// Volume serial + file index of a path (follows the name, not links).
bool FileIdOf(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& fi)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = GetFileInformationByHandle(h, &fi) != FALSE;
    CloseHandle(h);
    return ok;
}

// Relay side: the pipe server must be <our folder>\NDMan.exe running as our
// user (another user could pre-create a pipe with our name).
bool ServerIsTrusted(HANDLE hPipe)
{
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(hPipe, &pid) || pid == 0) return false;
    std::vector<BYTE> mine, theirs;
    HANDLE hp = nullptr;
    if (!OwnUserSid(mine) || !ProcessUserSid(pid, theirs, &hp)) return false;
    bool ok = SameSid(mine, theirs);
    if (ok) {
        wchar_t img[MAX_PATH * 2] = {};
        DWORD n = (DWORD)(sizeof(img) / sizeof(img[0]));
        ok = QueryFullProcessImageNameW(hp, 0, img, &n) != FALSE;
        const std::wstring want = ExeDir() + L"\\" NDM_EXE_NAME;
        if (ok && _wcsicmp(img, want.c_str()) != 0) {
            // Same file under another spelling (8.3 name, different case, ...)?
            BY_HANDLE_FILE_INFORMATION a = {}, b = {};
            ok = FileIdOf(img, a) && FileIdOf(want, b) &&
                 a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
                 a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
        }
    }
    CloseHandle(hp);
    return ok;
}

HANDLE OpenNdmPipe(const std::wstring& name, DWORD waitMs)
{
    if (name.empty()) return INVALID_HANDLE_VALUE;
    DWORD start = GetTickCount();
    for (;;) {
        // No GENERIC_WRITE: that includes FILE_CREATE_PIPE_INSTANCE.
        HANDLE h = CreateFileW(name.c_str(),
                               GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                               0, nullptr, OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (ServerIsTrusted(h)) return h;
            CloseHandle(h);                 // impostor: never talk to it
            SetLastError(ERROR_ACCESS_DENIED);
        }
        DWORD err     = GetLastError();
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= waitMs) return INVALID_HANDLE_VALUE;
        if (err == ERROR_PIPE_BUSY)
            WaitNamedPipeW(name.c_str(), (std::min)(1000ul, waitMs - elapsed));
        else
            Sleep(150);
    }
}

// Start "<dir of NDMMsgHost.exe>\NDMan.exe -tray" (never elevated), detached
// from the browser's job object (Firefox kills the whole job when the port
// closes).
bool LaunchNdm()
{
    std::wstring dir = ExeDir();
    std::wstring exe = dir + L"\\" NDM_EXE_NAME;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring cmd = L"\"" + exe + L"\" " NDM_LAUNCH_ARG;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    DWORD flags[] = { CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP,
                      CREATE_NEW_PROCESS_GROUP };
    for (DWORD f : flags) {
        std::vector<wchar_t> cl(cmd.begin(), cmd.end());
        cl.push_back(L'\0');
        if (CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, f,
                           nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        if (GetLastError() == ERROR_ELEVATION_REQUIRED) break;   // never prompt for UAC
    }
    return false;
}

} // namespace

int main(int, char**)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    try {
        return NdmMsgHost::RunStandalone();
    } catch (...) {
        return 3;
    }
}

int NdmMsgHost::RunStandalone()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hIn || hIn == INVALID_HANDLE_VALUE || !hOut || hOut == INVALID_HANDLE_VALUE)
        return 1;

    const std::wstring pipeName = PipeName();
    HANDLE hPipe    = INVALID_HANDLE_VALUE;
    bool   launched = false;

    auto closePipe = [&]() {
        if (hPipe != INVALID_HANDLE_VALUE) { CloseHandle(hPipe); hPipe = INVALID_HANDLE_VALUE; }
    };
    auto ensurePipe = [&](bool mayLaunch) -> bool {
        if (hPipe != INVALID_HANDLE_VALUE) return true;
        hPipe = OpenNdmPipe(pipeName, mayLaunch ? 1500 : 300);
        if (hPipe == INVALID_HANDLE_VALUE && mayLaunch) {
            if (!launched) launched = LaunchNdm();
            // First start after install can be slow (antivirus scanning the
            // new binaries), so wait generously for the pipe to appear.
            if (launched) hPipe = OpenNdmPipe(pipeName, 45000);
        }
        return hPipe != INVALID_HANDLE_VALUE;
    };

    std::string msg;
    for (;;) {
        // EOF on stdin = browser closed the port: exit.
        if (!ReadMsg(hIn, msg, kMaxInMsg)) break;

        std::string reply;
        try {
            JVal req;
            const bool parsed = ParseJson(msg, req);
            const std::string action = parsed ? req.GetStr("action") : std::string();
            const bool isPing = action == "ping";

            if (!parsed) {
                reply = ErrorReply(nullptr, "bad_json");
            } else if (!ensurePipe(!isPing)) {
                reply = isPing ? PingReply(&req, false) : ErrorReply(&req, "ndm_unreachable");
            } else {
                bool ok = false;
                if (WriteMsg(hPipe, msg)) {
                    ok = ReadMsg(hPipe, reply, kMaxInMsg);
                } else {
                    // Stale connection (NDMan restarted): reconnect and resend once.
                    closePipe();
                    if (ensurePipe(!isPing) && WriteMsg(hPipe, msg))
                        ok = ReadMsg(hPipe, reply, kMaxInMsg);
                }
                if (!ok) {
                    closePipe();
                    reply = ErrorReply(&req, "ndm_unreachable");
                }
            }
            if (reply.size() > kMaxOutMsg) reply = ErrorReply(&req, "reply_too_large");
        } catch (...) {
            closePipe();
            reply = ErrorReply(nullptr, "internal");
        }
        if (!WriteMsg(hOut, reply)) break;
    }

    closePipe();
    return 0;
}

#endif // NDM_MSGHOST_STANDALONE
