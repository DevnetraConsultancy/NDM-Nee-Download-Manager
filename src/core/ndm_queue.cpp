#include "ndm_queue.h"
#include "ndm_config.h"
#include "../network/ndm_http.h"
#include "../media/ndm_media.h"
#include "../torrent/ndm_torrent.h"
#include "../torrent/ndm_torrent_internal.h"
#include "../scheduler/ndm_scheduler.h"
#include <winsock2.h>
#include <iphlpapi.h>
#include <algorithm>
#include <string>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "iphlpapi.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

bool EnsureDirQ(const std::wstring& dirIn)
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
        if (!(parent.size() == 2 && parent[1] == L':')) EnsureDirQ(parent);
    }
    return CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring DirOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

int ClampConns(int n)
{
    if (n < 1)  n = 1;
    if (n > 32) n = 32;
    return n;
}

// ---------------------------------------------------------------------------
// XML helpers (minimal, no external XML lib)
// ---------------------------------------------------------------------------
std::wstring XmlEscape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 16);
    for (wchar_t c : s)
    {
        switch (c)
        {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:
                if (c < 0x20)
                {
                    wchar_t b[16];
                    swprintf_s(b, L"&#x%X;", (unsigned)c);
                    out += b;
                }
                else out += c;
                break;
        }
    }
    return out;
}

// Single pass, so "&amp;lt;" correctly becomes "&lt;".
std::wstring XmlUnescape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != L'&') { out += s[i]; continue; }
        size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 12) { out += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        wchar_t ch = 0;
        if      (ent == L"amp")  ch = L'&';
        else if (ent == L"lt")   ch = L'<';
        else if (ent == L"gt")   ch = L'>';
        else if (ent == L"quot") ch = L'"';
        else if (ent == L"apos") ch = L'\'';
        else if (ent.size() > 1 && ent[0] == L'#')
        {
            unsigned long v = (ent[1] == L'x' || ent[1] == L'X')
                              ? wcstoul(ent.c_str() + 2, nullptr, 16)
                              : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (v > 0 && v <= 0xFFFF) ch = (wchar_t)v;
        }
        if (ch) { out += ch; i = semi; }
        else    out += s[i];
    }
    return out;
}

// Extract attribute value from a tag string: ` attr="value"`
std::wstring GetAttr(const std::wstring& tag, const std::wstring& attr, bool* found = nullptr)
{
    std::wstring key = L" " + attr + L"=\"";
    size_t pos = tag.find(key);
    if (found) *found = false;
    if (pos == std::wstring::npos) return L"";
    pos += key.size();
    size_t end = tag.find(L'"', pos);
    if (end == std::wstring::npos) return L"";
    if (found) *found = true;
    return XmlUnescape(tag.substr(pos, end - pos));
}

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Current UTC time as a FILETIME value (100 ns since 1601).
UINT64 NowFt()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

UINT64 FtToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

std::wstring TrimW(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// "scheme://host:port" lower-cased (no user info), "" if not a URL.
std::wstring OriginOf(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return L"";
    size_t end = url.find_first_of(L"/?#", p + 3);
    std::wstring auth = url.substr(p + 3, end == std::wstring::npos ? std::wstring::npos : end - p - 3);
    size_t at = auth.find_last_of(L'@');
    if (at != std::wstring::npos) auth = auth.substr(at + 1);
    std::wstring o = url.substr(0, p) + L"://" + auth;
    for (wchar_t& c : o) c = (wchar_t)towlower(c);
    return o;
}

// Comparison key of a download address: trimmed, #fragment dropped, scheme
// and host lower-cased; magnet links by their info hash.
std::wstring UrlKey(const std::wstring& urlIn)
{
    std::wstring u = TrimW(urlIn);
    if (u.empty()) return L"";
    if (_wcsnicmp(u.c_str(), L"magnet:", 7) == 0)
    {
        std::string h = NdmTorrentMagnetHash(u);
        if (!h.empty()) return L"btih:" + std::wstring(h.begin(), h.end());
        return u;
    }
    u = NdmHttpEngine::NormalizeUrl(u);
    size_t p = u.find(L"://");
    if (p == std::wstring::npos) return u;
    size_t end = u.find_first_of(L"/?", p + 3);
    if (end == std::wstring::npos) end = u.size();
    for (size_t i = 0; i < end; ++i) u[i] = (wchar_t)towlower(u[i]);
    return u;
}

// Is any real network interface up with a gateway (i.e. a route out)?
bool NetConnected()
{
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 4 && r == ERROR_BUFFER_OVERFLOW; ++i)
    {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC,
                                 GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return false;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next)
        if (a->OperStatus == IfOperStatusUp && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
            a->IfType != IF_TYPE_TUNNEL && a->FirstUnicastAddress && a->FirstGatewayAddress)
            return true;
    return false;
}

int ClampPrio(int p) { return p < NDM_PRIO_LOW ? NDM_PRIO_LOW : (p > NDM_PRIO_HIGH ? NDM_PRIO_HIGH : p); }

// Start order: High before Normal before Low, then queue order, then id.
bool StartsBefore(const NdmDownload* a, const NdmDownload* b)
{
    if (a->priority != b->priority) return a->priority > b->priority;
    if (a->queueOrder != b->queueOrder) return a->queueOrder < b->queueOrder;
    return a->id < b->id;
}

bool OrderBefore(const NdmDownload* a, const NdmDownload* b)
{
    if (a->queueOrder != b->queueOrder) return a->queueOrder < b->queueOrder;
    return a->id < b->id;
}

} // namespace

// ---------------------------------------------------------------------------
// Thread proc — runs one download; the queue decides the final status.
// ---------------------------------------------------------------------------
static void DeleteMediaTempFiles(NdmDownload* dl) { NdmMediaDeleteTempFiles(dl); }

// Overwrite + drop the cookie strings of a finished download. Called under
// m_cs; the UI copies these fields only under LockFields.
static void WipeSecrets(NdmDownload* dl)
{
    NdmHttpEngine::LockFields(true);
    for (std::wstring* s : { &dl->cookies, &dl->cookieJar, &dl->cookiesEnc, &dl->cookieJarEnc })
    {
        if (!s->empty()) SecureZeroMemory(&(*s)[0], s->size() * sizeof(wchar_t));
        s->clear();
        s->shrink_to_fit();
    }
    NdmHttpEngine::UnlockFields(true);
}

DWORD WINAPI NdmQueue::ThreadProc(LPVOID param)
{
    NdmDownload* dl = reinterpret_cast<NdmDownload*>(param);
    NdmQueue& q = NdmQueue::GetInstance();

    NdmRunOptions opt;
    opt.setStatus   = false;
    opt.onChange    = &NdmQueue::OnEngineChange;
    opt.onChangeCtx = &q;
    EnterCriticalSection(&q.m_cs);
    opt.userNamed = q.m_userNamed.count(dl->id) != 0;
    // Only an explicit "Replace" in the File Info dialog may overwrite an
    // existing file; a user-named (browser / pipe supplied) name never does.
    opt.overwriteConfirmed = dl->overwriteConfirmed;
    LeaveCriticalSection(&q.m_cs);

    NdmRunResult r = (dl->kind == NDM_KIND_MEDIA)   ? NdmMediaRun(dl, opt)
                   : (dl->kind == NDM_KIND_TORRENT) ? NdmTorrentRun(dl, opt)
                                                    : NdmHttpEngine::Run(dl, opt);
    q.OnJobFinished(dl, (int)r);
    return 0;
}

void NdmQueue::OnEngineChange(NdmDownload* /*dl*/, void* ctx)
{
    // filename resolved / changed -> persist so a crash can resume it
    reinterpret_cast<NdmQueue*>(ctx)->AutoSave();
}

