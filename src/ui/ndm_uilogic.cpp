// ndm_uilogic.cpp -- pure UI rules (see ndm_uilogic.h)
// Invented with Care By Devnetra Consultancy

#include "ndm_uilogic.h"
#include "ndm_theme.h"
#include "../common/ndm_common.h"

#include <cwchar>
#include <cwctype>
#include <algorithm>

namespace
{
    std::wstring Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n\"'<>");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_last_not_of(L" \t\r\n\"'<>");
        return s.substr(a, b - a + 1);
    }

    std::wstring Lower(std::wstring s)
    {
        for (auto& ch : s) ch = (wchar_t)towlower(ch);
        return s;
    }
}

// ===========================================================================
// Dates
// ===========================================================================
UINT64 NdmUi::ListDateFt(int status, UINT64 addedFt, UINT64 completedFt)
{
    return status == NDM_STATUS_COMPLETED ? completedFt : addedFt;
}

std::wstring NdmUi::FormatLocalDate(const SYSTEMTIME& st, const wchar_t* locale)
{
    wchar_t d[64] = {}, t[64] = {};
    if (!GetDateFormatEx(locale, 0, &st, L"d MMM yyyy", d, 64, nullptr)) return L"";
    if (!GetTimeFormatEx(locale, TIME_NOSECONDS, &st, nullptr, t, 64)) return d;
    return std::wstring(d) + L", " + t;
}

std::wstring NdmUi::FormatListDate(UINT64 utcFt)
{
    if (!utcFt) return L"";
    FILETIME ft;
    ft.dwLowDateTime  = (DWORD)(utcFt & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)(utcFt >> 32);
    SYSTEMTIME utc, loc;
    if (!FileTimeToSystemTime(&ft, &utc)) return L"";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) return L"";
    return FormatLocalDate(loc, nullptr);
}

// ===========================================================================
// Search
// ===========================================================================
bool NdmUi::MatchesSearch(const std::wstring& name, const std::wstring& url, const std::wstring& query)
{
    if (query.empty()) return true;
    auto in = [&](const std::wstring& hay)
    {
        if (hay.size() < query.size()) return false;
        return FindStringOrdinal(FIND_FROMSTART, hay.c_str(), (int)hay.size(),
                                 query.c_str(), (int)query.size(), TRUE) >= 0;
    };
    return in(name) || in(url);
}

// ===========================================================================
// Clipboard
// ===========================================================================
std::wstring NdmUi::ClipboardDownloadLink(const std::wstring& text)
{
    if (text.size() > 8192) return L"";
    std::wstring s = Trim(text);
    if (s.empty() || s.find_first_of(L" \t\r\n") != std::wstring::npos) return L"";
    if (_wcsnicmp(s.c_str(), L"magnet:?", 8) == 0)
        return NdmIsNetOrMagnetUrl(s) && s.size() > 20 ? s : L"";
    if (!NdmIsHttpUrl(s)) return L"";

    // path only: no query / fragment, and past the host
    size_t scheme = s.find(L"://");
    if (scheme == std::wstring::npos) return L"";
    size_t pathAt = s.find(L'/', scheme + 3);
    if (pathAt == std::wstring::npos) return L"";
    size_t end = s.find_first_of(L"?#", pathAt);
    std::wstring path = s.substr(pathAt, end == std::wstring::npos ? std::wstring::npos : end - pathAt);
    size_t slash = path.find_last_of(L'/');
    std::wstring leaf = path.substr(slash + 1);
    size_t dot = leaf.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= leaf.size()) return L"";
    std::wstring ext = Lower(leaf.substr(dot + 1));

    if (ext == L"torrent") return s;
    // The category lists of the main window, minus what people copy to
    // share or read rather than to download: pictures, and text formats that
    // are usually web pages (README.md, .txt, .csv) or source files (.ts).
    static const wchar_t* skip[] = { L"md", L"txt", L"csv", L"ts" };
    for (const wchar_t* k : skip) if (ext == k) return L"";
    int cat = NdmTheme::CategoryOfFile(L"x." + ext);
    if (cat == CAT_GENERAL || cat == CAT_PICTURES) return L"";
    return s;
}

