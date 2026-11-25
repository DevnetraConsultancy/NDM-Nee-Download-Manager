// Queue tests (not part of the product).
//   qtest <dir>                        offline: queue.xml durability, new persisted fields,
//                                      queue order, lookups, scheduler math (uses <dir>\queue.xml
//                                      only, never the real %APPDATA% list)
//   qtest <dir> <baseUrl> <sha256>     + network tests against faultsrv.py (run_faults.py)
//   qtest --dump <queue.xml> <out>     (child process) load a list, write its items to <out>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include "ndm_common.h"
#include "ndm_queue.h"
#include "ndm_config.h"
#include "ndm_http.h"
#include "ndm_scheduler.h"

static int g_fail = 0;
#define CHECK(c, m) do { bool ok_ = (c); wprintf(L"  [%s] %s\n", ok_ ? L"PASS" : L"FAIL", m); if (!ok_) ++g_fail; } while (0)

static void WriteText(const std::wstring& p, const std::string& s)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(h);
}
static std::string ReadAll(const std::wstring& p)
{
    std::string s;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[65536]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd) s.append(buf, rd);
    CloseHandle(h);
    return s;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string U8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static UINT64 NowFt() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((UINT64)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static NdmDownload Snap(DWORD id) { NdmDownload d; NdmQueue::GetInstance().GetSnapshot(id, d); return d; }
static bool WaitFor(const std::function<bool()>& pred, DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) { if (pred()) return true; Sleep(50); }
    return pred();
}

static std::string HttpGet(const std::wstring& base, const std::wstring& path)
{
    std::string body;
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(base.c_str(), 0, 0, &uc)) return body;
    HINTERNET s = WinHttpOpen(L"qtest", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, 0) : nullptr;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
    {
        char buf[4096]; DWORD n = 0;
        while (WinHttpReadData(r, buf, sizeof(buf), &n) && n) body.append(buf, n);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return body;
}
static UINT64 StatSent(const std::wstring& base)
{
    std::string j = HttpGet(base, L"/stats");
    size_t p = j.find("\"sent\":");
    return p == std::string::npos ? 0 : _strtoui64(j.c_str() + p + 7, nullptr, 10);
}

static std::string Sha256File(const std::wstring& path)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0); return ""; }
    std::vector<BYTE> buf(1 << 20); DWORD rd = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd) BCryptHashData(h, buf.data(), rd, 0);
    CloseHandle(f);
    BYTE out[32]; BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex; char b[3];
    for (BYTE x : out) { sprintf_s(b, "%02x", x); hex += b; }
    return hex;
}

// One line per item, in queue order: the fields that must survive a save + load.
static std::string Dump()
{
    std::string s;
    for (DWORD id : NdmQueue::GetInstance().GetQueueOrder())
    {
        NdmDownload d = Snap(id);
        char b[512];
        sprintf_s(b, "%lu st=%d prio=%d lim=%lu ord=%lld sched=%d add=%llu done=%llu ek=%d btih=%s url=%s\n",
                  id, (int)d.status, d.priority, d.speedLimit, d.queueOrder, d.scheduled ? 1 : 0,
                  d.addedTime, d.completedTime, d.status == NDM_STATUS_ERROR ? d.errorKind : 0,
                  U8(d.torrentHash).c_str(), U8(d.url).c_str());
        s += b;
    }
    return s;
}

static UINT64 LocalFt(int y, int mo, int d, int h, int mi) { return NdmLocalFt(y, mo, d, h * 60 + mi); }

