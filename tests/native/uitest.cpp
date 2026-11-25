// UI rule tests (not part of the product): src/ui/ndm_uilogic.cpp
//   uitest            prints [PASS] / [FAIL] lines, exit code = failures
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <oleacc.h>
#include "ndm_common.h"
#include "ndm_uilogic.h"
#include "ndm_theme.h"
#include "ndm_config.h"

static int g_fail = 0;
#define CHECK(c, m) do { bool ok_ = (c); wprintf(L"  [%s] %s\n", ok_ ? L"PASS" : L"FAIL", m); if (!ok_) ++g_fail; } while (0)

static SYSTEMTIME St(int y, int mo, int d, int h, int mi)
{
    SYSTEMTIME s = {};
    s.wYear = (WORD)y; s.wMonth = (WORD)mo; s.wDay = (WORD)d; s.wHour = (WORD)h; s.wMinute = (WORD)mi;
    FILETIME ft; SystemTimeToFileTime(&s, &ft); FileTimeToSystemTime(&ft, &s);   // fills wDayOfWeek
    return s;
}

static void Dates()
{
    using namespace NdmUi;
    wprintf(L"dates\n");
    CHECK(ListDateFt(NDM_STATUS_COMPLETED, 100, 200) == 200, L"finished item: completion date");
    CHECK(ListDateFt(NDM_STATUS_PAUSED, 100, 200) == 100, L"unfinished item: added date");
    CHECK(ListDateFt(NDM_STATUS_COMPLETED, 100, 0) == 0, L"finished, completion unknown: blank (no fallback guess)");
    CHECK(FormatListDate(0).empty(), L"0 -> blank");
    std::wstring gb = FormatLocalDate(St(2026, 11, 24, 16, 52), L"en-GB");
    CHECK(gb == L"24 Nov 2026, 16:52", (L"en-GB: \"" + gb + L"\"").c_str());
    std::wstring us = FormatLocalDate(St(2026, 11, 24, 16, 52), L"en-US");
    CHECK(us == L"24 Nov 2026, 4:52 PM", (L"en-US: \"" + us + L"\"").c_str());
    std::wstring de = FormatLocalDate(St(2026, 3, 5, 7, 5), L"de-DE");
    CHECK(de.rfind(L"5 M", 0) == 0 && de.find(L" 2026, 07:05") != std::wstring::npos,
          (L"de-DE (locale month name, 24 h): \"" + de + L"\"").c_str());
    // UTC -> local with this PC's time zone, same as Windows itself
    SYSTEMTIME u = St(2026, 7, 1, 12, 0), loc;
    FILETIME ft; SystemTimeToFileTime(&u, &ft);
    SystemTimeToTzSpecificLocalTime(nullptr, &u, &loc);
    UINT64 v = ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    CHECK(FormatListDate(v) == FormatLocalDate(loc, nullptr) && !FormatListDate(v).empty(),
          L"UTC FILETIME shown in local time");
    CHECK(FormatListDate(v).size() <= 22, L"compact (<= 22 characters)");
}

static void Search()
{
    using namespace NdmUi;
    wprintf(L"search\n");
    CHECK(MatchesSearch(L"Ubuntu-24.04.iso", L"https://a/b", L""), L"empty query matches everything");
    CHECK(MatchesSearch(L"Ubuntu-24.04.iso", L"https://a/b", L"UBUNTU"), L"name, case-insensitive");
    CHECK(MatchesSearch(L"x.zip", L"https://releases.example.com/x.zip", L"Example.COM"), L"address, case-insensitive");
    CHECK(!MatchesSearch(L"x.zip", L"https://a/x.zip", L"y.zip"), L"no match");
    CHECK(MatchesSearch(L"Ärger.pdf", L"", L"ärger"), L"non-ASCII case folding");
    CHECK(!MatchesSearch(L"ab", L"", L"abc"), L"query longer than the text");
}