bool NdmUi::ClipboardGate::Offer(const std::wstring& url, ULONGLONG nowMs)
{
    if (url.empty()) return false;
    if (url == m_last && nowMs - m_at < 30000) return false;
    m_last = url;
    m_at = nowMs;
    return true;
}

// ===========================================================================
// Menu state rules
// ===========================================================================
bool NdmUi::CanRename(int kind, int status, UINT64 downloaded)
{
    if (kind == NDM_KIND_TORRENT) return false;
    if (status == NDM_STATUS_COMPLETED) return true;
    if (status == NDM_STATUS_DOWNLOADING) return false;
    return kind == NDM_KIND_HTTP && downloaded == 0;
}

bool NdmUi::CanRefreshAddress(int kind, int status)
{
    return kind == NDM_KIND_HTTP && status != NDM_STATUS_COMPLETED;
}

bool NdmUi::CanMoveInQueue(int status)
{
    return status == NDM_STATUS_QUEUED || status == NDM_STATUS_PAUSED || status == NDM_STATUS_ERROR;
}

bool NdmUi::CanSetCompletionAction(int status)
{
    return status != NDM_STATUS_COMPLETED;
}

bool NdmUi::IsLeafName(const std::wstring& n)
{
    if (n.empty() || n == L"." || n == L"..") return false;
    return n.find_first_of(L"\\/:") == std::wstring::npos;
}

// ===========================================================================
// Speed
// ===========================================================================
DWORD NdmUi::SpeedFromInput(const std::wstring& textIn, bool mega)
{
    std::wstring t = Trim(textIn);
    if (t.empty() || t.size() > 12) return 0;
    for (auto& ch : t) if (ch == L',') ch = L'.';
    int dots = 0;
    for (wchar_t ch : t)
    {
        if (ch == L'.') { if (++dots > 1) return 0; }
        else if (ch < L'0' || ch > L'9') return 0;
    }
    double v = _wtof(t.c_str());
    double bps = v * (mega ? 1048576.0 : 1024.0);
    if (bps < 1024.0) return v > 0 ? 1024 : 0;       // at least 1 KB/s
    if (bps > 4000000.0 * 1024.0) bps = 4000000.0 * 1024.0;
    return (DWORD)(bps + 0.5);
}

void NdmUi::SpeedToInput(DWORD bps, std::wstring& text, bool& mega)
{
    wchar_t b[32];
    if (bps >= 1048576 && bps % 1048576 == 0)
    {
        mega = true;
        swprintf_s(b, L"%lu", (unsigned long)(bps / 1048576));
    }
    else
    {
        mega = false;
        swprintf_s(b, L"%lu", (unsigned long)((bps + 512) / 1024));
    }
    text = bps ? b : L"";
}

std::wstring NdmUi::SpeedLabel(DWORD bps)
{
    wchar_t b[32];
    if (bps >= 1048576)
    {
        double m = bps / 1048576.0;
        if (bps % 1048576 == 0) swprintf_s(b, L"%.0f MB/s", m);
        else                    swprintf_s(b, L"%.1f MB/s", m);
    }
    else
        swprintf_s(b, L"%lu KB/s", (unsigned long)((bps + 512) / 1024));
    return b;
}

// ===========================================================================
// Completion action
// ===========================================================================
const wchar_t* NdmUi::ActionName(int a)
{
    switch (a)
    {
    case NDM_DONE_SLEEP:     return L"Sleep";
    case NDM_DONE_HIBERNATE: return L"Hibernate";
    case NDM_DONE_SHUTDOWN:  return L"Shut down";
    default:                 return L"Nothing";
    }
}

std::wstring NdmUi::CountdownText(int action, int seconds)
{
    const wchar_t* what = action == NDM_DONE_SLEEP     ? L"Going to sleep"
                        : action == NDM_DONE_HIBERNATE ? L"Hibernating"
                        :                                L"Shutting down";
    if (seconds < 0) seconds = 0;
    return std::wstring(what) + L" in " + std::to_wstring(seconds) + L" s";
}

