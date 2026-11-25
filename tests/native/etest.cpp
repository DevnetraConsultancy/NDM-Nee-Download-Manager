// Engine driver for fault tests (not part of the product).
//   etest <url> <dir> <name> <id> [stopAtBytes] [key=value ...]
// Runs NdmHttpEngine::Run once; stops when downloadedSize >= stopAtBytes.
// Options:
//   limit=<bps>   per-download speed limit       prio=<0|1|2> priority
//   glimit=<bps>  global speed limit             secs=<s>     stop after s seconds
//   from=<s>      rate measured from this second (default 1.5) to the end
//   name2=<file>  run a second download of the same URL at the same time
//                 (id+1000), with limit2= / prio2= / url2=
//   retries=<n>   engine retry count (default 10)
// Prints (one line per download, the second one prefixed "B "):
//   result=<n> got=<bytes> total=<bytes> resumable=<n> mapok=<n> rewound=<n> rate=<bps> ek=<errorKind> err=<text>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <map>
#include "ndm_common.h"
#include "ndm_http.h"

struct Item
{
    NdmDownload* dl = nullptr;
    NdmRunResult r = NDM_RUN_ERROR;
    HANDLE       thread = nullptr;
    UINT64       maxSeen = 0;
    bool         rewound = false;
    UINT64       b0 = 0, b1 = 0;
    ULONGLONG    t0 = 0, t1 = 0;
    volatile LONG done = 0;
};

static Item      g_items[2];
static int       g_count = 1;
static UINT64    g_stopAt = 0;
static double    g_secs = 0, g_from = 1.5;
static volatile bool g_quit = false;
static ULONGLONG g_start = 0;

static DWORD WINAPI RunProc(LPVOID p)
{
    Item* it = (Item*)p;
    NdmRunOptions opt;
    opt.userNamed = true;
    it->r = NdmHttpEngine::Run(it->dl, opt);
    InterlockedExchange(&it->done, 1);
    return 0;
}

static DWORD WINAPI WatchProc(LPVOID)
{
    while (!g_quit)
    {
        ULONGLONG now = GetTickCount64();
        double el = (now - g_start) / 1000.0;
        for (int i = 0; i < g_count; ++i)
        {
            Item& it = g_items[i];
            if (it.done) continue;
            UINT64 g = NdmHttpEngine::AtomicRead64(&it.dl->downloadedSize);
            if (g > it.maxSeen) it.maxSeen = g;
            if (g + 4 * 1024 * 1024 < it.maxSeen) it.rewound = true;   // progress thrown away
            if (!it.t0 && el >= g_from) { it.t0 = now; it.b0 = g; }
            if (it.t0) { it.t1 = now; it.b1 = g; }
            if (i == 0 && g_stopAt && g >= g_stopAt) SetEvent(it.dl->hStopEvent);
            if (g_secs > 0 && el >= g_secs) SetEvent(it.dl->hStopEvent);
        }
        Sleep(5);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) { fwprintf(stderr, L"usage\n"); return 2; }
    std::map<std::wstring, std::wstring> o;
    int firstOpt = 5;
    if (argc > 5 && !wcschr(argv[5], L'=')) { g_stopAt = _wcstoui64(argv[5], nullptr, 10); firstOpt = 6; }
    for (int i = firstOpt; i < argc; ++i)
    {
        const wchar_t* eq = wcschr(argv[i], L'=');
        if (eq) o[std::wstring(argv[i], (size_t)(eq - argv[i]))] = eq + 1;
    }
    auto num = [&](const wchar_t* k, double def) { return o.count(k) ? _wtof(o[k].c_str()) : def; };
    g_secs = num(L"secs", 0);
    g_from = num(L"from", 1.5);
    NdmHttpEngine::SetSpeedLimit((DWORD)num(L"glimit", 0));
    NdmHttpEngine::SetRetryPolicy((int)num(L"retries", 10), 60);

    g_count = o.count(L"name2") ? 2 : 1;
    for (int i = 0; i < g_count; ++i)
    {
        NdmDownload* dl = new NdmDownload();
        dl->url = (i == 1 && o.count(L"url2")) ? o[L"url2"] : argv[1];
        dl->savePath = argv[2];
        dl->filename = i == 0 ? argv[3] : o[L"name2"];
        dl->id = (DWORD)_wtoi(argv[4]) + (i ? 1000 : 0);
        dl->numConnections = 8;
        dl->speedLimit = (DWORD)num(i ? L"limit2" : L"limit", 0);
        dl->priority = (int)num(i ? L"prio2" : L"prio", NDM_PRIO_NORMAL);
        dl->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        NdmHttpEngine::PrepareSharedStrings(dl);
        g_items[i].dl = dl;
    }
    g_start = GetTickCount64();
    HANDLE w = CreateThread(nullptr, 0, WatchProc, nullptr, 0, nullptr);
    for (int i = 0; i < g_count; ++i)
        g_items[i].thread = CreateThread(nullptr, 0, RunProc, &g_items[i], 0, nullptr);
    for (int i = 0; i < g_count; ++i) WaitForSingleObject(g_items[i].thread, INFINITE);
    g_quit = true;
    WaitForSingleObject(w, 2000);
    for (int i = 0; i < g_count; ++i)
    {
        Item& it = g_items[i];
        NdmDownload* dl = it.dl;
        double secs = it.t1 > it.t0 ? (it.t1 - it.t0) / 1000.0 : 0;
        unsigned long long rate = secs > 0 ? (unsigned long long)((it.b1 - it.b0) / secs) : 0;
        wprintf(L"%sresult=%d got=%llu total=%llu resumable=%ld mapok=%ld rewound=%d rate=%llu ek=%d err=%s\n",
                i ? L"B " : L"", (int)it.r,
                (unsigned long long)dl->downloadedSize, (unsigned long long)dl->totalSize, dl->resumable,
                dl->fileMapOk, it.rewound ? 1 : 0, rate, dl->errorKind, NdmHttpEngine::GetSharedString(dl->errorMsg).c_str());
    }
    return 0;
}
