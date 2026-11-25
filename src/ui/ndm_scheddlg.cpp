// ndm_scheddlg.cpp -- Scheduler window (modeless, compact)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Layout (96-dpi units, client 460 x 282):
//   [o] Start download at   [07:00]
//       [Daily][Once]  [Mon][Tue]...[Sun]      (Once: on [2026-11-25])
//   [o] Stop download at    [09:00]
//   Max simultaneous downloads              [-][ 3 ][+]
//   When all downloads finish
//   [Nothing][Sleep][Hibernate][Shut down]
//   3 items in the scheduler queue
//   ------------------------------------------------------------
//   [Start queue now] [Stop queue]                       [Close]
//
// Settings are applied as soon as they are valid (toggle / button clicks,
// leaving a time field, Close): NdmConfigMgr [Scheduler] + NdmQueue.

#include "ndm_scheddlg.h"
#include "ndm_theme.h"
#include "ndm_uilogic.h"
#include "ndm_power.h"
#include "ndm_optdlg.h"
#include "ndm_mainwnd.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"

#include <commctrl.h>
#include <algorithm>
#include <string>

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass = L"NDMSchedulerWnd";
    const int kW = 460, kH = 282, kM = 16, kFieldH = 26, kFootH = 46;
    const UINT kTimer = 1;
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);

    enum
    {
        IDC_S_START = 700, IDC_S_STARTTIME, IDC_S_DAILY, IDC_S_ONCE, IDC_S_DATE,
        IDC_S_STOP, IDC_S_STOPTIME, IDC_S_MAXMINUS, IDC_S_MAX, IDC_S_MAXPLUS,
        IDC_S_RUN, IDC_S_HALT, IDC_S_CLOSE,
        IDC_S_DAY0 = 720,               // + 0..6 = Sunday..Saturday (dayMask bit)
        IDC_S_ACT0 = 730                // + NDM_DONE_*
    };

    HWND g_wnd = nullptr;

    struct Ctx
    {
        HWND hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        HWND hStart = nullptr, hStartTime = nullptr, hDaily = nullptr, hOnce = nullptr, hDate = nullptr,
             hStop = nullptr, hStopTime = nullptr, hMaxMinus = nullptr, hMax = nullptr, hMaxPlus = nullptr,
             hRun = nullptr, hHalt = nullptr, hClose = nullptr;
        HWND hDay[7] = {};
        HWND hAct[4] = {};
        int  dayOrder[7] = { 1, 2, 3, 4, 5, 6, 0 };   // display order (bit numbers)
        bool once = false;
        int  dayMask = 0x7F;
        int  allDone = NDM_DONE_NONE;
        int  scheduledCount = 0;
        std::wstring error, info;
        RECT rcStartTime{}, rcStopTime{}, rcDate{}, rcMax{};
        int  rowMaxY = 0, rowActY = 0, rowInfoY = 0;
        int S(int v) const { return res.S(v); }
    };

    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    void SetCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    bool IsOn(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int  Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    int TextW(HFONT f, const std::wstring& s)
    {
        HDC dc = GetDC(nullptr);
        SIZE sz = MeasureText(dc, s, f);
        ReleaseDC(nullptr, dc);
        return sz.cx;
    }

    int TodayYmd()
    {
        SYSTEMTIME st; GetLocalTime(&st);
        return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    }

    void CountScheduled(Ctx* c)
    {
        int n = 0;
        for (const NdmDownload& d : NdmQueue::GetInstance().GetAllSnapshots())
            if (d.scheduled && d.status != NDM_STATUS_COMPLETED) ++n;
        if (n != c->scheduledCount || c->info.empty())
        {
            c->scheduledCount = n;
            c->info = n == 0 ? L"Scheduler queue is empty. Right-click a download → Add to scheduler."
                    : n == 1 ? L"1 item in the scheduler queue."
                    : std::to_wstring(n) + L" items in the scheduler queue.";
            RECT r = { 0, c->S(c->rowInfoY), c->S(kW), c->S(c->rowInfoY + 18) };
            InvalidateRect(c->hwnd, &r, FALSE);
        }
    }

    void Layout(Ctx* c)
    {
        int L = c->S(kM), R = c->S(kW - kM), fh = c->S(kFieldH);
        int y = c->S(14);
        // start
        MoveWindow(c->hStart, L, y + (fh - c->S(24)) / 2, c->S(168), c->S(24), TRUE);
        c->rcStartTime = { L + c->S(176), y, L + c->S(176 + 64), y + fh };
        PlaceEdit(c->hStartTime, c->rcStartTime, c->res.bold, c->S(6));
        // daily / once + days or date
        y = c->S(48);
        int x = L + c->S(40);
        int sw = (std::max)(c->S(52), TextW(c->res.bold, L"Daily") + c->S(20));
        MoveWindow(c->hDaily, x, y, sw, fh, TRUE); x += sw + c->S(4);
        sw = (std::max)(c->S(52), TextW(c->res.bold, L"Once") + c->S(20));
        MoveWindow(c->hOnce, x, y, sw, fh, TRUE); x += sw + c->S(10);
        int dx = x;
        int dw = c->S(34);
        for (int i = 0; i < 7; ++i)
            dw = (std::max)(dw, TextW(c->res.ui, GetText(c->hDay[i])) + c->S(14));
        dw = (std::min)(dw, (R - dx - c->S(3) * 6) / 7);
        for (int k = 0; k < 7; ++k)
        {
            MoveWindow(c->hDay[c->dayOrder[k]], dx, y, dw, fh, TRUE);
            dx += dw + c->S(3);
        }
        c->rcDate = { x + c->S(24), y, x + c->S(24 + 104), y + fh };
        PlaceEdit(c->hDate, c->rcDate, c->res.bold, c->S(6));
        // stop
        y = c->S(84);
        MoveWindow(c->hStop, L, y + (fh - c->S(24)) / 2, c->S(168), c->S(24), TRUE);
        c->rcStopTime = { L + c->S(176), y, L + c->S(176 + 64), y + fh };
        PlaceEdit(c->hStopTime, c->rcStopTime, c->res.bold, c->S(6));
        // max simultaneous
        c->rowMaxY = 122;
        y = c->S(c->rowMaxY);
        MoveWindow(c->hMaxPlus, R - fh, y, fh, fh, TRUE);
        c->rcMax = { R - fh - c->S(40), y, R - fh, y + fh };
        MoveWindow(c->hMaxMinus, c->rcMax.left - fh, y, fh, fh, TRUE);
        PlaceEdit(c->hMax, c->rcMax, c->res.bold, c->S(4));
        // all done action
        c->rowActY = 154;                          // caption row, buttons below it
        y = c->S(c->rowActY + 20);
        x = L;
        for (int a = NDM_DONE_NONE; a <= NDM_DONE_SHUTDOWN; ++a)
        {
            int w = (std::max)(c->S(64), TextW(c->res.bold, NdmUi::ActionName(a)) + c->S(22));
            MoveWindow(c->hAct[a], x, y, w, fh, TRUE);
            x += w + c->S(4);
        }
        c->rowInfoY = 208;
        // footer
        int bh = c->S(28), by = c->S(kH - kFootH) + (c->S(kFootH) - bh) / 2;
        int w1 = TextW(c->res.bold, L"Start queue now") + c->S(28);
        int w2 = TextW(c->res.bold, L"Stop queue") + c->S(28);
        MoveWindow(c->hRun, L, by, w1, bh, TRUE);
        MoveWindow(c->hHalt, L + w1 + c->S(8), by, w2, bh, TRUE);
        MoveWindow(c->hClose, R - c->S(76), by, c->S(76), bh, TRUE);

        HWND bold[] = { c->hDaily, c->hOnce, c->hMaxMinus, c->hMaxPlus, c->hRun, c->hHalt, c->hClose,
                        c->hAct[0], c->hAct[1], c->hAct[2], c->hAct[3] };
        for (HWND b : bold) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
        for (HWND b : c->hDay) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.ui, FALSE);
        SendMessageW(c->hStart, WM_SETFONT, (WPARAM)c->res.ui, FALSE);
        SendMessageW(c->hStop, WM_SETFONT, (WPARAM)c->res.ui, FALSE);
    }

    void ShowMode(Ctx* c)
    {
        for (HWND d : c->hDay) ShowWindow(d, c->once ? SW_HIDE : SW_SHOW);
        ShowWindow(c->hDate, c->once ? SW_SHOW : SW_HIDE);
        bool startOn = IsOn(c->hStart);
        EnableWindow(c->hStartTime, startOn);
        EnableWindow(c->hDaily, startOn);
        EnableWindow(c->hOnce, startOn);
        EnableWindow(c->hDate, startOn);
        for (HWND d : c->hDay) EnableWindow(d, startOn);
        EnableWindow(c->hStopTime, IsOn(c->hStop));
        for (int a = 0; a < 4; ++a)
            EnableWindow(c->hAct[a], NdmPower::Available(a));
        InvalidateRect(c->hwnd, nullptr, FALSE);
    }

    // Validates and applies everything; false (+ error shown) when a field
    // is not valid -- nothing of the schedule is changed then.
    bool Commit(Ctx* c)
    {
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        NdmQueue& q = NdmQueue::GetInstance();
        NdmSchedule s = q.GetSchedule();
        s.startEnabled = IsOn(c->hStart);
        s.stopEnabled  = IsOn(c->hStop);
        s.once = c->once;
        s.dayMask = c->dayMask & 0x7F;
        int mins = 0;
        c->error.clear();
        if (NdmUi::ParseHhMm(GetText(c->hStartTime), mins)) s.startMinute = mins;
        else if (s.startEnabled) c->error = L"Enter the start time as hh:mm, for example 07:30.";
        if (NdmUi::ParseHhMm(GetText(c->hStopTime), mins)) s.stopMinute = mins;
        else if (s.stopEnabled && c->error.empty()) c->error = L"Enter the stop time as hh:mm, for example 09:00.";
        int ymd = 0;
        if (NdmUi::ParseIsoDate(GetText(c->hDate), ymd)) s.onceDate = ymd;
        else if (s.startEnabled && s.once && c->error.empty()) c->error = L"Enter the date as yyyy-mm-dd.";
        if (s.startEnabled && !s.once && s.dayMask == 0 && c->error.empty())
            c->error = L"Choose at least one day.";
        if (!c->error.empty())
        {
            InvalidateRect(c->hwnd, nullptr, FALSE);
            return false;
        }
        cfg.SetSchedule(s);
        int mx = Clamp(_wtoi(GetText(c->hMax).c_str()), 1, 16);
        cfg.SetMaxDownloads(mx);
        cfg.Save();
        NdmOptDlg::ApplyRunKey();            // cfg.Save rewrites the Run key; keep ours
        q.SetSchedule(s);
        q.SetMaxActiveDownloads(mx);
        InvalidateRect(c->hwnd, nullptr, FALSE);
        return true;
    }

    void Load(Ctx* c)
    {
        NdmSchedule s = NdmQueue::GetInstance().GetSchedule();
        SetCheck(c->hStart, s.startEnabled);
        SetCheck(c->hStop, s.stopEnabled);
        SetWindowTextW(c->hStartTime, NdmUi::FormatHhMm(s.startMinute).c_str());
        SetWindowTextW(c->hStopTime, NdmUi::FormatHhMm(s.stopMinute).c_str());
        SetWindowTextW(c->hDate, NdmUi::FormatIsoDate(s.onceDate > 0 ? s.onceDate : TodayYmd()).c_str());
        SetWindowTextW(c->hMax, std::to_wstring(Clamp(NdmConfigMgr::GetInstance().GetMaxDownloads(), 1, 16)).c_str());
        c->once = s.once;
        c->dayMask = s.dayMask & 0x7F;
        c->allDone = NdmQueue::GetInstance().GetAllDoneAction();
    }

    void Close(Ctx* c, bool force)
    {
        if (!Commit(c) && !force) return;
        DestroyWindow(c->hwnd);
    }

    void Paint(Ctx* c)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(c->hwnd, &ps);
        RECT cl; GetClientRect(c->hwnd, &cl);
        {
            NdmMemDC mem(hdc, cl);
            HDC dc = mem.ok() ? mem.dc : hdc;
            const NdmDlgRes& r = c->res;
            Fill(dc, cl, kBg);
            int L = c->S(kM), R = cl.right - c->S(kM);
            const UINT one = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            HWND f = GetFocus();
            bool startOn = IsOn(c->hStart);
            DrawField(dc, c->rcStartTime, f == c->hStartTime, !startOn, r.dpi);
            DrawField(dc, c->rcStopTime, f == c->hStopTime, !IsOn(c->hStop), r.dpi);
            if (c->once)
            {
                RECT on = { c->rcDate.left - c->S(24), c->rcDate.top, c->rcDate.left - c->S(4), c->rcDate.bottom };
                Text(dc, L"on", on, startOn ? NDM_CLR_FG : NDM_CLR_DISABLED, r.ui, one);
                DrawField(dc, c->rcDate, f == c->hDate, !startOn, r.dpi);
            }
            RECT mr = { L, c->S(c->rowMaxY), c->rcMax.left - c->S(36), c->S(c->rowMaxY + kFieldH) };
            Text(dc, L"Max simultaneous downloads", mr, NDM_CLR_FG, r.ui, one);
            DrawField(dc, c->rcMax, f == c->hMax, false, r.dpi);
            RECT al = { L, c->S(c->rowActY), R, c->S(c->rowActY + 18) };
            Text(dc, L"When all downloads finish", al, NDM_CLR_FG, r.ui, one);
            RECT ir = { L, c->S(c->rowInfoY), R, c->S(c->rowInfoY + 18) };
            if (!c->error.empty())
                Text(dc, c->error, ir, RGB(236, 110, 110), r.sm, one);
            else
            {
                std::wstring t = c->info;
                std::wstring na = NdmPower::UnavailableText(NDM_DONE_HIBERNATE);
                if (na.empty()) na = NdmPower::UnavailableText(NDM_DONE_SLEEP);
                if (!na.empty() && c->scheduledCount > 0) t += L"  " + na;   // the greyed button already says it otherwise
                Text(dc, t, ir, NDM_CLR_FG_DIM, r.sm, one);
            }
            RECT ft = { 0, c->S(kH - kFootH), cl.right, cl.bottom };
            Fill(dc, ft, kFootBg);
            HLine(dc, 0, cl.right, ft.top, RGB(40, 44, 53));
        }
        EndPaint(c->hwnd, &ps);
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
            // first day of the week from the user's locale (0 = Monday)
            wchar_t fd[4] = {};
            int first = 0;
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, fd, 4)) first = _wtoi(fd) % 7;
            for (int k = 0; k < 7; ++k) c->dayOrder[k] = (first + k + 1) % 7;   // Monday = bit 1
            // creation order = tab order
            c->hStart     = MakeBtn(h, IDC_S_START, L"Start download at", GLY_NONE, true);
            c->hStartTime = MakeEdit(h, IDC_S_STARTTIME, ES_CENTER);
            c->hDaily     = MakeBtn(h, IDC_S_DAILY, L"Daily");
            c->hOnce      = MakeBtn(h, IDC_S_ONCE, L"Once");
            for (int k = 0; k < 7; ++k)
            {
                int bit = c->dayOrder[k];
                wchar_t nm[32] = {};
                // LOCALE_SABBREVDAYNAME1 = Monday ... 7 = Sunday
                LCTYPE t = (LCTYPE)(LOCALE_SABBREVDAYNAME1 + (bit + 6) % 7);
                if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, t, nm, 32)) wcscpy_s(nm, L"?");
                c->hDay[bit] = MakeBtn(h, IDC_S_DAY0 + bit, nm);
            }
            c->hDate      = MakeEdit(h, IDC_S_DATE, ES_CENTER);
            c->hStop      = MakeBtn(h, IDC_S_STOP, L"Stop download at", GLY_NONE, true);
            c->hStopTime  = MakeEdit(h, IDC_S_STOPTIME, ES_CENTER);
            c->hMaxMinus  = MakeBtn(h, IDC_S_MAXMINUS, L"", GLY_MINUS);
            c->hMax       = MakeEdit(h, IDC_S_MAX, ES_NUMBER | ES_CENTER);
            c->hMaxPlus   = MakeBtn(h, IDC_S_MAXPLUS, L"", GLY_PLUS);
            SetAccName(c->hStartTime, L"Start time");
            SetAccName(c->hDate, L"Date");
            SetAccName(c->hStopTime, L"Stop time");
            SetAccName(c->hMax, L"Max simultaneous downloads");
            SetAccName(c->hMaxMinus, L"Fewer simultaneous downloads");
            SetAccName(c->hMaxPlus, L"More simultaneous downloads");
            for (int a = NDM_DONE_NONE; a <= NDM_DONE_SHUTDOWN; ++a)
                c->hAct[a] = MakeBtn(h, IDC_S_ACT0 + a, NdmUi::ActionName(a));
            c->hRun   = MakeBtn(h, IDC_S_RUN, L"Start queue now");
            c->hHalt  = MakeBtn(h, IDC_S_HALT, L"Stop queue");
            c->hClose = MakeBtn(h, IDC_S_CLOSE, L"Close");
            SendMessageW(c->hStartTime, EM_LIMITTEXT, 5, 0);
            SendMessageW(c->hStopTime, EM_LIMITTEXT, 5, 0);
            SendMessageW(c->hDate, EM_LIMITTEXT, 10, 0);
            SendMessageW(c->hMax, EM_LIMITTEXT, 2, 0);
            SendMessageW(c->hDate, EM_SETCUEBANNER, FALSE, (LPARAM)L"yyyy-mm-dd");
            Load(c);
            Layout(c);
            ShowMode(c);
            CountScheduled(c);
            SetTimer(h, kTimer, 1000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (w == kTimer)
            {
                CountScheduled(c);
                int a = NdmQueue::GetInstance().GetAllDoneAction();   // one-shot: resets after it fired
                if (a != c->allDone)
                {
                    c->allDone = a;
                    for (HWND b : c->hAct) InvalidateRect(b, nullptr, FALSE);
                }
            }
            return 0;
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
            COLORREF bg = kBg;
            HFONT f = c->res.bold;
            if (id == IDC_S_START || id == IDC_S_STOP) { k = BTN_TOGGLE; f = c->res.ui; }
            else if (id == IDC_S_DAILY) k = !c->once ? BTN_PRIMARY : BTN_SECONDARY;
            else if (id == IDC_S_ONCE)  k = c->once ? BTN_PRIMARY : BTN_SECONDARY;
            else if (id >= IDC_S_DAY0 && id < IDC_S_DAY0 + 7)
            {
                k = (c->dayMask & (1 << (id - IDC_S_DAY0))) ? BTN_PRIMARY : BTN_SECONDARY;
                f = c->res.ui;
            }
            else if (id >= IDC_S_ACT0 && id < IDC_S_ACT0 + 4)
                k = c->allDone == id - IDC_S_ACT0 ? BTN_PRIMARY : BTN_SECONDARY;
            else if (id == IDC_S_MAXMINUS || id == IDC_S_MAXPLUS) k = BTN_STEPPER;
            else if (id == IDC_S_RUN || id == IDC_S_HALT) bg = kFootBg;
            else if (id == IDC_S_CLOSE) { k = BTN_GHOST; bg = kFootBg; }
            DrawButton(di, k, bg, f, c->res.dpi);
            return TRUE;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == IDOK)
            {
                // Enter: the focused button, else leave the field (applies it)
                HWND f = GetFocus();
                wchar_t cls[16] = {};
                if (f && GetClassNameW(f, cls, 16) && _wcsicmp(cls, L"Button") == 0)
                {
                    SendMessageW(f, BM_CLICK, 0, 0);
                    return 0;
                }
                Commit(c);
                return 0;
            }
            if (id >= IDC_S_DAY0 && id < IDC_S_DAY0 + 7)
            {
                c->dayMask ^= 1 << (id - IDC_S_DAY0);
                InvalidateRect((HWND)l, nullptr, FALSE);
                Commit(c);
                return 0;
            }
            if (id >= IDC_S_ACT0 && id < IDC_S_ACT0 + 4)
            {
                int a = id - IDC_S_ACT0;
                if (!NdmPower::Available(a)) return 0;
                c->allDone = a;
                NdmQueue::GetInstance().SetAllDoneAction(a);
                for (HWND b : c->hAct) InvalidateRect(b, nullptr, FALSE);
                return 0;
            }
            switch (id)
            {
            case IDC_S_START:
            case IDC_S_STOP:
                ShowMode(c);
                Commit(c);
                return 0;
            case IDC_S_DAILY:
            case IDC_S_ONCE:
                c->once = id == IDC_S_ONCE;
                InvalidateRect(c->hDaily, nullptr, FALSE);
                InvalidateRect(c->hOnce, nullptr, FALSE);
                ShowMode(c);
                Commit(c);
                return 0;
            case IDC_S_MAXMINUS:
            case IDC_S_MAXPLUS:
            {
                int v = Clamp(_wtoi(GetText(c->hMax).c_str()) + (id == IDC_S_MAXPLUS ? 1 : -1), 1, 16);
                SetWindowTextW(c->hMax, std::to_wstring(v).c_str());
                Commit(c);
                return 0;
            }
            case IDC_S_RUN:
                if (Commit(c))
                {
                    NdmQueue::GetInstance().StartQueue();
                    NdmMainWindow::GetInstance().RefreshListView();
                }
                return 0;
            case IDC_S_HALT:
                NdmQueue::GetInstance().StopQueue();
                NdmMainWindow::GetInstance().RefreshListView();
                CountScheduled(c);
                return 0;
            case IDC_S_CLOSE:
                Close(c, false);
                return 0;
            case IDCANCEL:                     // Esc: apply what is valid, always close
                Close(c, true);
                return 0;
            case IDC_S_STARTTIME:
            case IDC_S_STOPTIME:
            case IDC_S_DATE:
            case IDC_S_MAX:
                if (code == EN_KILLFOCUS)
                {
                    if (id == IDC_S_MAX)
                        SetWindowTextW(c->hMax, std::to_wstring(Clamp(_wtoi(GetText(c->hMax).c_str()), 1, 16)).c_str());
                    Commit(c);
                }
                if (code == EN_CHANGE && !c->error.empty()) { c->error.clear(); InvalidateRect(h, nullptr, FALSE); }
                if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_DPICHANGED:
        {
            c->res.Create(HIWORD(w));
            SetWindowIcons(h, HIWORD(w));
            RECT* nr = (RECT*)l;
            SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            Close(c, false);
            return 0;
        case WM_DESTROY:
            KillTimer(h, kTimer);
            RemoveModeless(h);
            if (g_wnd == h) g_wnd = nullptr;
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

void NdmSchedDlg::Show(HWND owner)
{
    if (g_wnd && IsWindow(g_wnd)) { ForceForeground(g_wnd); return; }
    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        reg = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    Ctx* c = new Ctx();
    c->owner = owner;
    POINT pt = CreationPoint(owner);
    HWND h = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kClass, L"Scheduler",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, nullptr, nullptr, hi, c);
    if (!h) { delete c; return; }
    g_wnd = h;
    PlaceWindow(h, kW, kH, owner);
    AddModeless(h);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetFocus(c->hStart);
}