// ===========================================================================
// Scheduler fields
// ===========================================================================
bool NdmUi::ParseHhMm(const std::wstring& sIn, int& minutes)
{
    std::wstring s = Trim(sIn);
    size_t c = s.find_first_of(L":.");
    if (c == std::wstring::npos || c == 0 || c > 2 || s.size() - c - 1 != 2) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != c && (s[i] < L'0' || s[i] > L'9')) return false;
    int h = _wtoi(s.substr(0, c).c_str()), m = _wtoi(s.substr(c + 1).c_str());
    if (h > 23 || m > 59) return false;
    minutes = h * 60 + m;
    return true;
}

std::wstring NdmUi::FormatHhMm(int minutes)
{
    if (minutes < 0 || minutes > 1439) minutes = 0;
    wchar_t b[8];
    swprintf_s(b, L"%02d:%02d", minutes / 60, minutes % 60);
    return b;
}

bool NdmUi::ParseIsoDate(const std::wstring& sIn, int& out)
{
    std::wstring s = Trim(sIn);
    if (s.size() != 10 || s[4] != L'-' || s[7] != L'-') return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 4 && i != 7 && (s[i] < L'0' || s[i] > L'9')) return false;
    int y = _wtoi(s.substr(0, 4).c_str()), m = _wtoi(s.substr(5, 2).c_str()), d = _wtoi(s.substr(8, 2).c_str());
    if (y < 2000 || y > 2199 || m < 1 || m > 12 || d < 1) return false;
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int maxd = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) maxd = 29;
    if (d > maxd) return false;
    out = y * 10000 + m * 100 + d;
    return true;
}

std::wstring NdmUi::FormatIsoDate(int v)
{
    if (v <= 0) return L"";
    wchar_t b[16];
    swprintf_s(b, L"%04d-%02d-%02d", v / 10000, (v / 100) % 100, v % 100);
    return b;
}

// ===========================================================================
// Wording
// ===========================================================================
const wchar_t* NdmUi::StatusWord(int status)
{
    switch (status)
    {
    case NDM_STATUS_COMPLETED:   return L"complete";
    case NDM_STATUS_DOWNLOADING: return L"downloading";
    case NDM_STATUS_QUEUED:      return L"queued";
    case NDM_STATUS_PAUSED:      return L"paused";
    default:                     return L"failed";
    }
}

std::wstring NdmUi::ProgressWord(int status, UINT64 done, UINT64 total)
{
    std::wstring w = StatusWord(status);
    if (status == NDM_STATUS_COMPLETED || !total) return w;
    if (done > total) done = total;
    int pct = (int)(done * 100 / total);
    return w + L" at " + std::to_wstring(pct) + L"%";
}

std::wstring NdmUi::RefreshFailText(const std::wstring& coreMsg)
{
    std::wstring m = coreMsg.empty() ? std::wstring(L"The new link could not be used.") : coreMsg;
    if (!m.empty() && m.back() != L'.') m += L'.';
    return m + L" The old link is kept.";
}

// ===========================================================================
// List columns / sorting
// ===========================================================================
bool NdmUi::ParseColumnWidths(const std::wstring& s, int* out, int n, int minW, int maxW)
{
    if (!out || n <= 0 || s.empty() || s.size() > 512) return false;
    std::vector<int> v;
    size_t i = 0;
    while (i <= s.size())
    {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos) j = s.size();
        std::wstring part = s.substr(i, j - i);
        size_t a = part.find_first_not_of(L" \t");
        size_t b = part.find_last_not_of(L" \t");
        if (a == std::wstring::npos) return false;
        part = part.substr(a, b - a + 1);
        if (part.size() > 5) return false;
        int x = 0;
        for (wchar_t ch : part)
        {
            if (ch < L'0' || ch > L'9') return false;
            x = x * 10 + (ch - L'0');
        }
        if (x < minW || x > maxW) return false;
        v.push_back(x);
        if ((int)v.size() > n) return false;
        i = j + 1;
    }
    if ((int)v.size() != n) return false;
    for (int k = 0; k < n; ++k) out[k] = v[k];
    return true;
}

std::wstring NdmUi::FormatColumnWidths(const int* w, int n)
{
    std::wstring s;
    for (int k = 0; w && k < n; ++k)
    {
        if (k) s += L',';
        s += std::to_wstring(w[k]);
    }
    return s;
}

int NdmUi::ClampSortColumn(int col, int count)
{
    return (col >= -1 && col < count) ? col : -1;
}