static void Clipboard()
{
    using namespace NdmUi;
    wprintf(L"clipboard\n");
    CHECK(ClipboardDownloadLink(L"https://ex.com/files/a.zip") == L"https://ex.com/files/a.zip", L"zip link");
    CHECK(ClipboardDownloadLink(L"  \"https://ex.com/a.ISO?sig=1#x\"\r\n") == L"https://ex.com/a.ISO?sig=1#x",
          L"trimmed, quotes, query, upper-case extension");
    CHECK(ClipboardDownloadLink(L"http://ex.com/setup.exe") == L"http://ex.com/setup.exe", L"exe over http");
    CHECK(ClipboardDownloadLink(L"https://ex.com/v.mp4") != L"", L"video");
    CHECK(ClipboardDownloadLink(L"https://ex.com/a.torrent") != L"", L".torrent link");
    CHECK(ClipboardDownloadLink(L"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567") != L"", L"magnet link");
    CHECK(ClipboardDownloadLink(L"https://ex.com/page.html").empty(), L"web page: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com/").empty(), L"no file name: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com").empty(), L"host only: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com/a.zip/").empty(), L"trailing slash: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com/photo.jpg").empty(), L"pictures: no");
    CHECK(ClipboardDownloadLink(L"https://github.com/o/r/blob/main/README.md").empty(), L"README.md: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com/src/app.ts").empty(), L".ts source: no");
    CHECK(ClipboardDownloadLink(L"https://ex.com/a.zip https://ex.com/b.zip").empty(), L"two links: no");
    CHECK(ClipboardDownloadLink(L"see https://ex.com/a.zip").empty(), L"text around a link: no");
    CHECK(ClipboardDownloadLink(L"ftp://ex.com/a.zip").empty(), L"ftp: no (not supported)");
    CHECK(ClipboardDownloadLink(L"https://ex.com/?file=a.zip").empty(), L"extension only in the query: no");
    CHECK(ClipboardDownloadLink(std::wstring(9000, L'a')).empty(), L"huge text: no");
    ClipboardGate g;
    CHECK(g.Offer(L"https://a/x.zip", 1000), L"gate: first time offered");
    CHECK(!g.Offer(L"https://a/x.zip", 1000 + 29999), L"gate: same link within 30 s ignored");
    CHECK(g.Offer(L"https://a/x.zip", 1000 + 30000), L"gate: same link after 30 s offered again");
    CHECK(g.Offer(L"https://a/y.zip", 31001), L"gate: another link offered at once");
    CHECK(!g.Offer(L"", 99999), L"gate: empty never offered");
}

static void MenuRules()
{
    using namespace NdmUi;
    wprintf(L"menu rules\n");
    CHECK(CanRename(NDM_KIND_HTTP, NDM_STATUS_COMPLETED, 10), L"rename: finished HTTP");
    CHECK(CanRename(NDM_KIND_MEDIA, NDM_STATUS_COMPLETED, 10), L"rename: finished media");
    CHECK(!CanRename(NDM_KIND_TORRENT, NDM_STATUS_COMPLETED, 10), L"rename: torrent never");
    CHECK(CanRename(NDM_KIND_HTTP, NDM_STATUS_PAUSED, 0), L"rename: HTTP not started yet");
    CHECK(CanRename(NDM_KIND_HTTP, NDM_STATUS_QUEUED, 0), L"rename: HTTP queued, nothing written");
    CHECK(!CanRename(NDM_KIND_HTTP, NDM_STATUS_PAUSED, 1), L"rename: partly downloaded: no (.ndmpart naming)");
    CHECK(!CanRename(NDM_KIND_HTTP, NDM_STATUS_DOWNLOADING, 0), L"rename: while downloading: no");
    CHECK(!CanRename(NDM_KIND_MEDIA, NDM_STATUS_PAUSED, 0), L"rename: unfinished media: no");
    CHECK(CanRefreshAddress(NDM_KIND_HTTP, NDM_STATUS_ERROR), L"refresh address: failed HTTP");
    CHECK(CanRefreshAddress(NDM_KIND_HTTP, NDM_STATUS_DOWNLOADING), L"refresh address: running HTTP (paused first)");
    CHECK(!CanRefreshAddress(NDM_KIND_HTTP, NDM_STATUS_COMPLETED), L"refresh address: finished: no");
    CHECK(!CanRefreshAddress(NDM_KIND_MEDIA, NDM_STATUS_PAUSED), L"refresh address: media: no");
    CHECK(!CanRefreshAddress(NDM_KIND_TORRENT, NDM_STATUS_PAUSED), L"refresh address: torrent: no");
    CHECK(CanMoveInQueue(NDM_STATUS_QUEUED) && CanMoveInQueue(NDM_STATUS_PAUSED) && CanMoveInQueue(NDM_STATUS_ERROR),
          L"move in queue: waiting items");
    CHECK(!CanMoveInQueue(NDM_STATUS_DOWNLOADING) && !CanMoveInQueue(NDM_STATUS_COMPLETED),
          L"move in queue: not running / finished items");
    CHECK(CanSetCompletionAction(NDM_STATUS_DOWNLOADING) && !CanSetCompletionAction(NDM_STATUS_COMPLETED),
          L"completion action: unfinished only");
    CHECK(IsLeafName(L"a.zip") && !IsLeafName(L"") && !IsLeafName(L"..") && !IsLeafName(L"a\\b") &&
          !IsLeafName(L"c:x") && !IsLeafName(L"a/b"), L"delete with file: only a plain file name");
}