void NdmQueue::OnJobFinished(NdmDownload* dl, int result)
{
    bool zombie = false;
    bool wakeTimer = false;
    std::vector<std::pair<int, DWORD>> fire;

    EnterCriticalSection(&m_cs);
    m_running.erase(dl);
    if (m_zombies.count(dl))
    {
        m_zombies.erase(dl);
        zombie = true;
    }
    else
    {
        switch (result)
        {
        case NDM_RUN_COMPLETED:
            dl->status = NDM_STATUS_COMPLETED;
            dl->completedTime = NowFt();
            dl->errorKind = NDM_ERRKIND_NONE;
            dl->nextRetryTime = 0;
            dl->autoRetries = 0;
            m_failBytes.erase(dl->id);
            WipeSecrets(dl);             // session cookies are not kept after completion
            if (dl->completionAction != NDM_DONE_NONE && !m_shuttingDown)
                fire.push_back({ dl->completionAction, dl->id });
            dl->completionAction = NDM_DONE_NONE;
            m_allDoneSeenCompletion = true;
            break;
        case NDM_RUN_STOPPED:
            if (dl->status == NDM_STATUS_QUEUED)  break;    // resumed meanwhile
            dl->status = m_shuttingDown ? NDM_STATUS_QUEUED : NDM_STATUS_PAUSED;
            break;
        default:
            if (dl->status == NDM_STATUS_DOWNLOADING)
            {
                dl->status = NDM_STATUS_ERROR;
                if (dl->errorKind == NDM_ERRKIND_NONE) dl->errorKind = NDM_ERRKIND_FATAL;
                if (dl->errorKind == NDM_ERRKIND_TRANSIENT && !m_shuttingDown)
                {
                    // Auto-retry later; the count restarts whenever a run
                    // got further than the previous failed one.
                    UINT64 got = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
                    auto fb = m_failBytes.find(dl->id);
                    if (fb != m_failBytes.end() && got > fb->second) dl->autoRetries = 0;
                    m_failBytes[dl->id] = got;
                    int k = std::min(dl->autoRetries, 20);
                    UINT64 delay = std::min<UINT64>((UINT64)m_retryMaxMs, (UINT64)m_retryFirstMs << k);
                    dl->nextRetryTime = NowFt() + delay * 10000ULL;
                    ++dl->autoRetries;
                    wakeTimer = true;
                }
            }
            break;
        }
        dl->speed = 0;
        if (result != NDM_RUN_STOPPED && !m_shuttingDown) CheckAllDone(fire);
    }
    bool shutting = m_shuttingDown;
    LeaveCriticalSection(&m_cs);

    if (wakeTimer) WakeTimer();
    FireActions(fire);

    if (zombie)
    {
        // Removed while running: we are the last user of this object.
        if (dl->hThread)    { CloseHandle(dl->hThread);    dl->hThread = nullptr; }
        if (dl->hStopEvent) { CloseHandle(dl->hStopEvent); dl->hStopEvent = nullptr; }
        if (result != NDM_RUN_COMPLETED) {
            if (dl->kind == NDM_KIND_MEDIA) DeleteMediaTempFiles(dl);
            else NdmHttpEngine::DeleteTempFiles(dl);
        }
        delete dl;
    }

    if (!shutting)
    {
        AutoSave();
        ScheduleNext();
    }
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
NdmQueue& NdmQueue::GetInstance()
{
    static NdmQueue instance;
    return instance;
}

NdmQueue::NdmQueue()
{
    InitializeCriticalSection(&m_cs);
    InitializeCriticalSection(&m_saveCs);
    m_saveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_saveQuit  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_timerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_timerQuit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NdmQueue::~NdmQueue()
{
    Shutdown(8000);
    // The critical sections are intentionally not deleted: a thread that is
    // still stuck in WinINet at process exit may touch them.
}

// yt-dlp leftovers of a media item: NdmMediaDeleteTempFiles removes only the
// partial / intermediate names NdmMediaRun recorded (dl->mediaTemps), each
// re-validated as a yt-dlp partial name, never the finished file.

void NdmQueue::DestroyDownload(NdmDownload* dl, bool deleteTemp)
{
    if (deleteTemp) {
        if (dl->kind == NDM_KIND_MEDIA) DeleteMediaTempFiles(dl);
        else NdmHttpEngine::DeleteTempFiles(dl);
    }
    if (dl->hThread)    CloseHandle(dl->hThread);
    if (dl->hStopEvent) CloseHandle(dl->hStopEvent);
    delete dl;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void NdmQueue::Shutdown(DWORD timeoutMs)
{
    std::vector<HANDLE> waits;

    EnterCriticalSection(&m_cs);
    if (m_shutdownDone) { LeaveCriticalSection(&m_cs); return; }
    m_shuttingDown = true;
    LeaveCriticalSection(&m_cs);
    StopTimer();                         // no scheduler / auto-retry from now on

    EnterCriticalSection(&m_cs);
    std::set<NdmDownload*> all = m_running;
    for (NdmDownload* z : m_zombies) all.insert(z);
    for (NdmDownload* dl : all)
    {
        if (dl->hStopEvent) SetEvent(dl->hStopEvent);
        HANDLE dup = nullptr;
        if (dl->hThread &&
            DuplicateHandle(GetCurrentProcess(), dl->hThread, GetCurrentProcess(),
                            &dup, SYNCHRONIZE, FALSE, 0))
            waits.push_back(dup);
    }
    LeaveCriticalSection(&m_cs);

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (size_t i = 0; i < waits.size(); i += MAXIMUM_WAIT_OBJECTS)
    {
        DWORD n = (DWORD)std::min<size_t>(MAXIMUM_WAIT_OBJECTS, waits.size() - i);
        ULONGLONG now = GetTickCount64();
        DWORD left = now >= deadline ? 0 : (DWORD)(deadline - now);
        WaitForMultipleObjects(n, &waits[i], TRUE, left);
    }
    for (HANDLE h : waits) CloseHandle(h);

    // Persist final state (stopped downloads are saved as QUEUED so they
    // continue on next start; engine state files hold the byte progress):
    // stop the background saver, then one synchronous save.
    StopSaver();
    EnterCriticalSection(&m_cs);
    std::wstring finalPath = m_autoSavePath;
    LeaveCriticalSection(&m_cs);
    if (!finalPath.empty()) WriteQueue(finalPath);

    EnterCriticalSection(&m_cs);
    bool allDone = m_running.empty() && m_zombies.empty();
    if (allDone)
    {
        for (auto& kv : m_downloads)
            DestroyDownload(kv.second, false);
        m_downloads.clear();
        m_shutdownDone = true;
    }
    LeaveCriticalSection(&m_cs);
}

// ---------------------------------------------------------------------------
// AddDownload
// ---------------------------------------------------------------------------
DWORD NdmQueue::AddDownload(const std::wstring& url,
                             const std::wstring& savePath,
                             const std::wstring& filename,
                             int                 connections)
{
    NdmAddParams p;
    p.url         = url;
    p.savePath    = savePath;
    p.filename    = filename;
    p.connections = connections;
    p.startNow    = true;
    return AddDownloadEx(p);
}

// ---------------------------------------------------------------------------
// AddDownloadEx
// ---------------------------------------------------------------------------
DWORD NdmQueue::AddDownloadEx(const NdmAddParams& p)
{
    std::wstring url = p.url;
    while (!url.empty() && iswspace(url.back())) url.pop_back();
    size_t a = 0;
    while (a < url.size() && iswspace(url[a])) ++a;
    url = url.substr(a);
    if (url.empty()) return 0;

    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();

    NdmDownload* dl = new NdmDownload();
    dl->url       = url;
    dl->savePath  = !p.savePath.empty() ? p.savePath : cfg.GetSavePath();
    if (dl->savePath.empty()) dl->savePath = NdmConfigMgr::GetDefaultSavePath();
    int conns = p.connections > 0 ? p.connections : cfg.GetMaxConnections();
    dl->numConnections = ClampConns(conns);
    dl->referrer  = p.referrer;
    dl->cookies   = p.cookies;
    dl->userAgent = p.userAgent;
    dl->kind        = p.kind;
    dl->pageUrl     = p.pageUrl;
    dl->mediaFormat = p.mediaFormat;
    dl->torrentFiles = p.torrentFiles;
    dl->cookieJar   = p.kind == NDM_KIND_MEDIA ? p.cookieJar : std::wstring();
    dl->overwriteConfirmed = p.overwriteConfirmed;
    dl->status    = p.startNow ? NDM_STATUS_QUEUED : NDM_STATUS_PAUSED;
    dl->priority  = ClampPrio(p.priority);
    dl->speedLimit = p.speedLimit;
    dl->scheduled = p.scheduled;
    dl->addedTime = NowFt();

    bool userNamed = !p.filename.empty();
    dl->filename = userNamed ? NdmHttpEngine::SanitizeFilename(p.filename)
                             : NdmSafeFilename(url);
    NdmHttpEngine::PrepareSharedStrings(dl);

    EnterCriticalSection(&m_cs);
    if (m_shuttingDown)
    {
        LeaveCriticalSection(&m_cs);
        delete dl;
        return 0;
    }
    DWORD id = m_nextId++;
    dl->id = id;
    dl->queueOrder = m_nextOrder++;
    m_downloads[id] = dl;
    if (userNamed) m_userNamed.insert(id);
    LeaveCriticalSection(&m_cs);

    EnsureTimer();
    AutoSave();
    if (p.startNow) ScheduleNext();
    return id;
}

// ---------------------------------------------------------------------------
// RemoveDownload
// ---------------------------------------------------------------------------
void NdmQueue::RemoveDownload(DWORD id)
{
    EnterCriticalSection(&m_cs);

    auto it = m_downloads.find(id);
    if (it == m_downloads.end())
    {
        LeaveCriticalSection(&m_cs);
        return;
    }

    NdmDownload* dl = it->second;
    m_downloads.erase(it);
    m_userNamed.erase(id);
    m_failBytes.erase(id);

    bool running = m_running.count(dl) != 0;
    if (running)
    {
        // The worker thread owns the object from now on and frees it.
        m_zombies.insert(dl);
        if (dl->hStopEvent) SetEvent(dl->hStopEvent);
    }
    bool completed = dl->status == NDM_STATUS_COMPLETED;
    LeaveCriticalSection(&m_cs);

    if (!running)
        DestroyDownload(dl, !completed);

    AutoSave();
    ScheduleNext();
}

// ---------------------------------------------------------------------------
// PauseDownload
// ---------------------------------------------------------------------------
void NdmQueue::PauseDownload(DWORD id)
{
    bool changed = false;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    if (it != m_downloads.end())
    {
        NdmDownload* dl = it->second;
        if (dl->status == NDM_STATUS_DOWNLOADING || dl->status == NDM_STATUS_QUEUED)
        {
            if (m_running.count(dl) && dl->hStopEvent)
                SetEvent(dl->hStopEvent);   // engine stops and saves its state
            dl->status = NDM_STATUS_PAUSED;
            dl->speed  = 0;
            dl->nextRetryTime = 0;
            changed = true;
        }
        else if (dl->status == NDM_STATUS_ERROR && dl->nextRetryTime)
        {
            dl->status = NDM_STATUS_PAUSED;     // cancels the pending auto-retry
            dl->nextRetryTime = 0;
            changed = true;
        }
    }
    LeaveCriticalSection(&m_cs);

    if (changed)
    {
        AutoSave();
        ScheduleNext();
    }
}

// ---------------------------------------------------------------------------
// ResumeDownload
// ---------------------------------------------------------------------------
void NdmQueue::ResumeDownload(DWORD id)
{
    bool changed = false;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    if (it != m_downloads.end())
    {
        NdmDownload* dl = it->second;
        if (dl->status == NDM_STATUS_PAUSED ||
            dl->status == NDM_STATUS_ERROR)
        {
            dl->status = NDM_STATUS_QUEUED;
            NdmHttpEngine::SetSharedString(dl->errorMsg, L"");
            dl->errorKind = NDM_ERRKIND_NONE;
            dl->nextRetryTime = 0;
            dl->autoRetries = 0;
            m_failBytes.erase(id);
            changed = true;
        }
        else if (dl->status == NDM_STATUS_QUEUED)
            changed = true;
    }
    LeaveCriticalSection(&m_cs);

    if (changed)
    {
        AutoSave();
        ScheduleNext();
    }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
static void CopySnapshot(const NdmDownload* dl, NdmDownload& out)
{
    NdmHttpEngine::LockFields(false);
    out = *dl;
    NdmHttpEngine::UnlockFields(false);
    out.filename       = out.filename.c_str();   // drop the padding capacity
    out.errorMsg       = out.errorMsg.c_str();
    out.downloadedSize = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
    out.totalSize      = NdmHttpEngine::AtomicRead64(&dl->totalSize);
    out.hThread        = nullptr;
    out.hStopEvent     = nullptr;
}

bool NdmQueue::GetSnapshot(DWORD id, NdmDownload& out)
{
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    bool ok = it != m_downloads.end();
    if (ok) CopySnapshot(it->second, out);
    LeaveCriticalSection(&m_cs);
    return ok;
}

std::vector<NdmDownload> NdmQueue::GetAllSnapshots()
{
    std::vector<NdmDownload> v;
    EnterCriticalSection(&m_cs);
    v.resize(m_downloads.size());
    size_t i = 0;
    for (auto& kv : m_downloads) CopySnapshot(kv.second, v[i++]);
    LeaveCriticalSection(&m_cs);
    return v;
}

// ---------------------------------------------------------------------------
// ScheduleNext — start QUEUED downloads up to m_maxActive
// ---------------------------------------------------------------------------
void NdmQueue::ScheduleNext()
{
    EnterCriticalSection(&m_cs);
    if (m_shuttingDown) { LeaveCriticalSection(&m_cs); return; }

    // Apply the configured global speed limit (0 = unlimited) and retry policy.
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    NdmHttpEngine::SetSpeedLimit(cfg.GetSpeedLimit());
    NdmHttpEngine::SetRetryPolicy(cfg.GetRetryCount(), cfg.GetRetryMaxDelay());

    int maxActive = m_maxActive < 1 ? 1 : m_maxActive;

    int active = 0;
    std::vector<NdmDownload*> cand;
    for (auto& kv : m_downloads)
    {
        NdmDownload* dl = kv.second;
        if (dl->status == NDM_STATUS_DOWNLOADING) ++active;
        else if (dl->status == NDM_STATUS_QUEUED && !m_running.count(dl))   // (previous run still exiting)
            cand.push_back(dl);
    }
    // High before Normal before Low, then queue order.
    std::stable_sort(cand.begin(), cand.end(), StartsBefore);

    for (NdmDownload* dl : cand)
    {
        if (active >= maxActive) break;

        if (!dl->hStopEvent)
            dl->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        else
            ResetEvent(dl->hStopEvent);

        if (dl->hThread)
        {
            CloseHandle(dl->hThread);
            dl->hThread = nullptr;
        }

        dl->status = NDM_STATUS_DOWNLOADING;
        dl->errorKind = NDM_ERRKIND_NONE;
        dl->nextRetryTime = 0;
        m_running.insert(dl);
        dl->hThread = dl->hStopEvent
                    ? CreateThread(nullptr, 0, ThreadProc, dl, 0, nullptr)
                    : nullptr;
        if (!dl->hThread)
        {
            m_running.erase(dl);
            dl->status = NDM_STATUS_ERROR;
            dl->errorKind = NDM_ERRKIND_FATAL;
            NdmHttpEngine::SetSharedString(dl->errorMsg,
                L"Cannot start download thread: " +
                NdmHttpEngine::ErrorText(GetLastError()));
        }
        else
            ++active;
    }

    LeaveCriticalSection(&m_cs);
}

// ---------------------------------------------------------------------------
// SaveQueue  (UTF-8 XML, written atomically via temp file + rename)
//
// AutoSave only marks the queue dirty and wakes the saver thread, which waits
// ~1 s to coalesce bursts (adding thousands of links = one write) and then
// writes. The XML is built from snapshots taken under m_cs; DPAPI
// encryption of cookies and the file write happen outside m_cs, and each
// item's encrypted blob is cached (cookiesEnc / cookieJarEnc), so an
// unchanged item is never encrypted twice.
// ---------------------------------------------------------------------------
void NdmQueue::AutoSave()
{
    EnterCriticalSection(&m_cs);
    bool have = !m_autoSavePath.empty();
    bool stopped = m_saverStopped;
    if (have && !stopped && !m_saveThread && m_saveEvent && m_saveQuit)
        m_saveThread = CreateThread(nullptr, 0, SaveThreadProc, this, 0, nullptr);
    HANDLE thr = m_saveThread;
    LeaveCriticalSection(&m_cs);
    if (!have) return;
    InterlockedExchange(&m_dirty, 1);
    if (thr) SetEvent(m_saveEvent);
    else if (!stopped)
    {
        // no saver thread (creation failed): save synchronously
        EnterCriticalSection(&m_cs);
        std::wstring p = m_autoSavePath;
        LeaveCriticalSection(&m_cs);
        if (InterlockedExchange(&m_dirty, 0)) WriteQueue(p);
    }
}

DWORD WINAPI NdmQueue::SaveThreadProc(LPVOID param)
{
    NdmQueue* q = reinterpret_cast<NdmQueue*>(param);
    HANDLE w[2] = { q->m_saveQuit, q->m_saveEvent };
    for (;;)
    {
        DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 + 1) break;                    // quit (or failure)
        // Debounce: further changes within this second join this save.
        if (WaitForSingleObject(q->m_saveQuit, 1000) == WAIT_OBJECT_0) break;
        if (!InterlockedExchange(&q->m_dirty, 0)) continue;
        EnterCriticalSection(&q->m_cs);
        std::wstring p = q->m_autoSavePath;
        bool done = q->m_shutdownDone;
        LeaveCriticalSection(&q->m_cs);
        if (!p.empty() && !done) q->WriteQueue(p);
    }
    return 0;
}

void NdmQueue::StopSaver()
{
    EnterCriticalSection(&m_cs);
    m_saverStopped = true;
    HANDLE thr = m_saveThread;
    m_saveThread = nullptr;
    LeaveCriticalSection(&m_cs);
    if (m_saveQuit) SetEvent(m_saveQuit);
    if (thr)
    {
        WaitForSingleObject(thr, 15000);
        CloseHandle(thr);
    }
}

void NdmQueue::SaveQueue(const std::wstring& path)
{
    if (path.empty()) return;
    EnterCriticalSection(&m_cs);
    bool done = m_shutdownDone;
    if (!done) m_autoSavePath = path;
    LeaveCriticalSection(&m_cs);
    if (done) return;                    // final state already on disk; items are gone
    InterlockedExchange(&m_dirty, 0);
    WriteQueue(path);
}

namespace {
struct SaveRec
{
    NdmDownload* ptr = nullptr;
    DWORD        id = 0;
    int          status = 0;
    bool         completed = false;
    UINT64       downloaded = 0, total = 0;
    int          conns = 0;
    bool         userNamed = false, overwrite = false;
    int          kind = 0;
    DWORD        limit = 0;
    int          prio = NDM_PRIO_NORMAL, errKind = 0;
    UINT64       added = 0, completedAt = 0;
    INT64        order = 0;
    bool         sched = false;
    std::wstring url, file, path, referrer, ua, page, format, tfiles, error, mtemps, btih;
    // cookies: cached blob, or the plaintext to encrypt (then wiped)
    std::wstring ckEnc, jarEnc, ckPlain, jarPlain;
};

void Wipe(std::wstring& s)
{
    if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    s.clear();
}
} // namespace

void NdmQueue::WriteQueue(const std::wstring& path)
{
    if (path.empty()) return;

    // 1. Snapshot under the lock (no encryption, no I/O here).
    std::vector<SaveRec> recs;
    EnterCriticalSection(&m_cs);
    recs.reserve(m_downloads.size());
    NdmHttpEngine::LockFields(false);
    for (auto& kv : m_downloads)
    {
        NdmDownload* dl = kv.second;
        recs.emplace_back();
        SaveRec& r = recs.back();
        r.ptr        = dl;
        r.id         = dl->id;
        r.status     = (int)dl->status;
        r.completed  = dl->status == NDM_STATUS_COMPLETED;
        if (r.status == NDM_STATUS_DOWNLOADING) r.status = NDM_STATUS_QUEUED;   // resume on restart
        r.downloaded = NdmHttpEngine::AtomicRead64(&dl->downloadedSize);
        r.total      = NdmHttpEngine::AtomicRead64(&dl->totalSize);
        r.conns      = dl->numConnections;
        r.userNamed  = m_userNamed.count(dl->id) != 0;
        r.overwrite  = dl->overwriteConfirmed;
        r.kind       = dl->kind;
        r.url        = dl->url;
        r.file       = dl->filename.c_str();
        r.path       = dl->savePath;
        r.referrer   = dl->referrer;
        r.ua         = dl->userAgent;
        r.page       = dl->pageUrl;
        r.format     = dl->mediaFormat;
        r.tfiles     = dl->torrentFiles;
        r.mtemps     = r.completed ? std::wstring() : dl->mediaTemps;
        r.limit      = dl->speedLimit;
        r.prio       = dl->priority;
        r.added      = dl->addedTime;
        r.completedAt = dl->completedTime;
        r.order      = dl->queueOrder;
        r.sched      = dl->scheduled;
        r.btih       = dl->torrentHash;
        if (dl->status == NDM_STATUS_ERROR) { r.error = dl->errorMsg.c_str(); r.errKind = dl->errorKind; }
        if (!r.completed)
        {
            // Cookies are secrets: DPAPI (current user) + base64, never plaintext.
            if (!dl->cookies.empty())
            {
                if (!dl->cookiesEnc.empty()) r.ckEnc = dl->cookiesEnc;
                else                          r.ckPlain = dl->cookies;
            }
            if (!dl->cookieJar.empty())
            {
                if (!dl->cookieJarEnc.empty()) r.jarEnc = dl->cookieJarEnc;
                else                            r.jarPlain = dl->cookieJar;
            }
        }
    }
    NdmHttpEngine::UnlockFields(false);
    LeaveCriticalSection(&m_cs);

    // 2. Encrypt what is not cached yet (outside m_cs), then cache the blobs.
    bool anyNew = false;
    for (SaveRec& r : recs)
    {
        if (!r.ckPlain.empty())  { r.ckEnc  = NdmProtectString(r.ckPlain);  anyNew = true; }
        if (!r.jarPlain.empty()) { r.jarEnc = NdmProtectString(r.jarPlain); anyNew = true; }
    }
    if (anyNew)
    {
        EnterCriticalSection(&m_cs);
        for (SaveRec& r : recs)
        {
            if (r.ckPlain.empty() && r.jarPlain.empty()) continue;
            auto it = m_downloads.find(r.id);
            if (it == m_downloads.end() || it->second != r.ptr) continue;   // removed meanwhile
            NdmDownload* dl = it->second;
            NdmHttpEngine::LockFields(true);
            if (!r.ckPlain.empty() && !r.ckEnc.empty() && dl->cookies == r.ckPlain)
                dl->cookiesEnc = r.ckEnc;
            if (!r.jarPlain.empty() && !r.jarEnc.empty() && dl->cookieJar == r.jarPlain)
                dl->cookieJarEnc = r.jarEnc;
            NdmHttpEngine::UnlockFields(true);
        }
        LeaveCriticalSection(&m_cs);
        for (SaveRec& r : recs) { Wipe(r.ckPlain); Wipe(r.jarPlain); }
    }

    // 3. XML (outside m_cs).
    std::wstring xml;
    xml.reserve(recs.size() * 512 + 128);
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    xml += L"<NDMQueue version=\"2\">\r\n";
    for (const SaveRec& r : recs)
    {
        xml += L"  <Download id=\"" + std::to_wstring(r.id) + L"\"";
        xml += L" url=\""        + XmlEscape(r.url) + L"\"";
        xml += L" file=\""       + XmlEscape(r.file) + L"\"";
        xml += L" path=\""       + XmlEscape(r.path) + L"\"";
        xml += L" status=\""     + std::to_wstring(r.status) + L"\"";
        xml += L" downloaded=\"" + std::to_wstring(r.downloaded) + L"\"";
        xml += L" total=\""      + std::to_wstring(r.total) + L"\"";
        xml += L" connections=\"" + std::to_wstring(r.conns) + L"\"";
        xml += L" usernamed=\""  + std::wstring(r.userNamed ? L"1" : L"0") + L"\"";
        if (!r.referrer.empty()) xml += L" referrer=\""  + XmlEscape(r.referrer)  + L"\"";
        if (!r.ckEnc.empty())    xml += L" cookiesx=\""  + XmlEscape(r.ckEnc) + L"\"";
        if (!r.jarEnc.empty())   xml += L" cookiejarx=\"" + XmlEscape(r.jarEnc) + L"\"";
        if (r.overwrite)         xml += L" overwrite=\"1\"";
        if (!r.ua.empty())       xml += L" useragent=\"" + XmlEscape(r.ua) + L"\"";
        if (r.kind != NDM_KIND_HTTP) xml += L" kind=\"" + std::to_wstring(r.kind) + L"\"";
        if (!r.page.empty())     xml += L" page=\""   + XmlEscape(r.page) + L"\"";
        if (!r.format.empty())   xml += L" format=\"" + XmlEscape(r.format) + L"\"";
        if (!r.tfiles.empty())   xml += L" tfiles=\"" + XmlEscape(r.tfiles) + L"\"";
        if (!r.mtemps.empty())   xml += L" mtemps=\"" + XmlEscape(r.mtemps) + L"\"";
        if (!r.error.empty())    xml += L" error=\"" + XmlEscape(r.error) + L"\"";
        if (r.errKind)           xml += L" errkind=\"" + std::to_wstring(r.errKind) + L"\"";
        if (r.limit)             xml += L" limit=\"" + std::to_wstring(r.limit) + L"\"";
        if (r.prio != NDM_PRIO_NORMAL) xml += L" prio=\"" + std::to_wstring(r.prio) + L"\"";
        if (r.added)             xml += L" added=\"" + std::to_wstring(r.added) + L"\"";
        if (r.completedAt)       xml += L" completed=\"" + std::to_wstring(r.completedAt) + L"\"";
        xml += L" order=\"" + std::to_wstring(r.order) + L"\"";
        if (r.sched)             xml += L" sched=\"1\"";
        if (!r.btih.empty())     xml += L" btih=\"" + XmlEscape(r.btih) + L"\"";
        xml += L"/>\r\n";
    }
    xml += L"</NDMQueue>\r\n";
    recs.clear();
    std::string data = ToUtf8(xml);

    EnterCriticalSection(&m_saveCs);
    EnsureDirQ(DirOf(path));
    // The list on disk could not be read at start (antivirus / backup tool
    // held it): copy it aside first, never overwrite what was not loaded.
    if (m_keepUnread)
    {
        if (CopyFileW(path.c_str(), (path + L".unread").c_str(), FALSE) ||
            GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            m_keepUnread = false;
        else
        {
            LeaveCriticalSection(&m_saveCs);
            InterlockedExchange(&m_dirty, 1);          // try again on the next save
            return;
        }
    }
    std::wstring tmp = path + L".tmp";
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
        // On disk before the swap: a power cut must never leave an empty list.
        if (ok && written == data.size()) ok = FlushFileBuffers(hFile);
        CloseHandle(hFile);
        if (ok && written == data.size())
        {
            // Swap in the new list; the previous one stays as queue.xml.bak.
            if (!ReplaceFileW(path.c_str(), tmp.c_str(), (path + L".bak").c_str(),
                              REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
                MoveFileExW(tmp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        else
            DeleteFileW(tmp.c_str());
    }
    LeaveCriticalSection(&m_saveCs);
}

// ---------------------------------------------------------------------------
// Reads a queue file into text. 0 = ok, else a Win32 error (ERROR_FILE_NOT_FOUND
// when absent, ERROR_INVALID_DATA when empty, cut off or not an NDM queue).
// ---------------------------------------------------------------------------
static DWORD ReadQueueText(const std::wstring& path, std::wstring& xml)
{
    xml.clear();
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD e = 0;
    // A scanner or backup tool may hold the file for a moment.
    for (int i = 0; i < 15; ++i)
    {
        hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) break;
        e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION && e != ERROR_ACCESS_DENIED) return e;
        Sleep(200);
    }
    if (hFile == INVALID_HANDLE_VALUE) return e ? e : ERROR_SHARING_VIOLATION;

    LARGE_INTEGER fsz = {};
    if (!GetFileSizeEx(hFile, &fsz) || fsz.QuadPart < 2 || fsz.QuadPart > 64LL * 1024 * 1024)
    {
        CloseHandle(hFile);
        return ERROR_INVALID_DATA;
    }
    std::vector<BYTE> raw((size_t)fsz.QuadPart + 2, 0);
    DWORD rd = 0;
    BOOL okRead = ReadFile(hFile, raw.data(), (DWORD)fsz.QuadPart, &rd, nullptr);
    e = okRead ? 0 : GetLastError();
    CloseHandle(hFile);
    if (!okRead) return e ? e : ERROR_READ_FAULT;
    if (rd < 2) return ERROR_INVALID_DATA;

    if (raw[0] == 0xFF && raw[1] == 0xFE)
    {
        xml.assign(reinterpret_cast<wchar_t*>(raw.data() + 2), (rd - 2) / sizeof(wchar_t));
    }
    else
    {
        size_t off = (rd >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        const char* src = reinterpret_cast<const char*>(raw.data() + off);
        int srcLen = (int)(rd - off);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, src, srcLen, nullptr, 0);
        if (wlen > 0)
        {
            xml.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, src, srcLen, &xml[0], wlen);
        }
    }
    // A complete list ends with its closing tag (a cut-off file does not).
    if (xml.find(L"<NDMQueue") == std::wstring::npos || xml.find(L"</NDMQueue>") == std::wstring::npos)
    {
        xml.clear();
        return ERROR_INVALID_DATA;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// LoadQueue — parse the XML written by SaveQueue (v1 UTF-16 files too)
// ---------------------------------------------------------------------------
void NdmQueue::LoadQueue(const std::wstring& path)
{
    EnterCriticalSection(&m_cs);
    m_autoSavePath = path;
    LeaveCriticalSection(&m_cs);

    // queue.xml, else the previous good list (.bak), else a temp file a crash
    // left behind. A damaged queue.xml is kept as .corrupt; one that could
    // not be read at all is copied aside before the first save (m_keepUnread).
    std::wstring xml;
    DWORD eMain = ReadQueueText(path, xml);
    if (eMain)
    {
        if (eMain == ERROR_INVALID_DATA)
            CopyFileW(path.c_str(), (path + L".corrupt").c_str(), FALSE);
        else if (eMain != ERROR_FILE_NOT_FOUND && eMain != ERROR_PATH_NOT_FOUND)
        {
            EnterCriticalSection(&m_saveCs);
            m_keepUnread = true;
            LeaveCriticalSection(&m_saveCs);
        }
        if (ReadQueueText(path + L".bak", xml) && ReadQueueText(path + L".tmp", xml))
        {
            EnsureTimer();
            ScheduleNext();
            return;
        }
    }

    // Parse each <Download .../> tag
    const std::wstring tag = L"<Download ";
    size_t pos = 0;
    bool added = false;
    while ((pos = xml.find(tag, pos)) != std::wstring::npos)
    {
        size_t end = xml.find(L'>', pos);
        if (end == std::wstring::npos) break;
        std::wstring elem = xml.substr(pos, end - pos + 1);
        pos = end + 1;

        std::wstring url = GetAttr(elem, L"url");
        if (url.empty()) continue;

        DWORD   id      = (DWORD)_wcstoui64(GetAttr(elem, L"id").c_str(), nullptr, 10);
        int     status  = (int)_wtol(GetAttr(elem, L"status").c_str());
        UINT64  dl_sz   = _wcstoui64(GetAttr(elem, L"downloaded").c_str(), nullptr, 10);
        UINT64  tot_sz  = _wcstoui64(GetAttr(elem, L"total").c_str(), nullptr, 10);
        int     conns   = (int)_wtol(GetAttr(elem, L"connections").c_str());
        bool    hasUN   = false;
        std::wstring un = GetAttr(elem, L"usernamed", &hasUN);

        NdmDownload* dl = new NdmDownload();
        dl->url            = url;
        dl->filename       = NdmHttpEngine::SanitizeFilename(GetAttr(elem, L"file"));
        dl->savePath       = GetAttr(elem, L"path");
        dl->referrer       = GetAttr(elem, L"referrer");
        {
            bool hasX = false;
            std::wstring enc = GetAttr(elem, L"cookiesx", &hasX);
            if (!hasX || !NdmUnprotectString(enc, dl->cookies))
                dl->cookies = hasX ? std::wstring() : GetAttr(elem, L"cookies");   // legacy plaintext (read once)
            else if (!dl->cookies.empty())
                dl->cookiesEnc = enc;                    // valid blob: reuse, no re-encryption
            enc = GetAttr(elem, L"cookiejarx", &hasX);
            if (hasX && !NdmUnprotectString(enc, dl->cookieJar)) dl->cookieJar.clear();
            else if (hasX && !dl->cookieJar.empty()) dl->cookieJarEnc = enc;
        }
        dl->mediaTemps = GetAttr(elem, L"mtemps");
        if (dl->mediaTemps.size() > 16384) dl->mediaTemps.clear();
        dl->overwriteConfirmed = GetAttr(elem, L"overwrite") == L"1";
        dl->userAgent      = GetAttr(elem, L"useragent");
        dl->kind           = (int)_wtol(GetAttr(elem, L"kind").c_str());
        dl->pageUrl        = GetAttr(elem, L"page");
        dl->mediaFormat    = GetAttr(elem, L"format");
        dl->torrentFiles   = GetAttr(elem, L"tfiles");
        dl->errorMsg       = GetAttr(elem, L"error");
        dl->downloadedSize = dl_sz;
        dl->totalSize      = tot_sz;
        dl->numConnections = ClampConns(conns > 0 ? conns : 8);
        if (dl->savePath.empty()) dl->savePath = NdmConfigMgr::GetInstance().GetSavePath();

        switch (status)
        {
        case NDM_STATUS_COMPLETED: dl->status = NDM_STATUS_COMPLETED; break;
        case NDM_STATUS_PAUSED:    dl->status = NDM_STATUS_PAUSED;    break;
        case NDM_STATUS_ERROR:     dl->status = NDM_STATUS_ERROR;     break;
        default:                   dl->status = NDM_STATUS_QUEUED;    break; // queued / was downloading
        }
        dl->speedLimit  = (DWORD)wcstoul(GetAttr(elem, L"limit").c_str(), nullptr, 10);
        {
            bool hasP = false;
            std::wstring pr = GetAttr(elem, L"prio", &hasP);
            dl->priority = hasP ? ClampPrio(_wtoi(pr.c_str())) : NDM_PRIO_NORMAL;
        }
        dl->addedTime     = _wcstoui64(GetAttr(elem, L"added").c_str(), nullptr, 10);
        dl->completedTime = _wcstoui64(GetAttr(elem, L"completed").c_str(), nullptr, 10);
        {
            bool hasO = false;
            std::wstring o = GetAttr(elem, L"order", &hasO);
            dl->queueOrder = hasO ? _wcstoi64(o.c_str(), nullptr, 10) : (INT64)id;   // old lists: FIFO by id
        }
        dl->scheduled   = GetAttr(elem, L"sched") == L"1";
        dl->torrentHash = GetAttr(elem, L"btih");
        if (dl->torrentHash.size() != 40) dl->torrentHash.clear();
        if (dl->status == NDM_STATUS_ERROR)
        {
            int ek = _wtoi(GetAttr(elem, L"errkind").c_str());
            dl->errorKind = (ek == NDM_ERRKIND_TRANSIENT) ? NDM_ERRKIND_TRANSIENT : NDM_ERRKIND_FATAL;
            // a network failure before the last exit: retry it after the first delay
            if (dl->errorKind == NDM_ERRKIND_TRANSIENT && dl->kind == NDM_KIND_HTTP)
                dl->nextRetryTime = NowFt() + (UINT64)m_retryFirstMs * 10000ULL;
        }
        if (dl->status == NDM_STATUS_COMPLETED && !dl->completedTime)
        {
            // Lists from before dates were stored: the finished file's
            // last-write time is the real completion time. No file -> unknown.
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            std::wstring fp = dl->savePath;
            if (!fp.empty() && fp.back() != L'\\' && fp.back() != L'/') fp += L'\\';
            fp += dl->filename;
            if (!dl->filename.empty() && GetFileAttributesExW(fp.c_str(), GetFileExInfoStandard, &fa))
                dl->completedTime = FtToU64(fa.ftLastWriteTime);
        }
        if (dl->status == NDM_STATUS_COMPLETED && tot_sz) dl->downloadedSize = tot_sz;
        if (dl->status == NDM_STATUS_COMPLETED) { dl->cookies.clear(); dl->cookieJar.clear(); dl->cookiesEnc.clear(); dl->cookieJarEnc.clear(); dl->mediaTemps.clear(); }
        if (dl->kind != NDM_KIND_MEDIA) { dl->cookieJar.clear(); dl->cookieJarEnc.clear(); dl->mediaTemps.clear(); }
        NdmHttpEngine::PrepareSharedStrings(dl);

        EnterCriticalSection(&m_cs);
        if (id == 0 || m_downloads.count(id)) id = m_nextId;
        dl->id = id;
        if (id >= m_nextId) m_nextId = id + 1;
        if (dl->queueOrder >= m_nextOrder) m_nextOrder = dl->queueOrder + 1;
        m_downloads[id] = dl;
        if (hasUN && un == L"1") m_userNamed.insert(id);
        LeaveCriticalSection(&m_cs);
        added = true;
    }

    (void)added;
    EnsureTimer();
    ScheduleNext();
}

// ---------------------------------------------------------------------------
// Max active downloads
// ---------------------------------------------------------------------------
void NdmQueue::SetMaxActiveDownloads(int n)
{
    m_maxActive = n < 1 ? 1 : n;
    ScheduleNext();                      // a higher limit starts queued items now
}

// ---------------------------------------------------------------------------
// Per-download speed limit / priority
// ---------------------------------------------------------------------------
bool NdmQueue::SetDownloadSpeedLimit(DWORD id, DWORD bytesPerSec)
{
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    bool ok = it != m_downloads.end();
    // Atomic store: the engines read it while running (allocator, ~1 s).
    if (ok) InterlockedExchange(reinterpret_cast<volatile LONG*>(&it->second->speedLimit), (LONG)bytesPerSec);
    LeaveCriticalSection(&m_cs);
    if (ok) AutoSave();
    return ok;
}

bool NdmQueue::SetPriority(DWORD id, int priority)
{
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    bool ok = it != m_downloads.end();
    if (ok) InterlockedExchange(reinterpret_cast<volatile LONG*>(&it->second->priority), ClampPrio(priority));
    LeaveCriticalSection(&m_cs);
    if (ok) { AutoSave(); ScheduleNext(); }
    return ok;
}

// ---------------------------------------------------------------------------
// Queue order
// ---------------------------------------------------------------------------
bool NdmQueue::Move(DWORD id, int how)
{
    EnterCriticalSection(&m_cs);
    std::vector<NdmDownload*> v;
    v.reserve(m_downloads.size());
    for (auto& kv : m_downloads) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), OrderBefore);
    size_t i = 0;
    while (i < v.size() && v[i]->id != id) ++i;
    bool ok = i < v.size();
    if (ok)
    {
        NdmDownload* d = v[i];
        switch (how)
        {
        case 0:  // up
            if (i == 0) { ok = false; break; }
            std::swap(d->queueOrder, v[i - 1]->queueOrder);
            if (d->queueOrder == v[i - 1]->queueOrder) --d->queueOrder;
            break;
        case 1:  // down
            if (i + 1 >= v.size()) { ok = false; break; }
            std::swap(d->queueOrder, v[i + 1]->queueOrder);
            if (d->queueOrder == v[i + 1]->queueOrder) ++d->queueOrder;
            break;
        case 2:  // top
            if (i == 0) { ok = false; break; }
            d->queueOrder = v[0]->queueOrder - 1;
            break;
        default: // bottom
            if (i + 1 >= v.size()) { ok = false; break; }
            d->queueOrder = std::max(m_nextOrder, v.back()->queueOrder + 1);
            m_nextOrder = d->queueOrder + 1;
            break;
        }
    }
    LeaveCriticalSection(&m_cs);
    if (ok) AutoSave();
    return ok;
}

bool NdmQueue::MoveUp(DWORD id)       { return Move(id, 0); }
bool NdmQueue::MoveDown(DWORD id)     { return Move(id, 1); }
bool NdmQueue::MoveToTop(DWORD id)    { return Move(id, 2); }
bool NdmQueue::MoveToBottom(DWORD id) { return Move(id, 3); }

std::vector<DWORD> NdmQueue::GetQueueOrder()
{
    EnterCriticalSection(&m_cs);
    std::vector<NdmDownload*> v;
    v.reserve(m_downloads.size());
    for (auto& kv : m_downloads) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), OrderBefore);
    std::vector<DWORD> ids;
    ids.reserve(v.size());
    for (NdmDownload* d : v) ids.push_back(d->id);
    LeaveCriticalSection(&m_cs);
    return ids;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
std::vector<DWORD> NdmQueue::FindByUrl(const std::wstring& url)
{
    std::vector<DWORD> ids;
    std::wstring key = UrlKey(url);
    if (key.empty()) return ids;
    EnterCriticalSection(&m_cs);
    NdmHttpEngine::LockFields(false);
    for (auto& kv : m_downloads)
    {
        const NdmDownload* d = kv.second;
        if (UrlKey(d->url) == key || (!d->pageUrl.empty() && UrlKey(d->pageUrl) == key))
            ids.push_back(d->id);
    }
    NdmHttpEngine::UnlockFields(false);
    LeaveCriticalSection(&m_cs);
    return ids;
}

DWORD NdmQueue::FindRefreshCandidate(const std::wstring& url, const std::wstring& fileName, UINT64 fileSize)
{
    (void)url;           // the new address may be anything; name + size identify the file
    if (fileName.empty() || fileSize == 0) return 0;
    std::wstring clean = NdmHttpEngine::SanitizeFilename(fileName);
    DWORD found = 0;
    INT64 bestOrder = 0;
    EnterCriticalSection(&m_cs);
    NdmHttpEngine::LockFields(false);
    for (auto& kv : m_downloads)
    {
        NdmDownload* d = kv.second;
        if (d->kind != NDM_KIND_HTTP) continue;
        if (d->status != NDM_STATUS_PAUSED && d->status != NDM_STATUS_ERROR) continue;
        if (m_running.count(d)) continue;
        if (NdmHttpEngine::AtomicRead64(&d->totalSize) != fileSize) continue;
        const wchar_t* fn = d->filename.c_str();
        if (_wcsicmp(fn, clean.c_str()) != 0 && _wcsicmp(fn, fileName.c_str()) != 0) continue;
        if (!found || d->queueOrder < bestOrder) { found = d->id; bestOrder = d->queueOrder; }
    }
    NdmHttpEngine::UnlockFields(false);
    LeaveCriticalSection(&m_cs);
    return found;
}

// ---------------------------------------------------------------------------
// Rename
// ---------------------------------------------------------------------------
bool NdmQueue::RenameDownload(DWORD id, const std::wstring& newNameIn, std::wstring& msg)
{
    msg.clear();
    const std::wstring want = TrimW(newNameIn);
    const std::wstring newName = NdmHttpEngine::SanitizeFilename(want);
    if (want.empty() || newName != want)
    {
        msg = L"That name cannot be used for a file.";
        return false;
    }
    auto join = [](const std::wstring& dir, const std::wstring& f)
    {
        std::wstring p = dir;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        return p + f;
    };
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };

    bool ok = false;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    if (it == m_downloads.end())
        msg = L"The download no longer exists.";
    else
    {
        NdmDownload* dl = it->second;
        const std::wstring oldName = NdmHttpEngine::GetSharedString(dl->filename);
        const std::wstring oldPath = join(dl->savePath, oldName), newPath = join(dl->savePath, newName);
        const bool caseOnly = _wcsicmp(oldName.c_str(), newName.c_str()) == 0;
        if (oldName == newName)
            ok = true;
        else if (dl->kind == NDM_KIND_TORRENT)
            msg = L"Torrent downloads cannot be renamed.";
        else if (m_running.count(dl) || dl->status == NDM_STATUS_DOWNLOADING)
            msg = L"Pause the download first.";
        else if (dl->status == NDM_STATUS_COMPLETED)
        {
            if (!caseOnly && exists(newPath))
                msg = L"A file named “" + newName + L"” is already in that folder.";
            else if (!MoveFileExW(oldPath.c_str(), newPath.c_str(), 0))
                msg = L"The file could not be renamed: " + NdmHttpEngine::ErrorText(GetLastError());
            else
                ok = true;
        }
        else if (dl->kind != NDM_KIND_HTTP)
            msg = L"This download can be renamed when it has finished.";
        else if (NdmHttpEngine::AtomicRead64(&dl->downloadedSize) > 0 ||
                 exists(oldPath + L".ndmpart") || exists(oldPath + L".ndmstate"))
            msg = L"Part of this file is already downloaded. Rename it when it has finished.";
        else if (!caseOnly && exists(newPath))
            msg = L"A file named “" + newName + L"” is already in that folder.";
        else
            ok = true;
        if (ok && oldName != newName)
        {
            NdmHttpEngine::SetSharedString(dl->filename, newName);
            m_userNamed.insert(id);
        }
    }
    LeaveCriticalSection(&m_cs);
    if (ok) AutoSave();
    return ok;
}

bool NdmQueue::RelocateCompleted(DWORD id, const std::wstring& newFullPath)
{
    const size_t k = newFullPath.find_last_of(L"\\/");
    if (k == std::wstring::npos || k == 0 || k + 1 >= newFullPath.size()) return false;
    if (GetFileAttributesW(newFullPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring dir = newFullPath.substr(0, k), name = newFullPath.substr(k + 1);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';          // "D:" -> "D:\"
    bool ok = false;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    if (it != m_downloads.end())
    {
        NdmDownload* dl = it->second;
        if (dl->status == NDM_STATUS_COMPLETED && !m_running.count(dl))
        {
            if (_wcsicmp(NdmHttpEngine::GetSharedString(dl->filename).c_str(), name.c_str()) != 0)
            {
                NdmHttpEngine::SetSharedString(dl->filename, name);
                m_userNamed.insert(id);
            }
            dl->savePath = dir;
            ok = true;
        }
    }
    LeaveCriticalSection(&m_cs);
    if (ok) AutoSave();
    return ok;
}

// ---------------------------------------------------------------------------
// Refresh download address
// ---------------------------------------------------------------------------
NdmRefreshResult NdmQueue::RefreshAddress(DWORD id, const std::wstring& newUrlIn,
                                          const std::wstring& newReferrer, const std::wstring& newCookies,
                                          std::wstring& msg)
{
    msg.clear();
    std::wstring newUrl = TrimW(newUrlIn);
    if (!NdmIsHttpUrl(newUrl))
    {
        msg = L"The new address is not an http:// or https:// link.";
        return NDM_REFRESH_NOT_HTTP;
    }

    // 1. what we have (copy under the lock; no I/O here)
    NdmDownload snap;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    if (it == m_downloads.end())
    {
        LeaveCriticalSection(&m_cs);
        msg = L"The download no longer exists.";
        return NDM_REFRESH_NOT_FOUND;
    }
    NdmDownload* dl = it->second;
    if (dl->kind != NDM_KIND_HTTP)
    {
        LeaveCriticalSection(&m_cs);
        msg = L"Only ordinary (HTTP) downloads can get a new address.";
        return NDM_REFRESH_NOT_HTTP;
    }
    if (dl->status == NDM_STATUS_DOWNLOADING || dl->status == NDM_STATUS_COMPLETED || m_running.count(dl))
    {
        bool done = dl->status == NDM_STATUS_COMPLETED;
        LeaveCriticalSection(&m_cs);
        msg = done ? L"The download is already complete." : L"Pause the download first.";
        return NDM_REFRESH_BUSY;
    }
    CopySnapshot(dl, snap);
    LeaveCriticalSection(&m_cs);

    // Browser cookies belong to their site: the old ones are never sent to
    // (or kept for) a different origin.
    const bool sameOrigin = OriginOf(newUrl) == OriginOf(snap.url);
    std::wstring cookies  = !newCookies.empty() ? newCookies : (sameOrigin ? snap.cookies : std::wstring());
    std::wstring referrer = !newReferrer.empty() ? newReferrer : snap.referrer;

    // 2. the file the existing part belongs to, and the new server's file
    UINT64 stTotal = 0, onDisk = 0;
    std::wstring stEtag, stLm;
    bool haveState = NdmHttpEngine::ReadStateInfo(&snap, stTotal, stEtag, stLm, onDisk);
    NdmHttpEngine::ProbeResult pr;
    bool ok = NdmHttpEngine::ProbeUrl(newUrl, referrer, cookies, snap.userAgent, pr);
    if (!cookies.empty()) SecureZeroMemory(&cookies[0], cookies.size() * sizeof(wchar_t));
    if (!snap.cookies.empty()) SecureZeroMemory(&snap.cookies[0], snap.cookies.size() * sizeof(wchar_t));
    if (!ok)
    {
        if (pr.netError)
        {
            msg = L"Cannot reach the server: " + pr.message;
            return NDM_REFRESH_NETWORK_ERROR;
        }
        msg = L"The server refused the new address: " +
              (pr.message.empty() ? L"HTTP " + std::to_wstring(pr.status) : pr.message);
        return NDM_REFRESH_HTTP_ERROR;
    }
    UINT64 known = haveState ? stTotal : snap.totalSize;
    if (known)
    {
        if (!pr.sizeKnown)
        {
            msg = L"The server does not report the file size, so it cannot be confirmed to be the same file.";
            return NDM_REFRESH_SIZE_MISMATCH;
        }
        if (pr.total != known)
        {
            msg = L"The new address serves a file of " + NdmFormatSize(pr.total) +
                  L", this download is " + NdmFormatSize(known) + L".";
            return NDM_REFRESH_SIZE_MISMATCH;
        }
    }
    if (haveState && !NdmHttpEngine::SameRemoteFile(stTotal, stEtag, stLm, pr.total, pr.etag, pr.lastMod))
    {
        msg = L"The new address serves a different version of the file (Last-Modified / ETag differ).";
        return NDM_REFRESH_DIFFERENT_FILE;
    }
    if (haveState && onDisk > 0 && !pr.rangesOk)
    {
        msg = L"The new address does not accept resume requests, so the " + NdmFormatSize(onDisk) +
              L" already downloaded could not be continued.";
        return NDM_REFRESH_NO_RANGES;
    }

    // 3. apply: .ndmstate first (still bound to the old url), then the item
    NdmRefreshResult res = NDM_REFRESH_OK;
    EnterCriticalSection(&m_cs);
    it = m_downloads.find(id);
    if (it == m_downloads.end() || it->second != dl)
    {
        res = NDM_REFRESH_NOT_FOUND;
        msg = L"The download no longer exists.";
    }
    else if (dl->status == NDM_STATUS_DOWNLOADING || dl->status == NDM_STATUS_COMPLETED || m_running.count(dl))
    {
        res = NDM_REFRESH_BUSY;
        msg = L"The download was started meanwhile. Pause it first.";
    }
    else if (!NdmHttpEngine::RebindState(dl, newUrl))
    {
        res = NDM_REFRESH_SAVE_FAILED;
        msg = L"Cannot update the download's state file.";
    }
    else
    {
        NdmHttpEngine::LockFields(true);
        dl->url = newUrl;
        if (!newReferrer.empty()) dl->referrer = newReferrer;
        if (!newCookies.empty() || !sameOrigin)
        {
            if (!dl->cookies.empty()) SecureZeroMemory(&dl->cookies[0], dl->cookies.size() * sizeof(wchar_t));
            dl->cookies = newCookies;
            dl->cookiesEnc.clear();
        }
        NdmHttpEngine::UnlockFields(true);
        if (pr.sizeKnown && !NdmHttpEngine::AtomicRead64(&dl->totalSize))
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&dl->totalSize), (LONG64)pr.total);
    }
    std::wstring path = m_autoSavePath;
    LeaveCriticalSection(&m_cs);
    // The .ndmstate now names the new address: write the list at once so
    // both agree even if NDM is closed right now.
    if (res == NDM_REFRESH_OK && !path.empty())
    {
        InterlockedExchange(&m_dirty, 0);
        WriteQueue(path);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Auto-recovery
// ---------------------------------------------------------------------------
void NdmQueue::SetAutoRetryDelays(DWORD firstMs, DWORD maxMs)
{
    EnterCriticalSection(&m_cs);
    m_retryFirstMs = firstMs < 100 ? 100 : firstMs;
    m_retryMaxMs   = maxMs < m_retryFirstMs ? m_retryFirstMs : maxMs;
    LeaveCriticalSection(&m_cs);
}

// Queue every item whose auto-retry is due (nowFt) or, with nowFt == 0, all
// items waiting for one. Returns the earliest remaining due time (0 = none).
UINT64 NdmQueue::RunDueRetries(UINT64 nowFt)
{
    bool any = false;
    UINT64 next = 0;
    EnterCriticalSection(&m_cs);
    if (!m_shuttingDown)
    {
        for (auto& kv : m_downloads)
        {
            NdmDownload* d = kv.second;
            if (d->status != NDM_STATUS_ERROR || !d->nextRetryTime) continue;
            if (nowFt && d->nextRetryTime > nowFt)
            {
                if (!next || d->nextRetryTime < next) next = d->nextRetryTime;
                continue;
            }
            d->status = NDM_STATUS_QUEUED;
            d->nextRetryTime = 0;
            d->errorKind = NDM_ERRKIND_NONE;
            NdmHttpEngine::SetSharedString(d->errorMsg, L"");
            any = true;
        }
    }
    LeaveCriticalSection(&m_cs);
    if (any)
    {
        AutoSave();
        ScheduleNext();
    }
    return next;
}

void NdmQueue::RetryTransientNow()
{
    RunDueRetries(0);
}

void NdmQueue::OnNetworkRestored()
{
    RetryTransientNow();
    WakeTimer();
}

void NdmQueue::OnSystemResumed()
{
    // Missed scheduler events are caught up by the timer (wall-clock check).
    RetryTransientNow();
    WakeTimer();
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
void NdmQueue::SetSchedule(const NdmSchedule& s)
{
    EnterCriticalSection(&m_cs);
    m_schedule = s;
    LeaveCriticalSection(&m_cs);
    EnsureTimer();
    WakeTimer();
}

NdmSchedule NdmQueue::GetSchedule()
{
    EnterCriticalSection(&m_cs);
    NdmSchedule s = m_schedule;
    LeaveCriticalSection(&m_cs);
    return s;
}

bool NdmQueue::SetScheduled(DWORD id, bool on)
{
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    bool ok = it != m_downloads.end();
    if (ok) it->second->scheduled = on;
    LeaveCriticalSection(&m_cs);
    if (ok) AutoSave();
    return ok;
}

void NdmQueue::StartQueue()
{
    bool any = false;
    EnterCriticalSection(&m_cs);
    if (!m_shuttingDown)
    {
        for (auto& kv : m_downloads)
        {
            NdmDownload* d = kv.second;
            if (!d->scheduled || m_running.count(d)) continue;
            if (d->status != NDM_STATUS_PAUSED && d->status != NDM_STATUS_ERROR) continue;
            d->status = NDM_STATUS_QUEUED;
            d->nextRetryTime = 0;
            d->errorKind = NDM_ERRKIND_NONE;
            NdmHttpEngine::SetSharedString(d->errorMsg, L"");
            any = true;
        }
    }
    LeaveCriticalSection(&m_cs);
    if (any) AutoSave();
    ScheduleNext();
}

void NdmQueue::StopQueue()
{
    bool any = false;
    EnterCriticalSection(&m_cs);
    if (!m_shuttingDown)
    {
        for (auto& kv : m_downloads)
        {
            NdmDownload* d = kv.second;
            if (d->status != NDM_STATUS_DOWNLOADING && d->status != NDM_STATUS_QUEUED) continue;
            if (m_running.count(d) && d->hStopEvent) SetEvent(d->hStopEvent);
            d->status = NDM_STATUS_PAUSED;
            d->speed = 0;
            d->scheduled = true;          // the next start time resumes it
            any = true;
        }
    }
    LeaveCriticalSection(&m_cs);
    if (any) AutoSave();
}

// ---------------------------------------------------------------------------
// Core timer thread: scheduler events, auto-retries, network watcher.
// Wakes at the next due time, when woken, on address changes, and at least
// every 15 s (wall-clock check: catches clock changes / sleep).
// ---------------------------------------------------------------------------
void NdmQueue::EnsureTimer()
{
    EnterCriticalSection(&m_cs);
    if (!m_timerThread && !m_timerStopped && !m_shuttingDown && m_timerWake && m_timerQuit)
        m_timerThread = CreateThread(nullptr, 0, TimerThreadProc, this, 0, nullptr);
    LeaveCriticalSection(&m_cs);
}

void NdmQueue::WakeTimer()
{
    if (m_timerWake) SetEvent(m_timerWake);
}

void NdmQueue::StopTimer()
{
    EnterCriticalSection(&m_cs);
    m_timerStopped = true;
    HANDLE thr = m_timerThread;
    m_timerThread = nullptr;
    LeaveCriticalSection(&m_cs);
    if (m_timerQuit) SetEvent(m_timerQuit);
    if (thr)
    {
        WaitForSingleObject(thr, 10000);
        CloseHandle(thr);
    }
}

DWORD WINAPI NdmQueue::TimerThreadProc(LPVOID param)
{
    reinterpret_cast<NdmQueue*>(param)->TimerLoop();
    return 0;
}

void NdmQueue::TimerLoop()
{
    HANDLE addrEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    OVERLAPPED ov = {};
    ov.hEvent = addrEv;
    HANDLE hNotify = nullptr;
    bool armed = addrEv && NotifyAddrChange(&hNotify, &ov) == ERROR_IO_PENDING;
    ULONGLONG netCheckAt = 0;
    UINT64 lastLocal = NdmLocalNowFt();

    for (;;)
    {
        // scheduler: events between the last check and now
        EnterCriticalSection(&m_cs);
        NdmSchedule s = m_schedule;
        LeaveCriticalSection(&m_cs);
        UINT64 nowLocal = NdmLocalNowFt();
        if (nowLocal > lastLocal)
        {
            int ev = NdmScheduleEventBetween(s, lastLocal, nowLocal);
            if (ev == NDM_SCHED_START)     StartQueue();
            else if (ev == NDM_SCHED_STOP) StopQueue();
        }
        lastLocal = nowLocal;                 // a clock set back: just continue from here

        // network: 2 s after the last address change, retry waiting items
        ULONGLONG tick = GetTickCount64();
        if (netCheckAt && tick >= netCheckAt)
        {
            netCheckAt = 0;
            if (NetConnected()) OnNetworkRestored();
        }

        // auto-retries that are due
        UINT64 nextRetry = RunDueRetries(NowFt());

        DWORD wait = 15000;
        if (nextRetry)
        {
            UINT64 now = NowFt();
            UINT64 ms = nextRetry > now ? (nextRetry - now) / 10000 + 1 : 0;
            if (ms < wait) wait = (DWORD)ms;
        }
        UINT64 nextEv = NdmScheduleNextEvent(s, nowLocal);
        if (nextEv)
        {
            UINT64 ms = (nextEv - nowLocal) / 10000 + 1;
            if (ms < wait) wait = (DWORD)ms;
        }
        if (netCheckAt)
        {
            ULONGLONG ms = netCheckAt > tick ? netCheckAt - tick : 0;
            if (ms < wait) wait = (DWORD)ms;
        }

        HANDLE hs[3] = { m_timerQuit, m_timerWake, addrEv };
        DWORD r = WaitForMultipleObjects(armed ? 3 : 2, hs, FALSE, wait);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        if (armed && r == WAIT_OBJECT_0 + 2)
        {
            netCheckAt = GetTickCount64() + 2000;      // let DHCP / DNS settle
            armed = NotifyAddrChange(&hNotify, &ov) == ERROR_IO_PENDING;
        }
    }
    if (armed && CancelIPChangeNotify(&ov)) WaitForSingleObject(addrEv, 1000);
    if (addrEv) CloseHandle(addrEv);
}

// ---------------------------------------------------------------------------
// On-completion actions
// ---------------------------------------------------------------------------
void NdmQueue::SetCompletionActionCallback(NdmCompletionActionCallback cb, void* ctx)
{
    EnterCriticalSection(&m_cs);
    m_doneCb = cb;
    m_doneCtx = ctx;
    LeaveCriticalSection(&m_cs);
}

bool NdmQueue::SetDownloadCompletionAction(DWORD id, int action)
{
    if (action < NDM_DONE_NONE || action > NDM_DONE_SHUTDOWN) return false;
    EnterCriticalSection(&m_cs);
    auto it = m_downloads.find(id);
    bool ok = it != m_downloads.end() && it->second->status != NDM_STATUS_COMPLETED;
    if (ok) it->second->completionAction = action;
    LeaveCriticalSection(&m_cs);
    return ok;
}

void NdmQueue::SetAllDoneAction(int action)
{
    if (action < NDM_DONE_NONE || action > NDM_DONE_SHUTDOWN) action = NDM_DONE_NONE;
    EnterCriticalSection(&m_cs);
    m_allDoneAction = action;
    m_allDoneSeenCompletion = false;      // counts completions from now on
    LeaveCriticalSection(&m_cs);
}

int NdmQueue::GetAllDoneAction()
{
    EnterCriticalSection(&m_cs);
    int a = m_allDoneAction;
    LeaveCriticalSection(&m_cs);
    return a;
}

// Called with m_cs held.
void NdmQueue::CheckAllDone(std::vector<std::pair<int, DWORD>>& fire)
{
    if (m_allDoneAction == NDM_DONE_NONE || !m_allDoneSeenCompletion) return;
    for (auto& kv : m_downloads)
    {
        const NdmDownload* d = kv.second;
        if (d->status == NDM_STATUS_DOWNLOADING || d->status == NDM_STATUS_QUEUED) return;
        if (d->status == NDM_STATUS_ERROR && d->nextRetryTime) return;   // auto-retry pending
    }
    fire.push_back({ m_allDoneAction, 0 });
    m_allDoneAction = NDM_DONE_NONE;
    m_allDoneSeenCompletion = false;
}

void NdmQueue::FireActions(const std::vector<std::pair<int, DWORD>>& fire)
{
    if (fire.empty()) return;
    EnterCriticalSection(&m_cs);
    NdmCompletionActionCallback cb = m_doneCb;
    void* ctx = m_doneCtx;
    LeaveCriticalSection(&m_cs);
    if (!cb) return;
    for (const auto& f : fire) cb(f.first, f.second, ctx);
}