bool NdmUi::ShouldResort(bool force, bool sameRows, bool liveColumn, bool pointerOverList,
                         bool menuOpen, ULONGLONG nowMs, ULONGLONG lastSortMs)
{
    if (force || !sameRows || !liveColumn) return true;
    if (pointerOverList || menuOpen) return false;
    return nowMs < lastSortMs || nowMs - lastSortMs >= kLiveResortMs;
}

std::vector<size_t> NdmUi::OrderLike(const std::vector<DWORD>& prevOrder, const std::vector<DWORD>& ids)
{
    std::vector<size_t> rank(ids.size(), (size_t)-1);
    {
        // id -> position in prevOrder (small lists: a sorted copy is enough)
        std::vector<std::pair<DWORD, size_t>> pos;
        pos.reserve(prevOrder.size());
        for (size_t i = 0; i < prevOrder.size(); ++i) pos.push_back({ prevOrder[i], i });
        std::sort(pos.begin(), pos.end());
        for (size_t i = 0; i < ids.size(); ++i)
        {
            auto it = std::lower_bound(pos.begin(), pos.end(), std::make_pair(ids[i], (size_t)0));
            if (it != pos.end() && it->first == ids[i]) rank[i] = it->second;
        }
    }
    std::vector<size_t> out(ids.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = i;
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) { return rank[a] < rank[b]; });
    return out;
}

bool NdmUi::SameIdSet(std::vector<DWORD> a, std::vector<DWORD> b)
{
    if (a.size() != b.size()) return false;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// ===========================================================================
// Sidebar scrolling
// ===========================================================================
int NdmUi::ClampScroll(int pos, int content, int view)
{
    int maxPos = content - view;
    if (maxPos <= 0 || pos <= 0) return 0;
    return pos > maxPos ? maxPos : pos;
}

void NdmUi::ScrollThumb(int pos, int content, int view, int trackLen, int minThumb, int& top, int& len)
{
    top = 0; len = 0;
    if (content <= view || view <= 0 || trackLen <= 0) return;
    len = (int)((long long)trackLen * view / content);
    if (len < minThumb) len = minThumb;
    if (len > trackLen) len = trackLen;
    int range = content - view;
    pos = ClampScroll(pos, content, view);
    top = (int)((long long)(trackLen - len) * pos / range);
}

int NdmUi::ScrollFromThumbDrag(int startPos, int dy, int content, int view, int trackLen, int thumbLen)
{
    int free_ = trackLen - thumbLen;
    if (content <= view || free_ <= 0) return 0;
    long long p = startPos + (long long)dy * (content - view) / free_;
    if (p < 0) p = 0;
    return ClampScroll((int)(p > 0x7FFFFFFF ? 0x7FFFFFFF : p), content, view);
}

// ===========================================================================
// Themed question
// ===========================================================================
int NdmUi::AskKeyResult(bool enter, int focused, int def, int cancel, int count)
{
    if (count <= 0) return -1;
    if (!enter) return (cancel >= 0 && cancel < count) ? cancel : -1;
    if (focused >= 0 && focused < count) return focused;
    return (def >= 0 && def < count) ? def : 0;
}

NdmUi::AskStyle NdmUi::AskButtonStyle(int i, int def, int danger, const std::wstring& label)
{
    if (i == def) return ASK_PRIMARY;
    if (i == danger) return ASK_DANGER;
    if (label == L"Cancel" || label == L"Close") return ASK_GHOST;
    return ASK_SECONDARY;
}

int NdmUi::AskLegacyPos(int index, int count)
{
    return (index >= 0 && index < count) ? count - 1 - index : -1;
}

// ===========================================================================
// Browser setup
// ===========================================================================
const wchar_t* NdmUi::BrowserHint(bool firefox)
{
    return firefox
        ? L"about:debugging \u2192 This Firefox \u2192 Load Temporary Add-on \u2192 manifest.json"
        : L"Extensions page: turn on Developer mode \u2192 Load unpacked \u2192 this folder";
}

const wchar_t* NdmUi::BrowserNote(bool firefox)
{
    return firefox
        ? L"Firefox removes a temporary add-on when it closes; load it again after restarting Firefox."
        : nullptr;
}