static void Speed()
{
    using namespace NdmUi;
    wprintf(L"speed\n");
    CHECK(SpeedFromInput(L"512", false) == 512u * 1024u, L"512 KB/s");
    CHECK(SpeedFromInput(L"1.5", true) == 1572864u, L"1.5 MB/s");
    CHECK(SpeedFromInput(L"2,5", true) == 2621440u, L"decimal comma");
    CHECK(SpeedFromInput(L"", false) == 0 && SpeedFromInput(L"abc", false) == 0 && SpeedFromInput(L"1.2.3", true) == 0,
          L"invalid -> 0");
    CHECK(SpeedFromInput(L"0", false) == 0, L"0 -> no limit value");
    CHECK(SpeedFromInput(L"0.1", false) == 1024, L"tiny -> at least 1 KB/s");
    std::wstring t; bool mb = false;
    SpeedToInput(5u * 1048576u, t, mb);
    CHECK(t == L"5" && mb, L"5 MB/s shown as 5 MB/s");
    SpeedToInput(700u * 1024u, t, mb);
    CHECK(t == L"700" && !mb, L"700 KB/s shown in KB/s");
    SpeedToInput(1572864u, t, mb);
    CHECK(t == L"1536" && !mb, L"1.5 MB/s shown as 1536 KB/s (exact)");
    CHECK(SpeedLabel(256u * 1024u) == L"256 KB/s" && SpeedLabel(1048576u) == L"1 MB/s" &&
          SpeedLabel(1572864u) == L"1.5 MB/s", L"labels");
}

static void Misc()
{
    using namespace NdmUi;
    wprintf(L"countdown / scheduler / wording\n");
    CHECK(CountdownText(NDM_DONE_SHUTDOWN, 30) == L"Shutting down in 30 s", L"shut down countdown");
    CHECK(CountdownText(NDM_DONE_SLEEP, 5) == L"Going to sleep in 5 s", L"sleep countdown");
    CHECK(CountdownText(NDM_DONE_HIBERNATE, 1) == L"Hibernating in 1 s", L"hibernate countdown");
    CHECK(wcscmp(ActionName(NDM_DONE_NONE), L"Nothing") == 0 && wcscmp(ActionName(NDM_DONE_SHUTDOWN), L"Shut down") == 0,
          L"action names");
    int m = -1;
    CHECK(ParseHhMm(L"07:05", m) && m == 425, L"07:05");
    CHECK(ParseHhMm(L"7:05", m) && m == 425, L"7:05");
    CHECK(ParseHhMm(L"23.59", m) && m == 1439, L"23.59");
    CHECK(!ParseHhMm(L"24:00", m) && !ParseHhMm(L"7:5", m) && !ParseHhMm(L"ab:cd", m) && !ParseHhMm(L"", m) &&
          !ParseHhMm(L"12:60", m), L"invalid times refused");
    CHECK(FormatHhMm(425) == L"07:05" && FormatHhMm(0) == L"00:00", L"time text");
    int d = 0;
    CHECK(ParseIsoDate(L"2026-11-25", d) && d == 20261125, L"2026-11-25");
    CHECK(ParseIsoDate(L"2028-02-29", d) && !ParseIsoDate(L"2026-02-29", d) && !ParseIsoDate(L"2026-13-01", d) &&
          !ParseIsoDate(L"2026-1-5", d) && !ParseIsoDate(L"2026-04-31", d), L"leap years / invalid dates");
    CHECK(FormatIsoDate(20261125) == L"2026-11-25" && FormatIsoDate(0).empty(), L"date text");
    CHECK(ProgressWord(NDM_STATUS_PAUSED, 62, 100) == L"paused at 62%", L"paused at 62%");
    CHECK(ProgressWord(NDM_STATUS_ERROR, 5, 0) == L"failed", L"size unknown: no percentage");
    CHECK(ProgressWord(NDM_STATUS_COMPLETED, 100, 100) == L"complete", L"complete");
    CHECK(RefreshFailText(L"The new address serves a file of 1.2 GB, this download is 1.1 GB.") ==
          L"The new address serves a file of 1.2 GB, this download is 1.1 GB. The old link is kept.",
          L"refresh failure: reason + old link kept");
    CHECK(RefreshFailText(L"Pause the download first") == L"Pause the download first. The old link is kept.",
          L"refresh failure: full stop added");
}