static void SchedulerMath()
{
    wprintf(L"scheduler math\n");
    NdmSchedule s;
    s.startEnabled = true; s.stopEnabled = true; s.startMinute = 8 * 60; s.stopMinute = 10 * 60;
    CHECK(NdmScheduleEventBetween(s, LocalFt(2026, 11, 24, 7, 59), LocalFt(2026, 11, 24, 8, 0)) == NDM_SCHED_START,
          L"daily 08:00 start fires at 08:00");
    CHECK(NdmScheduleEventBetween(s, LocalFt(2026, 11, 24, 8, 0), LocalFt(2026, 11, 24, 8, 1)) == NDM_SCHED_NONE,
          L"no second start a minute later");
    CHECK(NdmScheduleEventBetween(s, LocalFt(2026, 11, 24, 9, 0), LocalFt(2026, 11, 24, 10, 0)) == NDM_SCHED_STOP,
          L"10:00 stop fires");
    CHECK(NdmScheduleEventBetween(s, LocalFt(2026, 11, 24, 7, 0), LocalFt(2026, 11, 24, 11, 0)) == NDM_SCHED_STOP,
          L"slept through start and stop: only the later stop counts");
    CHECK(NdmScheduleNextEvent(s, LocalFt(2026, 11, 24, 10, 30)) == LocalFt(2026, 11, 25, 8, 0),
          L"next event after 10:30 is tomorrow 08:00");
    NdmSchedule n;                         // overnight, Mondays only
    n.startEnabled = true; n.stopEnabled = true; n.startMinute = 23 * 60; n.stopMinute = 7 * 60; n.dayMask = 1 << 1;
    CHECK(NdmScheduleEventBetween(n, LocalFt(2026, 11, 23, 22, 59), LocalFt(2026, 11, 23, 23, 0)) == NDM_SCHED_START,
          L"Monday 23:00 start (Monday-only schedule)");
    CHECK(NdmScheduleEventBetween(n, LocalFt(2026, 11, 24, 6, 59), LocalFt(2026, 11, 24, 7, 0)) == NDM_SCHED_STOP,
          L"its 07:00 stop fires on Tuesday morning");
    CHECK(NdmScheduleEventBetween(n, LocalFt(2026, 11, 24, 22, 0), LocalFt(2026, 11, 25, 8, 0)) == NDM_SCHED_NONE,
          L"nothing on Tuesday night");
    NdmSchedule o;                         // once
    o.startEnabled = true; o.once = true; o.onceDate = 20261125; o.startMinute = 9 * 60;
    CHECK(NdmScheduleEventBetween(o, LocalFt(2026, 11, 25, 8, 59), LocalFt(2026, 11, 25, 9, 0)) == NDM_SCHED_START,
          L"one-off start on its date");
    CHECK(NdmScheduleEventBetween(o, LocalFt(2026, 11, 26, 8, 59), LocalFt(2026, 11, 26, 9, 0)) == NDM_SCHED_NONE,
          L"one-off start not repeated the next day");
    NdmSchedule off;
    CHECK(NdmScheduleNextEvent(off, LocalFt(2026, 11, 24, 0, 0)) == 0, L"disabled schedule has no events");
}

static int DumpMode(const std::wstring& xml, const std::wstring& out)
{
    NdmQueue& Q = NdmQueue::GetInstance();
    Q.LoadQueue(xml);
    std::string d = Dump();
    Q.Shutdown(3000);
    WriteText(out, d);
    return 0;
}

