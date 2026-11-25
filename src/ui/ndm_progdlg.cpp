// ndm_progdlg.cpp -- per-download progress window (modeless, compact)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Reads a consistent snapshot of the download on its own timer; closes itself
// if the download is removed. Never keeps an NdmDownload pointer.
//
// Layout (96-dpi units, client 440 x 262):
//   [Download status] [Speed Limiter] [Options on completion]   (tab strip)
//   Download status:
//     file name
//     Status / File size / Downloaded / Transfer rate / Time left / Resume
//     [==========progress==========]
//     [::::::::connections:::::::::]
//   Speed Limiter:
//     [o] Limit this download   [ 512 ] [KB/s][MB/s]
//     Priority                  [High][Normal][Low]
//   Options on completion:
//     When this download finishes
//     [Nothing][Sleep][Hibernate][Shut down]
//   ------------------------------------------------
//                                  [Pause] [Cancel]
//
// Tabs: click, Ctrl+Tab / Ctrl+Shift+Tab anywhere, or Left / Right / Home /
// End while the tab strip has the focus (it is a tab stop).

#include "ndm_progdlg.h"
#include "ndm_theme.h"
#include "ndm_uilogic.h"
#include "ndm_power.h"
#include "ndm_mainwnd.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"

#include <map>
#include <vector>
#include <shellapi.h>

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass    = L"NDMProgressWnd";
    const wchar_t* kTabClass = L"NDMProgressTabs";
    const int kW = 440, kTabH = 30, kH = 232 + kTabH;
    const int kM = 14;
    const int kRowY = 38 + kTabH, kRowH = 19, kRows = 6, kKeyW = 108;
    const int kBarY = 160 + kTabH, kBarH = 10, kSegY = 176 + kTabH, kSegH = 4;
    const int kFooterY = 190 + kTabH;
    const int kCtlX = 178;                      // value column on the option tabs
    const UINT kTimer = 1;
    const UINT kApplyTimer = 2;                 // typing in the limit field: apply after a pause
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);
    const COLORREF kTabBg  = RGB(21, 23, 28);

    enum
    {
        IDC_PAUSE = 400, IDC_CANCELDL, IDC_OPEN, IDC_OPENDIR, IDC_CLOSE, IDC_TABS,
        IDC_LIMIT, IDC_LIMITEDIT, IDC_UNIT_KB, IDC_UNIT_MB,
        IDC_PRIO0 = 420,                        // + NDM_PRIO_*
        IDC_ACT0  = 430                         // + NDM_DONE_*
    };
    enum { TAB_STATUS, TAB_SPEED, TAB_DONE, TAB_COUNT };
    const wchar_t* kTabName[TAB_COUNT] = { L"Download status", L"Speed Limiter", L"Options on completion" };

    std::map<DWORD, HWND> g_wins;

    struct Ctx
    {
        HWND      hwnd = nullptr, owner = nullptr;
        DWORD     id = 0;
        NdmDlgRes res;
        NdmDownload snap;
        bool      have = false;
        int       lastState = -1;     // 0 active, 1 done, 2 error
        int       phase = 0;
        int       tab = TAB_STATUS, hotTab = -1;
        RECT      tabRc[TAB_COUNT] = {};
        bool      unitMB = false;
        DWORD     shownLimit = 0;     // limit the speed controls show
        int       setting = 0;        // >0 while text is set programmatically
        HWND hPause = nullptr, hCancel = nullptr,
             hOpen = nullptr, hOpenDir = nullptr, hClose = nullptr, hTabs = nullptr,
             hLimit = nullptr, hLimitEdit = nullptr, hKB = nullptr, hMB = nullptr;
        HWND hPrio[3] = {};
        HWND hAct[4] = {};
        RECT rcLimit{};
        int S(int v) const { return res.S(v); }
    };

    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    int StateOf(NdmStatus s)
    {
        if (s == NDM_STATUS_COMPLETED) return 1;
        if (s == NDM_STATUS_ERROR)     return 2;
        return 0;
    }

    int TextW(HFONT f, const wchar_t* s)
    {
        HDC dc = GetDC(nullptr);
        SIZE sz = MeasureText(dc, s, f);
        ReleaseDC(nullptr, dc);
        return sz.cx;
    }

    bool IsOn(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

    // ---------------------------------------------------------------------
    // Tab strip (child window; state lives in the parent's Ctx)
    // ---------------------------------------------------------------------
    void LayoutTabs(Ctx* c)
    {
        int x = c->S(kM - 6);
        for (int i = 0; i < TAB_COUNT; ++i)
        {
            int w = TextW(c->res.bold, kTabName[i]) + c->S(24);
            c->tabRc[i] = { x, 0, x + w, c->S(kTabH) };
            x += w + c->S(2);
        }
    }

    void SelectTab(Ctx* c, int t);

    LRESULT CALLBACK TabProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        Ctx* c = Get(GetParent(h));
        if (!c) return DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            switch (w)
            {
            case VK_LEFT:  SelectTab(c, (c->tab + TAB_COUNT - 1) % TAB_COUNT); return 0;
            case VK_RIGHT: SelectTab(c, (c->tab + 1) % TAB_COUNT); return 0;
            case VK_HOME:  SelectTab(c, 0); return 0;
            case VK_END:   SelectTab(c, TAB_COUNT - 1); return 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        {
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            for (int i = 0; i < TAB_COUNT; ++i)
                if (PtInRect(&c->tabRc[i], pt)) { SetFocus(h); SelectTab(c, i); break; }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            int hot = -1;
            for (int i = 0; i < TAB_COUNT; ++i) if (PtInRect(&c->tabRc[i], pt)) hot = i;
            if (hot != c->hotTab)
            {
                c->hotTab = hot;
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (c->hotTab >= 0) { c->hotTab = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_SETCURSOR:
        {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            for (int i = 0; i < TAB_COUNT; ++i)
                if (PtInRect(&c->tabRc[i], pt)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT cl; GetClientRect(h, &cl);
            {
                NdmMemDC mem(hdc, cl);
                HDC dc = mem.ok() ? mem.dc : hdc;
                Fill(dc, cl, kTabBg);
                HLine(dc, 0, cl.right, cl.bottom - 1, NDM_CLR_BORDER);
                bool focus = GetFocus() == h;
                for (int i = 0; i < TAB_COUNT; ++i)
                {
                    RECT t = c->tabRc[i];
                    bool sel = i == c->tab, hot = i == c->hotTab;
                    if (sel)
                    {
                        RECT ul = { t.left + c->S(8), t.bottom - c->S(2) - 1, t.right - c->S(8), t.bottom - 1 };
                        Fill(dc, ul, NDM_CLR_GOLD);
                    }
                    if (sel && focus)
                    {
                        RECT fr = { t.left + c->S(2), t.top + c->S(4), t.right - c->S(2), t.bottom - c->S(5) };
                        RoundFrame(dc, fr, c->S(4), NDM_CLR_GOLD, 1.0f, 160);
                    }
                    COLORREF fg = sel ? NDM_CLR_GOLD_LIGHT : (hot ? RGB(240, 240, 244) : NDM_CLR_FG_DIM);
                    Text(dc, kTabName[i], t, fg, sel ? c->res.bold : c->res.ui,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            EndPaint(h, &ps);
            return 0;
        }
        }
        return DefWindowProcW(h, m, w, l);
    }

    // ---------------------------------------------------------------------
    // Pages
    // ---------------------------------------------------------------------
    bool Done(Ctx* c) { return c->have && c->snap.status == NDM_STATUS_COMPLETED; }

    void ShowPage(Ctx* c)
    {
        auto vis = [](HWND h, bool v) { ShowWindow(h, v ? SW_SHOW : SW_HIDE); };
        bool sp = c->tab == TAB_SPEED, dn = c->tab == TAB_DONE;
        vis(c->hLimit, sp); vis(c->hLimitEdit, sp); vis(c->hKB, sp); vis(c->hMB, sp);
        for (HWND b : c->hPrio) vis(b, sp);
        for (HWND b : c->hAct) vis(b, dn);
        bool done = Done(c);
        EnableWindow(c->hLimit, !done);
        EnableWindow(c->hLimitEdit, !done && IsOn(c->hLimit));
        EnableWindow(c->hKB, !done && IsOn(c->hLimit));
        EnableWindow(c->hMB, !done && IsOn(c->hLimit));
        for (HWND b : c->hPrio) EnableWindow(b, !done);
        for (int a = 0; a < 4; ++a) EnableWindow(c->hAct[a], !done && NdmPower::Available(a));
        InvalidateRect(c->hwnd, nullptr, FALSE);
    }

    void SelectTab(Ctx* c, int t)
    {
        if (t < 0 || t >= TAB_COUNT) return;
        if (t != c->tab)
        {
            c->tab = t;
            ShowPage(c);
        }
        InvalidateRect(c->hTabs, nullptr, FALSE);
    }

    // Speed controls <- the download's current limit
    void LoadLimit(Ctx* c)
    {
        DWORD lim = c->snap.speedLimit;
        c->shownLimit = lim;
        std::wstring t;
        bool mb = c->unitMB;
        if (lim) NdmUi::SpeedToInput(lim, t, mb);
        else if (GetWindowTextLengthW(c->hLimitEdit) == 0) { t = L"512"; mb = false; }
        else t = GetText(c->hLimitEdit);
        c->unitMB = mb;
        ++c->setting;
        SetWindowTextW(c->hLimitEdit, t.c_str());
        --c->setting;
        SendMessageW(c->hLimit, BM_SETCHECK, lim ? BST_CHECKED : BST_UNCHECKED, 0);
        InvalidateRect(c->hKB, nullptr, FALSE);
        InvalidateRect(c->hMB, nullptr, FALSE);
    }

    // The download <- speed controls (live)
    void ApplyLimit(Ctx* c)
    {
        DWORD v = 0;
        if (IsOn(c->hLimit)) v = NdmUi::SpeedFromInput(GetText(c->hLimitEdit), c->unitMB);
        if (IsOn(c->hLimit) && v == 0) return;            // incomplete entry: keep the current limit
        if (v != c->snap.speedLimit) NdmQueue::GetInstance().SetDownloadSpeedLimit(c->id, v);
        c->snap.speedLimit = v;
        c->shownLimit = v;
        NdmMainWindow::GetInstance().RefreshListView();
        InvalidateRect(c->hwnd, nullptr, FALSE);
    }

    void Layout(Ctx* c)
    {
        int bh = c->S(28), by = c->S(kFooterY) + (c->S(kH - kFooterY) - bh) / 2;
        int R = c->S(kW - kM);
        auto place = [&](HWND b, int w, int& x) { MoveWindow(b, x - w, by, w, bh, TRUE); x -= w + c->S(8); };
        int x = R;
        if (c->lastState == 1)
        {
            place(c->hOpen, c->S(84), x);
            place(c->hOpenDir, c->S(104), x);
            place(c->hClose, c->S(72), x);
        }
        else
        {
            place(c->hCancel, c->S(84), x);
            place(c->hPause, c->S(96), x);
        }
        HWND all[] = { c->hPause, c->hCancel, c->hOpen, c->hOpenDir, c->hClose };
        for (HWND b : all) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);

        // tabs
        LayoutTabs(c);
        MoveWindow(c->hTabs, 0, 0, c->S(kW), c->S(kTabH), TRUE);

        // speed page
        int L = c->S(kM), fh = c->S(26), T = c->S(kTabH);
        int y = T + c->S(16);
        MoveWindow(c->hLimit, L, y + (fh - c->S(24)) / 2, c->S(kCtlX - kM - 8), c->S(24), TRUE);
        c->rcLimit = { c->S(kCtlX), y, c->S(kCtlX + 76), y + fh };
        PlaceEdit(c->hLimitEdit, c->rcLimit, c->res.bold, c->S(8));
        int ux = c->rcLimit.right + c->S(6), uw = c->S(52);
        MoveWindow(c->hKB, ux, y, uw, fh, TRUE);
        MoveWindow(c->hMB, ux + uw + c->S(4), y, uw, fh, TRUE);
        y = T + c->S(56);
        int px = c->S(kCtlX);
        for (int p = NDM_PRIO_HIGH; p >= NDM_PRIO_LOW; --p)
        {
            int w = c->S(68);
            MoveWindow(c->hPrio[p], px, y, w, fh, TRUE);
            px += w + c->S(4);
        }
        // completion page
        y = T + c->S(40);
        int ax = L;
        for (int a = NDM_DONE_NONE; a <= NDM_DONE_SHUTDOWN; ++a)
        {
            int w = (std::max)(c->S(80), TextW(c->res.bold, NdmUi::ActionName(a)) + c->S(24));
            MoveWindow(c->hAct[a], ax, y, w, fh, TRUE);
            ax += w + c->S(4);
        }
        HWND seg[] = { c->hKB, c->hMB, c->hPrio[0], c->hPrio[1], c->hPrio[2],
                       c->hAct[0], c->hAct[1], c->hAct[2], c->hAct[3] };
        for (HWND b : seg) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
        SendMessageW(c->hLimit, WM_SETFONT, (WPARAM)c->res.ui, FALSE);
    }

    void ApplyState(Ctx* c)
    {
        int st = c->have ? StateOf(c->snap.status) : 0;
        bool done = (st == 1);
        ShowWindow(c->hOpen,    done ? SW_SHOW : SW_HIDE);
        ShowWindow(c->hOpenDir, done ? SW_SHOW : SW_HIDE);
        ShowWindow(c->hClose,   done ? SW_SHOW : SW_HIDE);
        ShowWindow(c->hPause,   done ? SW_HIDE : SW_SHOW);
        ShowWindow(c->hCancel,  done ? SW_HIDE : SW_SHOW);

        bool running = c->have && (c->snap.status == NDM_STATUS_DOWNLOADING ||
                                   c->snap.status == NDM_STATUS_QUEUED);
        const wchar_t* pl = running ? L"Pause" : (st == 2 ? L"Retry" : L"Resume");
        wchar_t cur[32] = {};
        GetWindowTextW(c->hPause, cur, 32);
        if (wcscmp(cur, pl) != 0) { SetWindowTextW(c->hPause, pl); InvalidateRect(c->hPause, nullptr, FALSE); }

        if (st != c->lastState)
        {
            int prev = c->lastState;
            c->lastState = st;
            Layout(c);
            ShowPage(c);                        // option controls follow "complete"
            if (done && prev == 0)
            {
                FLASHWINFO fi = { sizeof(fi), c->hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
                FlashWindowEx(&fi);
                SetFocus(c->hOpen);
            }
        }
        // Limit changed elsewhere (list menu) while the user is not typing here
        if (c->have && c->snap.speedLimit != c->shownLimit && GetFocus() != c->hLimitEdit)
            LoadLimit(c);

        std::wstring title;
        if (c->have)
        {
            if (done) title = L"Download complete";
            else if (c->snap.totalSize)
            {
                wchar_t b[32];
                swprintf_s(b, L"%.0f%% ", 100.0 * (double)c->snap.downloadedSize / (double)c->snap.totalSize);
                title = b + c->snap.filename;
            }
            else title = c->snap.filename;
        }
        if (title.empty()) title = L"NDM";
        wchar_t curT[512] = {};
        GetWindowTextW(c->hwnd, curT, 512);
        if (title != curT) SetWindowTextW(c->hwnd, title.c_str());
    }

    bool Poll(Ctx* c)
    {
        NdmDownload d;
        if (!NdmQueue::GetInstance().GetSnapshot(c->id, d)) return false;
        if (c->have && (d.priority != c->snap.priority || d.completionAction != c->snap.completionAction))
        {
            for (HWND b : c->hPrio) InvalidateRect(b, nullptr, FALSE);
            for (HWND b : c->hAct) InvalidateRect(b, nullptr, FALSE);
        }
        c->snap = d;
        c->have = true;
        return true;
    }

    void PaintStatus(Ctx* c, HDC dc, const RECT& cl)
    {
        const NdmDlgRes& r = c->res;
        const NdmDownload& d = c->snap;
        int L = c->S(kM), R = cl.right - c->S(kM);

        // ---- file name ----
        RECT nr = { L, c->S(kTabH + 10), R, c->S(kTabH + 30) };
        Text(dc, d.filename.empty() ? L"Resolving file name…" : d.filename, nr, NDM_CLR_FG,
             r.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        double frac = d.totalSize ? (double)d.downloadedSize / (double)d.totalSize : 0.0;
        if (d.status == NDM_STATUS_COMPLETED) frac = 1.0;
        if (frac > 1.0) frac = 1.0;

        // ---- 2-column details ----
        std::wstring status = StatusText(d.status);
        if (d.status == NDM_STATUS_ERROR && !d.errorMsg.empty()) status += L": " + d.errorMsg;
        std::wstring sz  = d.totalSize ? NdmFormatSize(d.totalSize) : std::wstring(L"Unknown");
        std::wstring got = NdmFormatSize(d.downloadedSize);
        if (d.totalSize) { wchar_t b[32]; swprintf_s(b, L"  (%.1f%%)", frac * 100.0); got += b; }
        bool live = d.status == NDM_STATUS_DOWNLOADING;
        std::wstring rate = live ? NdmFormatSpeed(d.speed) : std::wstring(L"—");
        // activeConns: HTTP = connections transferring now, torrent = peers connected now
        if (live && d.kind == NDM_KIND_HTTP && d.activeConns > 0)
            rate += L"  ·  " + std::to_wstring(d.activeConns) +
                    (d.activeConns == 1 ? L" connection" : L" connections");
        else if (live && d.kind == NDM_KIND_TORRENT && d.activeConns > 0)
            rate += L"  ·  " + std::to_wstring(d.activeConns) + (d.activeConns == 1 ? L" peer" : L" peers");
        if (live && d.speedLimit)
            rate += L"  ·  limit " + NdmUi::SpeedLabel(d.speedLimit);
        std::wstring left = L"—";
        if (live && d.speed && d.totalSize > d.downloadedSize)
            left = NdmFormatTime((DWORD)((d.totalSize - d.downloadedSize) / d.speed) + 1);
        // What the engine learned from the server, not a guess.
        std::wstring resume;
        if (d.status == NDM_STATUS_COMPLETED)      resume = L"—";
        else if (d.kind == NDM_KIND_TORRENT)       resume = L"Yes";
        else if (d.kind == NDM_KIND_MEDIA)         resume = L"Yes (partial streams are kept)";
        else if (d.resumable == 1)                 resume = L"Yes";
        else if (d.resumable == 0)                 resume = L"No — Pause starts it again from the beginning";
        else                                       resume = L"Not checked yet";

        struct KV { const wchar_t* k; const std::wstring* v; COLORREF c; HFONT f; };
        KV rows[kRows] = {
            { L"Status",            &status, StatusColor(d.status), r.bold },
            { L"File size",         &sz,     NDM_CLR_FG, r.ui },
            { L"Downloaded",        &got,    NDM_CLR_FG, r.ui },
            { L"Transfer rate",     &rate,   (live && d.speed) ? NDM_CLR_GREEN : NDM_CLR_FG, r.ui },
            { L"Time left",         &left,   NDM_CLR_FG, r.ui },
            { L"Resumable",         &resume, d.resumable == 0 && d.kind == NDM_KIND_HTTP &&
                                                 d.status != NDM_STATUS_COMPLETED ? NDM_CLR_AMBER : NDM_CLR_FG, r.ui },
        };
        int y = c->S(kRowY);
        for (int i = 0; i < kRows; ++i)
        {
            RECT kr = { L, y, L + c->S(kKeyW), y + c->S(kRowH) };
            RECT vr = { kr.right + c->S(8), y, R, y + c->S(kRowH) };
            Text(dc, rows[i].k, kr, NDM_CLR_FG_DIM, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, *rows[i].v, vr, rows[i].c, rows[i].f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += c->S(kRowH);
        }

        // ---- progress bar ----
        RECT pb = { L, c->S(kBarY), R, c->S(kBarY) + c->S(kBarH) };
        COLORREF f1 = NDM_CLR_GOLD_DARK, f2 = NDM_CLR_GOLD_LIGHT;
        if (d.status == NDM_STATUS_PAUSED) { f1 = RGB(150, 104, 24); f2 = NDM_CLR_AMBER; }
        if (d.status == NDM_STATUS_ERROR)  { f1 = RGB(140, 40, 40);  f2 = NDM_CLR_RED; }
        bool indet = live && d.totalSize == 0;
        ProgressBar(dc, pb, indet ? -1.0 : frac, f1, f2, c->phase);

        // ---- file map: which parts of the file are on disk (engine data) ----
        if (d.kind == NDM_KIND_HTTP && d.fileMapOk && d.status != NDM_STATUS_COMPLETED)
        {
            const int n = 32;
            RECT sb = { L, c->S(kSegY), R, c->S(kSegY) + c->S(kSegH) };
            int gap = c->S(2);
            int cw = ((sb.right - sb.left) - gap * (n - 1)) / n;
            if (cw < 2) { cw = 2; gap = 0; }
            for (int i = 0; i < n; ++i)
            {
                RECT cell = { sb.left + i * (cw + gap), sb.top, sb.left + i * (cw + gap) + cw, sb.bottom };
                if (i == n - 1) cell.right = sb.right;
                Fill(dc, cell, RGB(38, 42, 51));
                double sf = d.fileMap[i] / 1000.0;
                RECT fr = cell; fr.right = cell.left + (LONG)((cell.right - cell.left) * sf + 0.5);
                if (fr.right > fr.left) Fill(dc, fr, live ? NDM_CLR_GOLD : RGB(150, 122, 42));
            }
        }
    }

    void PaintOptions(Ctx* c, HDC dc, const RECT& cl)
    {
        const NdmDlgRes& r = c->res;
        const NdmDownload& d = c->snap;
        int L = c->S(kM), R = cl.right - c->S(kM), T = c->S(kTabH);
        const UINT one = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        bool done = d.status == NDM_STATUS_COMPLETED;
        auto note = [&](int y96, const std::wstring& s, COLORREF col)
        {
            RECT nr = { L, T + c->S(y96), R, T + c->S(y96 + 18) };
            Text(dc, s, nr, col, r.sm, one);
        };
        if (c->tab == TAB_SPEED)
        {
            DrawField(dc, c->rcLimit, GetFocus() == c->hLimitEdit, !IsWindowEnabled(c->hLimitEdit), r.dpi);
            RECT pl = { L, T + c->S(56), c->S(kCtlX - 8), T + c->S(82) };
            Text(dc, L"Priority", pl, done ? NDM_CLR_DISABLED : NDM_CLR_FG, r.ui, one);
            int y = 100;
            if (done) { note(y, L"This download is complete.", NDM_CLR_FG_DIM); return; }
            if (d.kind == NDM_KIND_MEDIA && d.status == NDM_STATUS_DOWNLOADING)
            {
                note(y, L"A media download uses a new limit the next time it starts.", NDM_CLR_FG_DIM);
                y += 20;
            }
            DWORD global = NdmConfigMgr::GetInstance().GetSpeedLimit();
            if (global)
                note(y, L"Overall limit for all downloads: " + NdmUi::SpeedLabel(global) + L" (Options).",
                     NDM_CLR_FG_DIM);
        }
        else if (c->tab == TAB_DONE)
        {
            RECT tr = { L, T + c->S(14), R, T + c->S(34) };
            Text(dc, L"When this download finishes", tr, done ? NDM_CLR_DISABLED : NDM_CLR_FG, r.ui, one);
            if (done) { note(80, L"This download is complete.", NDM_CLR_FG_DIM); return; }
            std::wstring na = NdmPower::UnavailableText(NDM_DONE_HIBERNATE);
            if (na.empty()) na = NdmPower::UnavailableText(NDM_DONE_SLEEP);
            note(80, L"You get 30 seconds to cancel. Not kept when NDM restarts.", NDM_CLR_FG_DIM);
            if (!na.empty()) note(100, na, NDM_CLR_FG_DIM);
        }
    }

    void Paint(Ctx* c)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(c->hwnd, &ps);
        RECT cl; GetClientRect(c->hwnd, &cl);
        {
            NdmMemDC mem(hdc, cl);
            HDC dc = mem.ok() ? mem.dc : hdc;
            Fill(dc, cl, kBg);
            if (c->tab == TAB_STATUS) PaintStatus(c, dc, cl);
            else                      PaintOptions(c, dc, cl);

            // ---- footer ----
            RECT ft = { 0, c->S(kFooterY), cl.right, cl.bottom };
            Fill(dc, ft, kFootBg);
            HLine(dc, 0, cl.right, ft.top, RGB(40, 44, 53));
        }
        EndPaint(c->hwnd, &ps);
    }

    void OnCmd(Ctx* c, int id, int code, HWND ctl)
    {
        NdmQueue& q = NdmQueue::GetInstance();
        if (id >= IDC_PRIO0 && id <= IDC_PRIO0 + NDM_PRIO_HIGH)
        {
            q.SetPriority(c->id, id - IDC_PRIO0);
            c->snap.priority = id - IDC_PRIO0;
            for (HWND b : c->hPrio) InvalidateRect(b, nullptr, FALSE);
            NdmMainWindow::GetInstance().RefreshListView();
            return;
        }
        if (id >= IDC_ACT0 && id <= IDC_ACT0 + NDM_DONE_SHUTDOWN)
        {
            int a = id - IDC_ACT0;
            if (NdmPower::Available(a) && q.SetDownloadCompletionAction(c->id, a))
                c->snap.completionAction = a;
            for (HWND b : c->hAct) InvalidateRect(b, nullptr, FALSE);
            return;
        }
        switch (id)
        {
        case IDC_LIMIT:
            ShowPage(c);
            ApplyLimit(c);
            if (IsOn(c->hLimit)) { SetFocus(c->hLimitEdit); SendMessageW(c->hLimitEdit, EM_SETSEL, 0, -1); }
            return;
        case IDC_UNIT_KB:
        case IDC_UNIT_MB:
            c->unitMB = id == IDC_UNIT_MB;
            InvalidateRect(c->hKB, nullptr, FALSE);
            InvalidateRect(c->hMB, nullptr, FALSE);
            ApplyLimit(c);
            return;
        case IDC_LIMITEDIT:
            if (code == EN_CHANGE && !c->setting) SetTimer(c->hwnd, kApplyTimer, 700, nullptr);
            if (code == EN_KILLFOCUS) { KillTimer(c->hwnd, kApplyTimer); ApplyLimit(c); }
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(c->hwnd, &c->rcLimit, FALSE);
            return;
        case IDC_TABS:
            return;
        case IDC_PAUSE:
            if (c->snap.status == NDM_STATUS_DOWNLOADING || c->snap.status == NDM_STATUS_QUEUED)
                q.PauseDownload(c->id);
            else
                q.ResumeDownload(c->id);
            break;
        case IDC_CANCELDL:      // stop the transfer (progress kept) and close
            if (c->snap.status == NDM_STATUS_DOWNLOADING || c->snap.status == NDM_STATUS_QUEUED)
                q.PauseDownload(c->id);
            DestroyWindow(c->hwnd);
            NdmMainWindow::GetInstance().RefreshListView();
            return;
        case IDC_OPEN:
        {
            std::wstring f = JoinPath(c->snap.savePath, c->snap.filename);
            ShellExecuteW(c->hwnd, L"open", f.c_str(), nullptr, c->snap.savePath.c_str(), SW_SHOWNORMAL);
            DestroyWindow(c->hwnd);
            return;
        }
        case IDC_OPENDIR:
            OpenFolderSelect(c->snap.savePath, c->snap.filename);
            DestroyWindow(c->hwnd);
            return;
        case IDOK:
        {
            // Enter in the limit field applies it; on an option button it
            // presses that button; otherwise the window's default action.
            HWND f = GetFocus();
            if (f == c->hLimitEdit) { KillTimer(c->hwnd, kApplyTimer); ApplyLimit(c); return; }
            if (f && GetParent(f) == c->hwnd && c->tab != TAB_STATUS && f != c->hTabs)
            {
                wchar_t cls[16] = {};
                if (GetClassNameW(f, cls, 16) && _wcsicmp(cls, L"Button") == 0)
                {
                    int fid = GetDlgCtrlID(f);
                    if (fid != IDC_PAUSE && fid != IDC_CANCELDL && fid != IDC_OPEN &&
                        fid != IDC_OPENDIR && fid != IDC_CLOSE)
                    {
                        SendMessageW(f, BM_CLICK, 0, 0);
                        return;
                    }
                }
            }
            if (c->lastState == 1) { OnCmd(c, IDC_OPEN, 0, nullptr); return; }
            DestroyWindow(c->hwnd);
            return;
        }
        case IDCANCEL:          // Esc / close box: hide this window, keep downloading
        case IDC_CLOSE:
            DestroyWindow(c->hwnd);
            return;
        default:
            (void)ctl;
            return;
        }
        if (Poll(c)) ApplyState(c);
        InvalidateRect(c->hwnd, nullptr, FALSE);
        NdmMainWindow::GetInstance().RefreshListView();
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        Ctx* c = Get(h);
        switch (m)
        {
        case WM_NCCREATE:
            c = (Ctx*)((CREATESTRUCTW*)l)->lpCreateParams;
            c->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
            break;
        case WM_CREATE:
        {
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            HINSTANCE hi = GetModuleHandleW(nullptr);
            // creation order = tab order: tabs, page controls, footer
            c->hTabs = CreateWindowExW(0, kTabClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_TABS, hi, nullptr);
            c->hLimit     = MakeBtn(h, IDC_LIMIT, L"Limit this download", GLY_NONE, true);
            c->hLimitEdit = MakeEdit(h, IDC_LIMITEDIT, ES_RIGHT);
            SetAccName(c->hLimitEdit, L"Speed limit for this download");
            c->hKB        = MakeBtn(h, IDC_UNIT_KB, L"KB/s");
            c->hMB        = MakeBtn(h, IDC_UNIT_MB, L"MB/s");
            c->hPrio[NDM_PRIO_HIGH]   = MakeBtn(h, IDC_PRIO0 + NDM_PRIO_HIGH,   L"High");
            c->hPrio[NDM_PRIO_NORMAL] = MakeBtn(h, IDC_PRIO0 + NDM_PRIO_NORMAL, L"Normal");
            c->hPrio[NDM_PRIO_LOW]    = MakeBtn(h, IDC_PRIO0 + NDM_PRIO_LOW,    L"Low");
            for (int a = NDM_DONE_NONE; a <= NDM_DONE_SHUTDOWN; ++a)
                c->hAct[a] = MakeBtn(h, IDC_ACT0 + a, NdmUi::ActionName(a));
            c->hPause   = MakeBtn(h, IDC_PAUSE,    L"Pause");
            c->hCancel  = MakeBtn(h, IDC_CANCELDL, L"Cancel");
            c->hOpen    = MakeBtn(h, IDC_OPEN,     L"Open");
            c->hOpenDir = MakeBtn(h, IDC_OPENDIR,  L"Open Folder");
            c->hClose   = MakeBtn(h, IDC_CLOSE,    L"Close");
            SendMessageW(c->hLimitEdit, EM_LIMITTEXT, 9, 0);
            Poll(c);
            LoadLimit(c);
            ApplyState(c);
            Layout(c);
            ShowPage(c);
            SetTimer(h, kTimer, 250, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (w == kApplyTimer)
            {
                KillTimer(h, kApplyTimer);
                ApplyLimit(c);
                return 0;
            }
            if (w == kTimer)
            {
                if (!Poll(c)) { DestroyWindow(h); return 0; }   // download removed
                c->phase = (c->phase + 8) % 1200;
                ApplyState(c);
                RECT cl; GetClientRect(h, &cl);
                cl.top = c->S(kTabH);
                cl.bottom = c->S(kFooterY);
                if (c->tab == TAB_STATUS) InvalidateRect(h, &cl, FALSE);
            }
            return 0;
        case WM_NDM_CTRLTAB:
            SelectTab(c, (c->tab + (w ? TAB_COUNT - 1 : 1)) % TAB_COUNT);
            return 1;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(c);
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return (LRESULT)EditColors((HDC)w, (HWND)l, c->res);
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            int id = (int)di->CtlID;
            NdmBtnKind k = BTN_SECONDARY;
            COLORREF bg = kFootBg;
            HFONT f = c->res.bold;
            if (id == IDC_PAUSE || id == IDC_OPEN) k = BTN_PRIMARY;
            else if (id == IDC_CLOSE) k = BTN_GHOST;
            else if (id == IDC_LIMIT) { k = BTN_TOGGLE; bg = kBg; f = c->res.ui; }
            else if (id == IDC_UNIT_KB || id == IDC_UNIT_MB)
            {
                k = ((id == IDC_UNIT_MB) == c->unitMB) ? BTN_PRIMARY : BTN_SECONDARY;
                bg = kBg;
            }
            else if (id >= IDC_PRIO0 && id <= IDC_PRIO0 + NDM_PRIO_HIGH)
            {
                k = c->snap.priority == id - IDC_PRIO0 ? BTN_PRIMARY : BTN_SECONDARY;
                bg = kBg;
            }
            else if (id >= IDC_ACT0 && id <= IDC_ACT0 + NDM_DONE_SHUTDOWN)
            {
                k = c->snap.completionAction == id - IDC_ACT0 ? BTN_PRIMARY : BTN_SECONDARY;
                bg = kBg;
            }
            DrawButton(di, k, bg, f, c->res.dpi);
            return TRUE;
        }
        case WM_COMMAND:
            OnCmd(c, LOWORD(w), HIWORD(w), (HWND)l);
            return 0;
        case WM_DPICHANGED:
        {
            c->res.Create(HIWORD(w));
            SetWindowIcons(h, HIWORD(w));
            RECT* nr = (RECT*)l;
            SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(c);
            InvalidateRect(h, nullptr, TRUE);
            InvalidateRect(c->hTabs, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            KillTimer(h, kTimer);
            if (c && KillTimer(h, kApplyTimer)) ApplyLimit(c);   // typed, then closed at once
            RemoveModeless(h);
            if (c) g_wins.erase(c->id);
            return 0;
        case WM_NCDESTROY:
            if (c)
            {
                c->res.Free();
                delete c;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

void NdmProgressDlg::Show(HWND owner, DWORD id)
{
    auto it = g_wins.find(id);
    if (it != g_wins.end() && IsWindow(it->second))
    {
        ForceForeground(it->second);
        return;
    }
    NdmDownload probe;
    if (!NdmQueue::GetInstance().GetSnapshot(id, probe)) return;

    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        bool a = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        WNDCLASSEXW tc = { sizeof(tc) };
        tc.lpfnWndProc   = TabProc;
        tc.hInstance     = hi;
        tc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        tc.lpszClassName = kTabClass;
        bool b = RegisterClassExW(&tc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        reg = a && b;
    }
    Ctx* c = new Ctx();
    c->id = id;
    c->owner = owner;
    POINT pt = CreationPoint(owner);
    HWND h = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kClass, L"NDM",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, nullptr, nullptr, hi, c);
    if (!h) { delete c; return; }
    g_wins[id] = h;
    // Cascade slightly when several are open
    PlaceWindow(h, kW, kH, owner);
    if (g_wins.size() > 1)
    {
        RECT wr; GetWindowRect(h, &wr);
        int off = c->S(24) * (int)((g_wins.size() - 1) % 6);
        SetWindowPos(h, nullptr, wr.left + off, wr.top + off, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    AddModeless(h);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
}

void NdmProgressDlg::CloseFor(DWORD id)
{
    auto it = g_wins.find(id);
    if (it != g_wins.end())
    {
        HWND h = it->second;
        if (IsWindow(h)) DestroyWindow(h);   // WM_DESTROY erases the entry
        else g_wins.erase(it);
    }
}
