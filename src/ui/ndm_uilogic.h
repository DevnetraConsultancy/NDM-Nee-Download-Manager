// ndm_uilogic.h -- small pure UI rules (no windows): date text, search
// matching, clipboard link classification, menu state rules, speed / time
// parsing. Unit-tested by build_p1/tests/uitest.cpp.
// Invented with Care By Devnetra Consultancy
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace NdmUi
{
    // ---- dates -----------------------------------------------------------
    // The date the list shows: completion for finished items, otherwise the
    // time the item was added. 0 = unknown (shown blank).
    UINT64       ListDateFt(int status, UINT64 addedFt, UINT64 completedFt);
    // "24 Nov 2026, 16:52": day, the locale's short month name and year,
    // then the locale's time without seconds. locale = nullptr: user's.
    std::wstring FormatLocalDate(const SYSTEMTIME& local, const wchar_t* locale);
    // UTC FILETIME value -> local time text, "" for 0.
    std::wstring FormatListDate(UINT64 utcFt);

    // ---- search ----------------------------------------------------------
    // Case-insensitive substring of name or url; empty query matches all.
    bool         MatchesSearch(const std::wstring& name, const std::wstring& url,
                               const std::wstring& query);

    // ---- clipboard -------------------------------------------------------
    // A single http(s) link whose path ends in a downloadable extension, or
    // a magnet link: returns it trimmed, else "".
    std::wstring ClipboardDownloadLink(const std::wstring& text);
    // Same link again within 30 s is not offered twice.
    class ClipboardGate
    {
    public:
        bool Offer(const std::wstring& url, ULONGLONG nowMs);
    private:
        std::wstring m_last;
        ULONGLONG    m_at = 0;
    };

    // ---- menu state rules (status = NDM_STATUS_*, kind = NDM_KIND_*) -----
    // Completed (not torrent), or an HTTP item that has no bytes yet.
    bool         CanRename(int kind, int status, UINT64 downloaded);
    // HTTP and not finished.
    bool         CanRefreshAddress(int kind, int status);
    // Waiting items (queued / paused / failed): order decides what starts next.
    bool         CanMoveInQueue(int status);
    // Not finished yet.
    bool         CanSetCompletionAction(int status);
    // File name usable as one path element (no separators, not "." / "..").
    bool         IsLeafName(const std::wstring& name);

    // ---- speed -----------------------------------------------------------
    // "512" + KB/s -> 524288; "1.5" + MB/s -> 1572864. 0 = invalid / zero.
    DWORD        SpeedFromInput(const std::wstring& text, bool megabytes);
    // Inverse for showing a limit in an edit: whole MB/s when exact (and
    // >= 1 MB/s), else KB/s.
    void         SpeedToInput(DWORD bps, std::wstring& text, bool& megabytes);
    // "256 KB/s", "1 MB/s", "1.5 MB/s"
    std::wstring SpeedLabel(DWORD bps);

    // ---- completion action ----------------------------------------------
    // "Shutting down in 30 s", "Going to sleep in 5 s", "Hibernating in 1 s"
    std::wstring CountdownText(int action, int seconds);
    const wchar_t* ActionName(int action);        // "Nothing", "Sleep", ...

    // ---- scheduler fields ------------------------------------------------
    bool         ParseHhMm(const std::wstring& s, int& minutes);   // "7:05", "23:59"
    std::wstring FormatHhMm(int minutes);                          // "07:05"
    bool         ParseIsoDate(const std::wstring& s, int& yyyymmdd); // "2026-11-25"
    std::wstring FormatIsoDate(int yyyymmdd);

    // ---- wording ---------------------------------------------------------
    // "paused at 62%", "failed at 3%", "paused" (size unknown)
    std::wstring ProgressWord(int status, UINT64 done, UINT64 total);
    // "complete", "downloading", "queued", "paused", "failed"
    const wchar_t* StatusWord(int status);
    // A failed refresh in plain words: the core reason + "The old link is kept."
    std::wstring RefreshFailText(const std::wstring& coreMsg);

    // ---- list columns / sorting (main window) ----------------------------
    // ndm.ini [UI] ColumnWidths: "170,142,74,..." (96-dpi px). Parse fills
    // out[0..n) only when the text holds exactly n numbers, each within
    // [minW, maxW]; otherwise false and out is left untouched (defaults).
    bool         ParseColumnWidths(const std::wstring& s, int* out, int n, int minW, int maxW);
    std::wstring FormatColumnWidths(const int* w, int n);
    // Sort column read from ndm.ini: -1 (queue order) .. count-1, else -1.
    int          ClampSortColumn(int col, int count);
    // Live columns (status / progress / speed / time left change on every
    // refresh) are re-sorted at most every kLiveResortMs and never while the
    // pointer is over the list or a menu is open, so rows do not move under
    // the mouse. A header click (force), a changed set of rows or a column
    // whose values do not tick sorts at once.
    const ULONGLONG kLiveResortMs = 3000;
    bool         ShouldResort(bool force, bool sameRows, bool liveColumn, bool pointerOverList,
                              bool menuOpen, ULONGLONG nowMs, ULONGLONG lastSortMs);
    // Positions into ids, ordered like prevOrder; ids that are not in
    // prevOrder follow in their own order. Used to keep the shown order.
    std::vector<size_t> OrderLike(const std::vector<DWORD>& prevOrder, const std::vector<DWORD>& ids);
    // Same downloads (order ignored)?
    bool         SameIdSet(std::vector<DWORD> a, std::vector<DWORD> b);

    // ---- sidebar scrolling -------------------------------------------------
    // Scroll offset clamped to [0, content - view] (0 when everything fits).
    int          ClampScroll(int pos, int content, int view);
    // Slim scrollbar thumb inside a track of trackLen px: top offset + length
    // (len = 0 when everything fits).
    void         ScrollThumb(int pos, int content, int view, int trackLen, int minThumb,
                             int& top, int& len);
    // Thumb dragged by dy px from the scroll offset startPos.
    int          ScrollFromThumbDrag(int startPos, int dy, int content, int view, int trackLen, int thumbLen);

    // ---- themed question (NdmDialogs::AskEx) --------------------------------
    enum AskStyle { ASK_PRIMARY, ASK_SECONDARY, ASK_GHOST, ASK_DANGER };
    // Enter: the focused button (focused >= 0) else the default one.
    // Esc / close box: the cancel button (-1 when there is none).
    int          AskKeyResult(bool enter, int focused, int def, int cancel, int count);
    // Default = gold; danger = red outline; "Cancel" / "Close" = flat.
    AskStyle     AskButtonStyle(int i, int def, int danger, const std::wstring& label);
    // NdmDialogs::Ask: buttons[0] is the default and rightmost. Index <->
    // left-to-right position (the mapping is its own inverse).
    int          AskLegacyPos(int index, int count);
    // "name (2).ext" .. first name that exists() says is free; "" if none.
    template <class Exists>
    std::wstring KeepBothName(const std::wstring& name, Exists exists)
    {
        std::wstring base = name, ext;
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
        for (int i = 2; i < 10000; ++i)
        {
            std::wstring cand = base + L" (" + std::to_wstring(i) + L")" + ext;
            if (!exists(cand)) return cand;
        }
        return std::wstring();
    }

    // ---- browser setup ---------------------------------------------------------
    // One-line instruction for the unpacked extension, and an optional
    // second line (Firefox: temporary add-ons do not survive a restart).
    const wchar_t* BrowserHint(bool firefox);
    const wchar_t* BrowserNote(bool firefox);    // nullptr = none
}