// --------------------------------------------------------------------------
// offline tests
// --------------------------------------------------------------------------
static void Offline(const std::wstring& dir, const wchar_t* exe)
{
    std::wstring q = dir + L"\\queue.xml";
    // a finished file from an old list without dates: its last-write time is the completion time
    std::wstring oldFile = dir + L"\\old.bin";
    WriteText(oldFile, "x");
    SYSTEMTIME st = { 2025, 1, 0, 2, 3, 4, 5, 0 };
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    HANDLE hf = CreateFileW(oldFile.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    SetFileTime(hf, nullptr, nullptr, &ft); CloseHandle(hf);
    const UINT64 oldFt = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    std::string d8 = U8(dir);
    std::string good =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<NDMQueue version=\"2\">\r\n"
        "  <Download id=\"7\" url=\"https://example.com/a.zip\" file=\"a.zip\" path=\"C:\\\\x\" status=\"2\" downloaded=\"5\" total=\"10\" connections=\"8\" usernamed=\"0\"/>\r\n"
        "  <Download id=\"8\" url=\"https://example.com/old.bin\" file=\"old.bin\" path=\"" + d8 + "\" status=\"3\" downloaded=\"1\" total=\"1\" connections=\"8\" usernamed=\"1\"/>\r\n"
        "  <Download id=\"9\" url=\"https://example.com/gone.bin\" file=\"gone.bin\" path=\"" + d8 + "\" status=\"3\" downloaded=\"1\" total=\"1\" connections=\"8\" usernamed=\"1\"/>\r\n"
        "  <Download id=\"10\" url=\"https://example.com/p.bin\" file=\"p.bin\" path=\"C:\\\\x\" status=\"4\" downloaded=\"0\" total=\"0\" connections=\"8\" usernamed=\"0\""
        " error=\"HTTP 404\" errkind=\"2\" prio=\"2\" limit=\"12345\" order=\"-5\" sched=\"1\" added=\"133000000000000000\" btih=\"0123456789abcdef0123456789abcdef01234567\"/>\r\n"
        "</NDMQueue>\r\n";

    wprintf(L"durability\n");
    // 1. queue.xml cut off by a power loss, good .bak -> items come from .bak, damaged copy kept.
    WriteText(q, "<?xml version=\"1.0\"?>\r\n<NDMQueue version=\"2\">\r\n  <Download id=\"7\" url=\"https://exa");
    WriteText(q + L".bak", good);
    NdmQueue& Q = NdmQueue::GetInstance();
    Q.LoadQueue(q);
    NdmDownload d;
    CHECK(Q.GetSnapshot(7, d) && d.filename == L"a.zip", L"cut-off queue.xml: list restored from queue.xml.bak");
    CHECK(Exists(q + L".corrupt"), L"damaged queue.xml kept as queue.xml.corrupt");
    // 2. A save writes a complete file and keeps the previous one as .bak.
    Q.SaveQueue(q);
    std::string buf = ReadAll(q);
    CHECK(buf.find("</NDMQueue>") != std::string::npos && buf.find("a.zip") != std::string::npos, L"saved queue.xml is complete");
    CHECK(Exists(q + L".bak") && !Exists(q + L".tmp"), L"previous list kept as .bak, no .tmp left");

    wprintf(L"dates\n");
    CHECK(Snap(8).completedTime == oldFt, L"old list: completion date taken from the finished file's last-write time");
    CHECK(Snap(8).addedTime == 0, L"old list: unknown 'added' date stays empty (not invented)");
    CHECK(Snap(9).completedTime == 0, L"old list, file gone: completion date stays empty");
    NdmAddParams p;
    p.url = L"https://Example.COM/dir/x.zip#part2"; p.savePath = dir; p.startNow = false;
    UINT64 t0 = NowFt();
    DWORD nid = Q.AddDownloadEx(p);
    UINT64 t1 = NowFt();
    CHECK(Snap(nid).addedTime >= t0 && Snap(nid).addedTime <= t1, L"new item: 'added' is the current UTC time");
    CHECK(Snap(nid).completedTime == 0, L"new item: no completion date yet");

    wprintf(L"fields + order\n");
    NdmDownload d10 = Snap(10);
    CHECK(d10.priority == NDM_PRIO_HIGH && d10.speedLimit == 12345 && d10.queueOrder == -5 && d10.scheduled &&
          d10.addedTime == 133000000000000000ULL && d10.errorKind == NDM_ERRKIND_FATAL &&
          d10.torrentHash == L"0123456789abcdef0123456789abcdef01234567",
          L"prio / limit / order / sched / added / errkind / btih attributes loaded");
    CHECK(Snap(7).priority == NDM_PRIO_NORMAL && Snap(7).speedLimit == 0 && Snap(7).queueOrder == 7,
          L"old item: Normal priority, no limit, order = id (FIFO kept)");
    std::vector<DWORD> ord = Q.GetQueueOrder();
    CHECK(ord.size() == 5 && ord[0] == 10 && ord[4] == nid, L"queue order: lowest order first, new item last");
    CHECK(Q.MoveToBottom(10) && Q.GetQueueOrder().back() == 10, L"MoveToBottom");
    CHECK(Q.MoveUp(10) && Q.GetQueueOrder()[3] == 10, L"MoveUp");
    CHECK(Q.MoveToTop(9) && Q.GetQueueOrder()[0] == 9, L"MoveToTop");
    CHECK(Q.MoveDown(9) && Q.GetQueueOrder()[1] == 9 && !Q.MoveUp(Q.GetQueueOrder()[0]), L"MoveDown; MoveUp of the first item refused");
    CHECK(Q.SetDownloadSpeedLimit(nid, 777777) && Q.SetPriority(nid, NDM_PRIO_LOW) && Snap(nid).speedLimit == 777777 &&
          Snap(nid).priority == NDM_PRIO_LOW, L"SetDownloadSpeedLimit / SetPriority");
    CHECK(!Q.SetPriority(99999, 2), L"SetPriority of an unknown id fails");

    Q.SaveQueue(q);
    buf = ReadAll(q);
    CHECK(buf.find(" prio=\"2\"") != std::string::npos && buf.find(" limit=\"12345\"") != std::string::npos &&
          buf.find(" prio=\"0\"") != std::string::npos && buf.find(" limit=\"777777\"") != std::string::npos,
          L"saved: prio / limit written for non-default values");
    {
        size_t a = buf.find("id=\"7\""), e = buf.find("/>", a);
        std::string item7 = buf.substr(a, e - a);
        CHECK(item7.find("prio=") == std::string::npos && item7.find("limit=") == std::string::npos &&
              item7.find("added=") == std::string::npos && item7.find(" order=\"") != std::string::npos,
              L"saved: defaults / unknown dates are not written, order always is");
    }
    CHECK(buf.find(" completed=\"" + std::to_string(oldFt) + "\"") != std::string::npos, L"saved: completion date");

    // Round trip through a fresh process (same code path as an app restart).
    std::wstring rt = dir + L"\\rt";
    CreateDirectoryW(rt.c_str(), nullptr);
    CopyFileW(q.c_str(), (rt + L"\\queue.xml").c_str(), FALSE);
    std::wstring out = rt + L"\\dump.txt";
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --dump \"" + rt + L"\\queue.xml\" \"" + out + L"\"";
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    bool ran = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
    if (ran) { WaitForSingleObject(pi.hProcess, 30000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    std::string mine = Dump(), theirs = ReadAll(out);
    CHECK(ran && !mine.empty() && mine == theirs, L"save + load in a new process: order, priority, limit, dates, sched, errkind, btih identical");
    if (mine != theirs) printf("--- here:\n%s--- after reload:\n%s", mine.c_str(), theirs.c_str());

    wprintf(L"lookups\n");
    std::vector<DWORD> f = Q.FindByUrl(L"  https://example.com/dir/x.zip ");
    CHECK(f.size() == 1 && f[0] == nid, L"FindByUrl: fragment dropped, host case-insensitive");
    CHECK(Q.FindByUrl(L"https://example.com/dir/X.zip").empty(), L"FindByUrl: path stays case-sensitive");
    NdmAddParams m;
    m.url = L"magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&dn=one"; m.kind = NDM_KIND_TORRENT;
    m.savePath = dir; m.startNow = false;
    DWORD mid = Q.AddDownloadEx(m);
    f = Q.FindByUrl(L"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=other&tr=x");
    CHECK(f.size() == 1 && f[0] == mid, L"FindByUrl: magnet links compared by info hash");
    CHECK(Q.FindRefreshCandidate(L"https://other/x.zip", L"x.zip", 1000) == 0,
          L"FindRefreshCandidate: unknown size never matches");

    wprintf(L"rename\n");
    std::wstring rmsg;
    CHECK(Q.RenameDownload(8, L"renamed.bin", rmsg) && Exists(dir + L"\\renamed.bin") && !Exists(oldFile) &&
          Snap(8).filename == L"renamed.bin", L"RenameDownload: completed item renames its file on disk and the item");
    WriteText(dir + L"\\taken.bin", "y");
    CHECK(!Q.RenameDownload(8, L"taken.bin", rmsg) && !rmsg.empty() && Exists(dir + L"\\renamed.bin") &&
          ReadAll(dir + L"\\taken.bin") == "y" && Snap(8).filename == L"renamed.bin",
          L"RenameDownload: never replaces another file");
    CHECK(!Q.RenameDownload(8, L"bad:name.bin", rmsg) && !rmsg.empty(), L"RenameDownload: invalid name refused");
    CHECK(Q.RenameDownload(nid, L"y.zip", rmsg) && Snap(nid).filename == L"y.zip",
          L"RenameDownload: not-started HTTP item: only the target name changes");
    CHECK(!Q.RenameDownload(7, L"b.zip", rmsg) && Snap(7).filename == L"a.zip",
          L"RenameDownload: partly downloaded item refused");
    CHECK(!Q.RenameDownload(mid, L"t.bin", rmsg), L"RenameDownload: torrent refused");
    WriteText(dir + L"\\y.zip.ndmpart", "z");
    CHECK(!Q.RenameDownload(nid, L"z.zip", rmsg) && Snap(nid).filename == L"y.zip",
          L"RenameDownload: refused while a .ndmpart exists");
    DeleteFileW((dir + L"\\y.zip.ndmpart").c_str());
    CHECK(!Q.RenameDownload(99999, L"q.bin", rmsg), L"RenameDownload: unknown id fails");
    Q.RemoveDownload(mid);

    SchedulerMath();
}

// --------------------------------------------------------------------------
// network tests (faultsrv.py)
// --------------------------------------------------------------------------
static std::vector<std::pair<int, DWORD>> g_events;
static CRITICAL_SECTION g_evCs;
static void OnDone(int action, DWORD id, void*)
{
    EnterCriticalSection(&g_evCs);
    g_events.push_back({ action, id });
    LeaveCriticalSection(&g_evCs);
}

static void Network(const std::wstring& dir, const std::wstring& base, const std::string& sha)
{
    NdmQueue& Q = NdmQueue::GetInstance();
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();          // in memory only, never saved
    cfg.SetRetryCount(1);
    cfg.SetRetryMaxDelay(1);
    const UINT64 SIZE = 16ULL * 1024 * 1024;
    std::wstring nd = dir + L"\\net";
    CreateDirectoryW(nd.c_str(), nullptr);
    auto ctl = [&](const wchar_t* qs) { HttpGet(base, std::wstring(L"/ctl?") + qs); };
    auto add = [&](const wchar_t* path, const wchar_t* name, bool start, DWORD limit = 0, int prio = NDM_PRIO_NORMAL) {
        NdmAddParams p;
        p.url = base + path; p.savePath = nd; p.filename = name; p.startNow = start;
        p.speedLimit = limit; p.priority = prio;
        return Q.AddDownloadEx(p);
    };
    auto status = [&](DWORD id) { return Snap(id).status; };
    auto got = [&](DWORD id) { return Snap(id).downloadedSize; };
    auto rateOver = [&](DWORD id, DWORD ms) {
        UINT64 b0 = got(id); ULONGLONG t0 = GetTickCount64();
        Sleep(ms);
        return (double)(got(id) - b0) * 1000.0 / (double)(GetTickCount64() - t0);
    };

    wprintf(L"per-download limit (queue API, live change)\n");
    ctl(L"mode=normal&seed=1&delay=0.002"); HttpGet(base, L"/reset");
    DWORD lid = add(L"/file", L"lim.bin", true, 1024 * 1024);
    WaitFor([&] { return got(lid) > 0; }, 10000);
    Sleep(2000);
    double r1 = rateOver(lid, 2500);
    Q.SetDownloadSpeedLimit(lid, 2 * 1024 * 1024);
    Sleep(1500);
    double r2 = rateOver(lid, 2500);
    wchar_t msg[200];
    swprintf_s(msg, L"1 MiB/s limit: measured %.0f B/s (within 15%%)", r1);
    CHECK(r1 > 0.85 * 1048576 && r1 < 1.15 * 1048576, msg);
    swprintf_s(msg, L"raised to 2 MiB/s while running: measured %.0f B/s (within 15%%)", r2);
    CHECK(r2 > 0.85 * 2097152 && r2 < 1.15 * 2097152, msg);
    Q.RemoveDownload(lid);

    wprintf(L"start order by priority\n");
    Q.SetMaxActiveDownloads(1);
    DWORD a = add(L"/file", L"pa.bin", true, 256 * 1024);
    WaitFor([&] { return status(a) == NDM_STATUS_DOWNLOADING; }, 5000);
    DWORD lo = add(L"/file", L"pl.bin", true, 256 * 1024, NDM_PRIO_LOW);
    DWORD no = add(L"/file", L"pn.bin", true, 256 * 1024, NDM_PRIO_NORMAL);
    DWORD hi = add(L"/file", L"ph.bin", true, 256 * 1024, NDM_PRIO_HIGH);
    CHECK(status(lo) == NDM_STATUS_QUEUED && status(no) == NDM_STATUS_QUEUED && status(hi) == NDM_STATUS_QUEUED,
          L"max 1 active: Low, Normal, High (added in that order) wait");
    Q.PauseDownload(a);
    CHECK(status(hi) == NDM_STATUS_DOWNLOADING && status(no) == NDM_STATUS_QUEUED && status(lo) == NDM_STATUS_QUEUED,
          L"slot free: the High one starts first");
    Q.PauseDownload(hi);
    CHECK(status(no) == NDM_STATUS_DOWNLOADING && status(lo) == NDM_STATUS_QUEUED, L"then Normal before Low");
    for (DWORD id : { a, lo, no, hi }) Q.RemoveDownload(id);
    Q.SetMaxActiveDownloads(3);

    wprintf(L"refresh download address\n");
    HttpGet(base, L"/reset");
    DWORD rid = add(L"/file", L"r.bin", true);
    WaitFor([&] { return got(rid) >= 6 * 1024 * 1024; }, 20000);
    Q.PauseDownload(rid);
    std::wstring m;
    NdmRefreshResult rr = NDM_REFRESH_BUSY;
    WaitFor([&] { rr = Q.RefreshAddress(rid, base + L"/short", L"", L"", m); return rr != NDM_REFRESH_BUSY; }, 15000);
    CHECK(rr == NDM_REFRESH_SIZE_MISMATCH, (L"other size refused: " + m).c_str());
    CHECK(Q.FindRefreshCandidate(L"https://mirror.example/r.bin", L"R.BIN", SIZE) == rid,
          L"FindRefreshCandidate: paused item with same name (any case) and size");
    rr = Q.RefreshAddress(rid, base + L"/newer", L"", L"", m);
    CHECK(rr == NDM_REFRESH_DIFFERENT_FILE, (L"same size, other Last-Modified refused: " + m).c_str());
    rr = Q.RefreshAddress(rid, base + L"/missing", L"", L"", m);
    CHECK(rr == NDM_REFRESH_HTTP_ERROR, (L"404 refused: " + m).c_str());
    rr = Q.RefreshAddress(rid, L"http://127.0.0.1:1/r.bin", L"", L"", m);
    CHECK(rr == NDM_REFRESH_NETWORK_ERROR, (L"unreachable server: " + m).c_str());
    CHECK(Q.RefreshAddress(rid, L"ftp://x/r.bin", L"", L"", m) == NDM_REFRESH_NOT_HTTP, L"ftp:// refused");
    std::wstring statePath = nd + L"\\r.bin.ndmstate";
    UINT64 before = got(rid);
    rr = Q.RefreshAddress(rid, base + L"/file2", L"", L"", m);
    CHECK(rr == NDM_REFRESH_OK && Snap(rid).url == base + L"/file2", L"same file under a new address accepted");
    CHECK(ReadAll(statePath).find("url=" + U8(base) + "/file2\n") != std::string::npos && Exists(nd + L"\\r.bin.ndmpart"),
          L".ndmstate rebound to the new address, .ndmpart kept");
    HttpGet(base, L"/reset");
    Q.ResumeDownload(rid);
    WaitFor([&] { return status(rid) == NDM_STATUS_COMPLETED || status(rid) == NDM_STATUS_ERROR; }, 60000);
    UINT64 sent = StatSent(base);
    CHECK(status(rid) == NDM_STATUS_COMPLETED && Sha256File(nd + L"\\r.bin") == sha && !Exists(nd + L"\\r (1).bin"),
          L"resumed from the new address: complete, SHA-256 correct, no second file");
    swprintf_s(msg, L"resume continued the part (%llu bytes on disk before, fetched %llu of %llu)", before, sent, SIZE);
    CHECK(sent > 0 && sent <= SIZE - before + 8 * 1024 * 1024 && sent < SIZE * 3 / 4, msg);
    NdmDownload rd = Snap(rid);
    CHECK(rd.addedTime > 0 && rd.completedTime >= rd.addedTime && rd.completedTime <= NowFt(),
          L"completion date set (after the added date)");

    wprintf(L"auto-retry\n");
    Q.SetAutoRetryDelays(1500, 3000);
    ctl(L"mode=drop");
    DWORD tid = add(L"/file", L"t.bin", true);
    WaitFor([&] { return status(tid) == NDM_STATUS_ERROR; }, 30000);
    NdmDownload td = Snap(tid);
    CHECK(td.status == NDM_STATUS_ERROR && td.errorKind == NDM_ERRKIND_TRANSIENT && td.nextRetryTime > NowFt(),
          (L"server unreachable: error marked transient, auto-retry planned (" + td.errorMsg + L")").c_str());
    ctl(L"mode=normal");
    DWORD fid = add(L"/missing", L"f.bin", true);
    WaitFor([&] { return status(tid) == NDM_STATUS_COMPLETED; }, 40000);
    CHECK(status(tid) == NDM_STATUS_COMPLETED && Sha256File(nd + L"\\t.bin") == sha,
          L"server back: retried automatically, complete, SHA-256 correct");
    WaitFor([&] { return status(fid) == NDM_STATUS_ERROR; }, 15000);
    Sleep(4000);
    NdmDownload fd = Snap(fid);
    CHECK(fd.status == NDM_STATUS_ERROR && fd.errorKind == NDM_ERRKIND_FATAL && fd.nextRetryTime == 0,
          (L"404: fatal, not retried (" + fd.errorMsg + L")").c_str());

    Q.SetAutoRetryDelays(600000, 600000);
    ctl(L"mode=drop");
    DWORD uid = add(L"/file", L"u.bin", true);
    WaitFor([&] { return status(uid) == NDM_STATUS_ERROR; }, 30000);
    ctl(L"mode=normal");
    ULONGLONG ts = GetTickCount64();
    Q.OnNetworkRestored();
    WaitFor([&] { return status(uid) == NDM_STATUS_COMPLETED; }, 30000);
    swprintf_s(msg, L"OnNetworkRestored retries at once (10 min backoff pending): complete after %.1f s",
               (GetTickCount64() - ts) / 1000.0);
    CHECK(status(uid) == NDM_STATUS_COMPLETED && GetTickCount64() - ts < 30000 && Sha256File(nd + L"\\u.bin") == sha, msg);

    wprintf(L"completion actions\n");
    InitializeCriticalSection(&g_evCs);
    Q.SetCompletionActionCallback(OnDone, nullptr);
    Q.SetAllDoneAction(NDM_DONE_SHUTDOWN);
    DWORD cid = add(L"/file", L"c.bin", false);
    Q.SetDownloadCompletionAction(cid, NDM_DONE_SLEEP);
    Q.ResumeDownload(cid);
    WaitFor([&] { return status(cid) == NDM_STATUS_COMPLETED; }, 30000);
    Sleep(300);
    EnterCriticalSection(&g_evCs);
    auto ev = g_events;
    LeaveCriticalSection(&g_evCs);
    CHECK(ev.size() == 2 && ev[0] == std::make_pair((int)NDM_DONE_SLEEP, cid) && ev[1] == std::make_pair((int)NDM_DONE_SHUTDOWN, (DWORD)0),
          L"per-download action, then 'all done' action, each fired once");
    CHECK(Q.GetAllDoneAction() == NDM_DONE_NONE, L"'all done' action is one-shot");

    wprintf(L"scheduler queue\n");
    NdmAddParams sp;
    sp.url = base + L"/file"; sp.savePath = nd; sp.filename = L"s.bin"; sp.startNow = false;
    sp.scheduled = true; sp.speedLimit = 256 * 1024;
    DWORD sid = Q.AddDownloadEx(sp);
    Q.StartQueue();
    CHECK(status(sid) == NDM_STATUS_DOWNLOADING, L"StartQueue starts the scheduled item");
    Q.StopQueue();
    CHECK(status(sid) == NDM_STATUS_PAUSED && Snap(sid).scheduled, L"StopQueue pauses it (stays in the scheduler queue)");
    SYSTEMTIME lt; GetLocalTime(&lt);
    if (lt.wSecond >= 55) { Sleep((61 - lt.wSecond) * 1000); GetLocalTime(&lt); }
    NdmSchedule s;
    s.startEnabled = true;
    s.startMinute = (lt.wHour * 60 + lt.wMinute + 1) % 1440;
    WaitFor([&] { return false; }, 300);                       // let the paused run exit
    Q.SetSchedule(s);
    bool started = WaitFor([&] { return status(sid) == NDM_STATUS_DOWNLOADING; }, 75000);
    CHECK(started, L"timer thread: the start time (next minute) starts the queue");
    Q.SetSchedule(NdmSchedule());
    Q.PauseDownload(sid);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 4 && wcscmp(argv[1], L"--dump") == 0) return DumpMode(argv[2], argv[3]);
    if (argc < 2) return 2;
    std::wstring dir = argv[1];
    Offline(dir, argv[0]);
    if (argc >= 4)
    {
        std::string sha = U8(argv[3]);
        Network(dir, argv[2], sha);
    }
    NdmQueue::GetInstance().Shutdown(3000);
    wprintf(L"%s\n", g_fail ? L"QUEUE TESTS FAILED" : L"all queue tests passed");
    return g_fail ? 1 : 0;
}