static void ColumnsAndSort()
{
    using namespace NdmUi;
    wprintf(L"columns / sorting\n");
    int w[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    CHECK(ParseColumnWidths(L"170,142,74,100,84,70,76,80", w, 8, 24, 2000) && w[0] == 170 && w[7] == 80,
          L"widths parsed");
    CHECK(FormatColumnWidths(w, 8) == L"170,142,74,100,84,70,76,80", L"widths written back the same");
    int k[3] = { 7, 8, 9 };
    CHECK(!ParseColumnWidths(L"", k, 3, 24, 2000) && k[0] == 7, L"empty: defaults kept");
    CHECK(!ParseColumnWidths(L"100,200", k, 3, 24, 2000) && k[0] == 7, L"too few: defaults kept");
    CHECK(!ParseColumnWidths(L"100,200,300,400", k, 3, 24, 2000) && k[0] == 7, L"too many: defaults kept");
    CHECK(!ParseColumnWidths(L"100,x,300", k, 3, 24, 2000) && !ParseColumnWidths(L"100,,300", k, 3, 24, 2000) &&
          !ParseColumnWidths(L"100,-5,300", k, 3, 24, 2000) && k[0] == 7, L"garbage: defaults kept");
    CHECK(!ParseColumnWidths(L"100,5,300", k, 3, 24, 2000) && !ParseColumnWidths(L"100,99999,300", k, 3, 24, 2000),
          L"out of range: refused");
    CHECK(ParseColumnWidths(L" 100 , 200 ,300", k, 3, 24, 2000) && k[1] == 200, L"spaces tolerated");
    CHECK(ClampSortColumn(-1, 8) == -1 && ClampSortColumn(3, 8) == 3 && ClampSortColumn(8, 8) == -1 &&
          ClampSortColumn(-7, 8) == -1, L"sort column clamped (-1 = queue order)");

    // calm live sorting
    CHECK(ShouldResort(true, true, true, true, true, 100, 99), L"header click: sorts at once, even under the mouse");
    CHECK(ShouldResort(false, false, true, true, true, 100, 99), L"rows added / removed: sorts at once");
    CHECK(ShouldResort(false, true, false, true, false, 100, 99), L"non-live column (name / size): always sorted");
    CHECK(!ShouldResort(false, true, true, false, false, 1000 + kLiveResortMs - 1, 1000), L"live: not before the interval");
    CHECK(ShouldResort(false, true, true, false, false, 1000 + kLiveResortMs, 1000), L"live: after the interval");
    CHECK(!ShouldResort(false, true, true, true, false, 99999, 0), L"live: never while the pointer is over the list");
    CHECK(!ShouldResort(false, true, true, false, true, 99999, 0), L"live: never while a menu is open");
    std::vector<DWORD> prev = { 5, 3, 9, 1 }, ids = { 1, 3, 5, 9 };
    std::vector<size_t> o = OrderLike(prev, ids);
    CHECK(o.size() == 4 && ids[o[0]] == 5 && ids[o[1]] == 3 && ids[o[2]] == 9 && ids[o[3]] == 1,
          L"kept order: rows stay where they were");
    std::vector<DWORD> ids2 = { 7, 1, 3 };
    std::vector<size_t> o2 = OrderLike({ 3, 1 }, ids2);
    CHECK(ids2[o2[0]] == 3 && ids2[o2[1]] == 1 && ids2[o2[2]] == 7, L"kept order: unknown ids last");
    CHECK(SameIdSet({ 1, 2, 3 }, { 3, 1, 2 }) && !SameIdSet({ 1, 2 }, { 1, 3 }) && !SameIdSet({ 1 }, { 1, 1 }),
          L"same rows, order ignored");
}

static void UiStateIni()
{
    wprintf(L"ndm.ini [UI] state\n");
    wchar_t dir[MAX_PATH] = {}, file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"ndu", 0, file);          // never the real %APPDATA% ndm.ini
    std::wstring ini = file;
    NdmUiState fresh;
    fresh.Load(ini);
    CHECK(fresh.columnWidths.empty() && fresh.sortColumn == -1 && fresh.sortAscending && !fresh.trayHintShown,
          L"fresh install: defaults (queue order, tray hint not shown yet)");
    NdmUiState a;
    a.columnWidths = L"200,142,90,100,84,70,76,80";
    a.sortColumn = 5;
    a.sortAscending = false;
    a.trayHintShown = true;
    a.Save(ini);
    NdmUiState b;
    b.Load(ini);
    CHECK(b.columnWidths == a.columnWidths && b.sortColumn == 5 && !b.sortAscending,
          L"widths + sort column / direction survive a restart");
    CHECK(b.trayHintShown, L"\"NDM is still running\" hint: flag persisted (shown once ever)");
    a.sortColumn = -1;
    a.Save(ini);
    b.Load(ini);
    CHECK(b.sortColumn == -1, L"sort column -1 (queue order) round-trips");
    WritePrivateProfileStringW(L"UI", L"SortColumn", L"banana", ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    b.Load(ini);
    CHECK(b.sortColumn == -1, L"broken sort column -> queue order");
    DeleteFileW(ini.c_str());
}

static void SidebarScroll()
{
    using namespace NdmUi;
    wprintf(L"sidebar scrolling\n");
    CHECK(ClampScroll(50, 300, 400) == 0, L"fits: no scrolling");
    CHECK(ClampScroll(-5, 500, 300) == 0 && ClampScroll(150, 500, 300) == 150 && ClampScroll(999, 500, 300) == 200,
          L"offset clamped to [0, content - view]");
    int t = -1, l = -1;
    ScrollThumb(0, 300, 400, 380, 18, t, l);
    CHECK(l == 0, L"fits: no thumb");
    ScrollThumb(0, 600, 300, 300, 18, t, l);
    CHECK(t == 0 && l == 150, L"thumb length = view share of the track");
    ScrollThumb(300, 600, 300, 300, 18, t, l);
    CHECK(t == 150 && t + l == 300, L"scrolled to the end: thumb at the bottom");
    ScrollThumb(0, 100000, 100, 300, 18, t, l);
    CHECK(l == 18, L"minimum thumb length");
    CHECK(ScrollFromThumbDrag(0, 75, 600, 300, 300, 150) == 150 && ScrollFromThumbDrag(0, 9999, 600, 300, 300, 150) == 300 &&
          ScrollFromThumbDrag(100, -9999, 600, 300, 300, 150) == 0, L"thumb drag maps to the offset, clamped");
}

static void AskRules()
{
    using namespace NdmUi;
    wprintf(L"themed question\n");
    // [Replace] [Keep both] [Cancel], default Keep both, Esc = Cancel
    CHECK(AskKeyResult(true, -1, 1, 2, 3) == 1, L"Enter without focus: the default button");
    CHECK(AskKeyResult(true, 0, 1, 2, 3) == 0, L"Enter on a focused button: that button");
    CHECK(AskKeyResult(false, 0, 1, 2, 3) == 2, L"Esc: Cancel");
    CHECK(AskKeyResult(false, 0, 0, -1, 2) == -1, L"Esc without a cancel button: -1");
    CHECK(AskButtonStyle(1, 1, 0, L"Keep both") == ASK_PRIMARY && AskButtonStyle(0, 1, 0, L"Replace") == ASK_DANGER &&
          AskButtonStyle(2, 1, 0, L"Cancel") == ASK_GHOST && AskButtonStyle(0, 1, -1, L"Replace") == ASK_SECONDARY,
          L"styles: default gold, danger red, Cancel flat");
    CHECK(AskButtonStyle(1, 1, -1, L"Cancel") == ASK_PRIMARY, L"a default Cancel is still the gold one");
    // legacy Ask: buttons[0] default and rightmost
    CHECK(AskLegacyPos(0, 3) == 2 && AskLegacyPos(2, 3) == 0 && AskLegacyPos(1, 3) == 1 &&
          AskLegacyPos(AskLegacyPos(1, 2), 2) == 1 && AskLegacyPos(5, 3) == -1, L"legacy Ask: index <-> position");
    auto none = [](const std::wstring&) { return false; };
    auto two  = [](const std::wstring& n) { return n == L"a (2).zip"; };
    CHECK(KeepBothName(L"a.zip", none) == L"a (2).zip", L"keep both: a (2).zip");
    CHECK(KeepBothName(L"a.zip", two) == L"a (3).zip", L"keep both: next free number");
    CHECK(KeepBothName(L"README", none) == L"README (2)" && KeepBothName(L".hidden", none) == L".hidden (2)",
          L"keep both: no extension");
}

static void BrowserText()
{
    using namespace NdmUi;
    wprintf(L"browser setup text\n");
    std::wstring c = BrowserHint(false), f = BrowserHint(true);
    CHECK(c.find(L"Developer mode") != std::wstring::npos && c.find(L"Load unpacked") != std::wstring::npos,
          L"Chrome / Edge: Developer mode + Load unpacked");
    CHECK(f.find(L"Load Temporary Add-on") != std::wstring::npos, L"Firefox: Load Temporary Add-on");
    CHECK(BrowserNote(false) == nullptr, L"Chrome / Edge: no extra line");
    std::wstring n = BrowserNote(true) ? BrowserNote(true) : L"";
    CHECK(n.find(L"temporary add-on") != std::wstring::npos && n.find(L"restarting Firefox") != std::wstring::npos,
          L"Firefox: honest note that it is gone after a restart");
}

// Owner-drawn toggles / painted captions as screen readers see them (MSAA)
static void Accessibility()
{
    wprintf(L"accessibility\n");
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 200, 100, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    HWND tg = NdmTheme::MakeBtn(parent, 10, L"Start with Windows", GLY_NONE, true);
    HWND ed = NdmTheme::MakeEdit(parent, 11);
    NdmTheme::SetAccName(ed, L"URL");
    CHECK(tg && ed, L"controls created");
    SendMessageW(tg, BM_SETCHECK, BST_CHECKED, 0);
    CHECK(SendMessageW(tg, BM_GETCHECK, 0, 0) == BST_CHECKED, L"toggle: BM_GETCHECK = checked");
    wchar_t txt[64] = {};
    GetWindowTextW(tg, txt, 64);
    CHECK(wcscmp(txt, L"Start with Windows") == 0, L"toggle: window text is the label");

    IAccessible* acc = nullptr;
    VARIANT self; VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
    bool role = false, checked = false, name = false, unchecked = false, edName = false;
    if (SUCCEEDED(AccessibleObjectFromWindow(tg, (DWORD)OBJID_CLIENT, __uuidof(IAccessible), (void**)&acc)) && acc)
    {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(acc->get_accRole(self, &v)) && v.vt == VT_I4) role = v.lVal == ROLE_SYSTEM_CHECKBUTTON;
        VariantClear(&v);
        if (SUCCEEDED(acc->get_accState(self, &v)) && v.vt == VT_I4) checked = (v.lVal & STATE_SYSTEM_CHECKED) != 0;
        VariantClear(&v);
        SendMessageW(tg, BM_SETCHECK, BST_UNCHECKED, 0);
        if (SUCCEEDED(acc->get_accState(self, &v)) && v.vt == VT_I4) unchecked = (v.lVal & STATE_SYSTEM_CHECKED) == 0;
        VariantClear(&v);
        BSTR b = nullptr;
        if (SUCCEEDED(acc->get_accName(self, &b)) && b) { name = wcscmp(b, L"Start with Windows") == 0; SysFreeString(b); }
        acc->Release();
    }
    acc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(ed, (DWORD)OBJID_CLIENT, __uuidof(IAccessible), (void**)&acc)) && acc)
    {
        BSTR b = nullptr;
        if (SUCCEEDED(acc->get_accName(self, &b)) && b) { edName = wcscmp(b, L"URL") == 0; SysFreeString(b); }
        acc->Release();
    }
    CHECK(role, L"toggle: role = check box");
    CHECK(checked && unchecked, L"toggle: checked state follows BM_SETCHECK");
    CHECK(name, L"toggle: accessible name = label");
    CHECK(edName, L"edit with a painted caption: accessible name");
    DestroyWindow(parent);
    NdmTheme::Shutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

int wmain()
{
    Dates();
    Search();
    Clipboard();
    MenuRules();
    Speed();
    Misc();
    ColumnsAndSort();
    UiStateIni();
    SidebarScroll();
    AskRules();
    BrowserText();
    Accessibility();
    wprintf(L"%s: %d failed\n", g_fail ? L"FAILED" : L"OK", g_fail);
    return g_fail;
}
