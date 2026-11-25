// ndm_optdlg.cpp -- NDM Options (modal, compact, vertical tab rail)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Every control maps to an NdmConfigMgr field; Save writes the INI and
// applies the change to the running app immediately.

#include "ndm_optdlg.h"
#include "ndm_theme.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"
#include "ndm_firstrun.h"
#include "ndm_dialogs.h"
#include "ndm_uilogic.h"

#include <shellapi.h>
#include <string>

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass = L"NDMOptionsWnd";
    const wchar_t* kRailClass = L"NDMOptionsRail";
    const int kW = 540, kH = 256;          // client, 96-dpi units
    const int kRailW = 136;
    const int kTabs = 5;
    const int kFieldH = 26;
    const int kFooterY = kH - 46;
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);

    enum
    {
        IDC_FOLDER = 500, IDC_BROWSE, IDC_MAXDL_MINUS, IDC_MAXDL, IDC_MAXDL_PLUS,
        IDC_STARTUP, IDC_TRAY, IDC_CONN_MINUS, IDC_CONN, IDC_CONN_PLUS, IDC_SPEED,
        IDC_MONITOR, IDC_SAVE, IDC_CANCEL_BTN, IDC_MAIL,
        IDC_FILEINFO, IDC_CHROME, IDC_FIREFOX, IDC_NDLG, IDC_NTRAY, IDC_NSOUND, IDC_NOTICES,
        IDC_CLIPBOARD
    };
    enum { TAB_GENERAL, TAB_CONN, TAB_BROWSER, TAB_NOTIFY, TAB_ABOUT };

    struct Ctx
    {
        HWND      hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        int       tab = 0, hotTab = -1;
        bool      saved = false, done = false;
        HWND hRail = nullptr;            // page list: a tab stop (arrows, Home / End)
        HWND hFolder, hBrowse, hMaxMinus, hMax, hMaxPlus, hStartup, hTray,
             hConnMinus, hConn, hConnPlus, hSpeed, hMonitor, hSave, hCancel, hMail,
             hFileInfo, hChrome, hFirefox, hNDlg, hNTray, hNSound, hNotices, hClip;
        int  hint = -1;                  // browser setup hint: -1 none, 0 chrome, 1 firefox
        RECT rcTabs[kTabs] = {};
        RECT rcFolder{}, rcMax{}, rcConn{}, rcSpeed{};
        std::wstring error;
        int S(int v) const { return res.S(v); }
    };

    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    const wchar_t* kTabName[kTabs] = { L"General", L"Connection", L"Browser", L"Notifications", L"About" };
    const NdmGlyph kTabGlyph[kTabs] = { GLY_OPTIONS, GLY_GLOBE, GLY_BROWSER, GLY_FINISHED, GLY_INFO };

    int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void SetNum(HWND e, int v) { SetWindowTextW(e, std::to_wstring(v).c_str()); }
    int  GetNum(HWND e) { return _wtoi(GetText(e).c_str()); }

    void ShowPage(Ctx* c);

    // Switch page (rail click, arrows, Ctrl+Tab). Focus left on a control
    // of the page that was hidden moves to the rail.
    void SelectTab(Ctx* c, int tab)
    {
        tab = (tab % kTabs + kTabs) % kTabs;
        if (tab == c->tab) return;
        c->tab = tab;
        ShowPage(c);
        HWND f = GetFocus();
        if (c->hRail && (!f || !IsWindowVisible(f))) SetFocus(c->hRail);
        if (c->hRail)
        {
            InvalidateRect(c->hRail, nullptr, FALSE);
            SetAccName(c->hRail, (std::wstring(L"Options page: ") + kTabName[tab]).c_str());
        }
    }

    void ShowPage(Ctx* c)
    {
        auto vis = [](HWND h, bool v) { ShowWindow(h, v ? SW_SHOW : SW_HIDE); };
        bool g = c->tab == TAB_GENERAL, n = c->tab == TAB_CONN, b = c->tab == TAB_BROWSER,
             nt = c->tab == TAB_NOTIFY, a = c->tab == TAB_ABOUT;
        vis(c->hFolder, g); vis(c->hBrowse, g); vis(c->hMaxMinus, g); vis(c->hMax, g);
        vis(c->hMaxPlus, g); vis(c->hStartup, g); vis(c->hTray, g); vis(c->hClip, g);
        vis(c->hConnMinus, n); vis(c->hConn, n); vis(c->hConnPlus, n); vis(c->hSpeed, n);
        vis(c->hMonitor, b); vis(c->hFileInfo, b); vis(c->hChrome, b); vis(c->hFirefox, b);
        vis(c->hNDlg, nt); vis(c->hNTray, nt); vis(c->hNSound, nt);
        vis(c->hMail, a); vis(c->hNotices, a);
        InvalidateRect(c->hwnd, nullptr, FALSE);
        if (c->hRail) InvalidateRect(c->hRail, nullptr, FALSE);
    }

    // [-][ n ][+] stepper right-aligned at R; returns the number field rect
    RECT PlaceStepper(Ctx* c, HWND minus, HWND edit, HWND plus, int y, int R)
    {
        int fh = c->S(kFieldH), sw = fh;
        RECT f = { R - sw - c->S(40), y, R - sw, y + fh };
        MoveWindow(plus,  R - sw, y, sw, fh, TRUE);
        MoveWindow(minus, f.left - sw, y, sw, fh, TRUE);
        PlaceEdit(edit, f, c->res.bold, c->S(4));
        return f;
    }

    void Layout(Ctx* c)
    {
        for (int i = 0; i < kTabs; ++i)
            c->rcTabs[i] = { c->S(8), c->S(12) + i * c->S(30), c->S(kRailW - 8), c->S(12) + i * c->S(30) + c->S(28) };
        if (c->hRail) MoveWindow(c->hRail, 0, 0, c->S(kRailW), c->S(kH), TRUE);

        int L = c->S(kRailW + 18), R = c->S(kW - 18);
        int fh = c->S(kFieldH);
        // General
        c->rcFolder = { L, c->S(34), R - fh - c->S(6), c->S(34) + fh };
        MoveWindow(c->hBrowse, R - fh, c->rcFolder.top, fh, fh, TRUE);
        PlaceEdit(c->hFolder, c->rcFolder, c->res.ui, c->S(8));
        c->rcMax = PlaceStepper(c, c->hMaxMinus, c->hMax, c->hMaxPlus, c->S(74), R);
        MoveWindow(c->hStartup, L, c->S(114), R - L, c->S(24), TRUE);
        MoveWindow(c->hTray,    L, c->S(144), R - L, c->S(24), TRUE);
        MoveWindow(c->hClip,    L, c->S(174), R - L, c->S(24), TRUE);
        // Connection
        c->rcConn = PlaceStepper(c, c->hConnMinus, c->hConn, c->hConnPlus, c->S(14), R);
        c->rcSpeed = { R - c->S(92), c->S(52), R, c->S(52) + fh };
        PlaceEdit(c->hSpeed, c->rcSpeed, c->res.bold, c->S(8));
        // Browser
        MoveWindow(c->hMonitor,  L, c->S(14), R - L, c->S(24), TRUE);
        MoveWindow(c->hFileInfo, L, c->S(44), R - L, c->S(24), TRUE);
        MoveWindow(c->hFirefox, R - c->S(80), c->S(86), c->S(80), fh, TRUE);
        MoveWindow(c->hChrome,  R - c->S(80) - c->S(8) - c->S(108), c->S(86), c->S(108), fh, TRUE);
        // Notifications
        MoveWindow(c->hNDlg,   L, c->S(14), R - L, c->S(24), TRUE);
        MoveWindow(c->hNTray,  L, c->S(44), R - L, c->S(24), TRUE);
        MoveWindow(c->hNSound, L, c->S(74), R - L, c->S(24), TRUE);
        // About
        int cx = (L + R) / 2;
        MoveWindow(c->hNotices, cx - c->S(158), c->S(170), c->S(124), c->S(24), TRUE);
        MoveWindow(c->hMail,    cx - c->S(26), c->S(170), c->S(184), c->S(24), TRUE);

        int bh = c->S(28), by = c->S(kFooterY) + (c->S(kH - kFooterY) - bh) / 2;
        MoveWindow(c->hSave,   R - c->S(84), by, c->S(84), bh, TRUE);
        MoveWindow(c->hCancel, R - c->S(84) - c->S(8) - c->S(76), by, c->S(76), bh, TRUE);

        HWND all[] = { c->hBrowse, c->hMaxMinus, c->hMaxPlus, c->hConnMinus, c->hConnPlus,
                       c->hSave, c->hCancel, c->hChrome, c->hFirefox };
        for (HWND b : all) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
        HWND tg[] = { c->hStartup, c->hTray, c->hMonitor, c->hFileInfo, c->hNDlg, c->hNTray,
                      c->hNSound, c->hMail, c->hNotices, c->hClip };
        for (HWND b : tg) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.ui, FALSE);
    }

    void SetCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    bool IsOn(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void Load(Ctx* c)
    {
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        SetWindowTextW(c->hFolder, cfg.GetSavePath().c_str());
        SetNum(c->hMax,  Clamp(cfg.GetMaxDownloads(), 1, 16));
        SetNum(c->hConn, Clamp(cfg.GetMaxConnections(), 1, 32));
        SetNum(c->hSpeed, (int)(cfg.GetSpeedLimit() / 1024));
        SendMessageW(c->hStartup, BM_SETCHECK, cfg.GetStartWithWin()   ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hTray,    BM_SETCHECK, cfg.GetCloseToTray()    ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hMonitor, BM_SETCHECK, cfg.GetBrowserMonitor() ? BST_CHECKED : BST_UNCHECKED, 0);
        SetCheck(c->hFileInfo, NdmPrefs::FileInfo());
        SetCheck(c->hNDlg,     NdmPrefs::NotifyDialog());
        SetCheck(c->hNTray,    NdmPrefs::NotifyTray());
        SetCheck(c->hNSound,   NdmPrefs::NotifySound());
        SetCheck(c->hClip,     NdmPrefs::WatchClipboard());
    }

    bool Save(Ctx* c)
    {
        std::wstring folder = GetText(c->hFolder);
        while (!folder.empty() && (folder.back() == L' ')) folder.pop_back();
        if (folder.empty() || !EnsureFolder(folder))
        {
            c->error = L"The download folder is empty or cannot be created.";
            SelectTab(c, TAB_GENERAL);
            SetFocus(c->hFolder);
            return false;
        }
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        cfg.SetSavePath(folder);
        cfg.SetMaxDownloads(Clamp(GetNum(c->hMax), 1, 16));
        cfg.SetMaxConnections(Clamp(GetNum(c->hConn), 1, 32));
        int kb = Clamp(GetNum(c->hSpeed), 0, 4000000);
        cfg.SetSpeedLimit((DWORD)kb * 1024u);
        cfg.SetStartWithWin(SendMessageW(c->hStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.SetCloseToTray(SendMessageW(c->hTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.SetBrowserMonitor(SendMessageW(c->hMonitor, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.Save();
        NdmPrefs::Set(NdmPrefs::kFileInfo,     IsOn(c->hFileInfo));
        NdmPrefs::Set(NdmPrefs::kNotifyDialog, IsOn(c->hNDlg));
        NdmPrefs::Set(NdmPrefs::kNotifyTray,   IsOn(c->hNTray));
        NdmPrefs::Set(NdmPrefs::kNotifySound,  IsOn(c->hNSound));
        NdmPrefs::Set(NdmPrefs::kWatchClipboard, IsOn(c->hClip));
        NdmOptDlg::ApplySettings();
        return true;
    }

    // The page rail (its own child window so it can take keyboard focus)
    void PaintRail(Ctx* c, HDC dc, const RECT& cl)
    {
        const NdmDlgRes& r = c->res;
        Fill(dc, cl, RGB(21, 23, 28));
        VLine(dc, cl.right - 1, 0, cl.bottom, NDM_CLR_BORDER);
        bool focus = GetFocus() == c->hRail;
        for (int i = 0; i < kTabs; ++i)
        {
            RECT t = c->rcTabs[i];
            bool sel = i == c->tab, hot = i == c->hotTab;
            if (sel)
            {
                RoundFillV(dc, t, c->S(5), RGB(58, 51, 20), RGB(46, 41, 17));
                RECT bar = { t.left, t.top + c->S(7), t.left + c->S(3), t.bottom - c->S(7) };
                RoundFill(dc, bar, c->S(1), NDM_CLR_GOLD);
                if (focus) RoundFrame(dc, t, c->S(5), NDM_CLR_GOLD, 1.0f, 200);   // focus ring
            }
            else if (hot) RoundFill(dc, t, c->S(5), NDM_CLR_BG_HOVER);
            RECT g = { t.left + c->S(10), t.top + (t.bottom - t.top - c->S(16)) / 2, 0, 0 };
            g.right = g.left + c->S(16); g.bottom = g.top + c->S(16);
            Glyph(dc, kTabGlyph[i], g, sel ? NDM_CLR_GOLD_LIGHT : RGB(168, 150, 96), 1.0f);
            RECT tt = { g.right + c->S(8), t.top, t.right - c->S(4), t.bottom };
            Text(dc, kTabName[i], tt, sel ? NDM_CLR_GOLD_LIGHT : NDM_CLR_FG, sel ? r.bold : r.ui,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    LRESULT CALLBACK RailProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        Ctx* c = (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT cl; GetClientRect(h, &cl);
            if (c)
            {
                NdmMemDC mem(hdc, cl);
                PaintRail(c, mem.ok() ? mem.dc : hdc, cl);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (!c) break;
            switch (w)
            {
            case VK_UP:   case VK_LEFT:  SelectTab(c, c->tab - 1); return 0;
            case VK_DOWN: case VK_RIGHT: SelectTab(c, c->tab + 1); return 0;
            case VK_HOME:                SelectTab(c, 0); return 0;
            case VK_END:                 SelectTab(c, kTabs - 1); return 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_MOUSEMOVE:
        {
            if (!c) break;
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            int hot = -1;
            for (int i = 0; i < kTabs; ++i) if (PtInRect(&c->rcTabs[i], pt)) hot = i;
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
            if (c && c->hotTab >= 0) { c->hotTab = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_SETCURSOR:
        {
            if (!c) break;
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            for (int i = 0; i < kTabs; ++i)
                if (PtInRect(&c->rcTabs[i], pt)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN:
        {
            if (!c) break;
            POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
            for (int i = 0; i < kTabs; ++i)
                if (PtInRect(&c->rcTabs[i], pt))
                {
                    SetFocus(h);
                    SelectTab(c, i);
                }
            return 0;
        }
        }
        return DefWindowProcW(h, m, w, l);
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
            Fill(dc, cl, kBg);                // the rail paints itself (child window)

            int L = c->S(kRailW + 18), R = cl.right - c->S(18);
            HWND f = GetFocus();
            // label on the left of a right-aligned control
            auto label = [&](const RECT& ctl, int left, const wchar_t* t)
            {
                RECT a = { left, ctl.top, ctl.left - c->S(36), ctl.bottom };
                Text(dc, t, a, NDM_CLR_FG, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            };

            switch (c->tab)
            {
            case 0:
            {
                RECT cr = { L, c->S(12), R, c->S(30) };
                Text(dc, L"Download folder", cr, NDM_CLR_FG, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                DrawField(dc, c->rcFolder, f == c->hFolder, false, r.dpi);
                label(c->rcMax, L, L"Simultaneous downloads");
                DrawField(dc, c->rcMax, f == c->hMax, false, r.dpi);
                break;
            }
            case 1:
                label(c->rcConn, L, L"Connections per download");
                DrawField(dc, c->rcConn, f == c->hConn, false, r.dpi);
                {
                    RECT sr = c->rcSpeed; sr.left += c->S(30);   // label clearance like steppers
                    label(sr, L, L"Speed limit, KB/s (0 = unlimited)");
                }
                DrawField(dc, c->rcSpeed, f == c->hSpeed, false, r.dpi);
                break;
            case TAB_BROWSER:
            {
                RECT br = { L, c->S(86), c->S(kW - 18) - c->S(204), c->S(86) + c->S(kFieldH) };
                Text(dc, L"Add NDM to your browser", br, NDM_CLR_FG, r.ui,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (c->hint >= 0)
                {
                    RECT hr = { L, c->S(118), R, c->S(136) };
                    Text(dc, NdmUi::BrowserHint(c->hint == 1), hr, RGB(176, 150, 76), r.sm,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (const wchar_t* note = NdmUi::BrowserNote(c->hint == 1))
                    {
                        RECT nr = { L, c->S(138), R, c->S(170) };
                        Text(dc, note, nr, NDM_CLR_FG_DIM, r.sm, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                    }
                }
                break;
            }
            case TAB_ABOUT:
            {
                int cx = (L + R) / 2, ls = c->S(40);
                RECT lg = { cx - ls / 2, c->S(12), cx - ls / 2 + ls, c->S(12) + ls };
                Logo(dc, lg);
                const UINT ctr = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                auto line = [&](int y, int hgt, const wchar_t* s, COLORREF col, HFONT f)
                {
                    RECT lr = { L, c->S(y), R, c->S(y + hgt) };
                    Text(dc, s, lr, col, f, ctr);
                };
                line(56,  20, NDM_PRODUCT_NAME L"  ·  v" NDM_VERSION_STR_W, NDM_CLR_FG, r.title);
                line(78,  16, NDM_TAGLINE, NDM_CLR_GOLD_LIGHT, r.sm);
                line(96,  16, L"© 2026 Devnetra Consultancy  ·  MIT License", NDM_CLR_FG, r.sm);
                line(122, 16, L"Third-party: yt-dlp (Unlicense) · FFmpeg (GPL/LGPL)", NDM_CLR_FG_DIM, r.sm);
                line(138, 16, L"aria2 (GPL-2.0) · Deno (MIT)", NDM_CLR_FG_DIM, r.sm);
                break;
            }
            }

            RECT ft = { c->S(kRailW), c->S(kFooterY), cl.right, cl.bottom };
            Fill(dc, ft, kFootBg);
            HLine(dc, ft.left, cl.right, ft.top, RGB(40, 44, 53));
            if (!c->error.empty())
            {
                RECT er = { L, ft.top, R - c->S(176), cl.bottom };
                Text(dc, c->error, er, RGB(236, 110, 110), r.sm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        EndPaint(c->hwnd, &ps);
    }

    void Close(Ctx* c)
    {
        if (c->done) return;
        c->done = true;
        if (c->owner && IsWindow(c->owner)) EnableWindow(c->owner, TRUE);
        DestroyWindow(c->hwnd);
    }

    void Step(Ctx* /*c*/, HWND e, int d, int lo, int hi) { SetNum(e, Clamp(GetNum(e) + d, lo, hi)); }

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
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            // creation order = tab order: the page rail first
            c->hRail = CreateWindowExW(0, kRailClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 10, 10, h, nullptr, GetModuleHandleW(nullptr), c);
            SetAccName(c->hRail, (std::wstring(L"Options page: ") + kTabName[c->tab]).c_str());
            c->hFolder    = MakeEdit(h, IDC_FOLDER);
            c->hBrowse    = MakeBtn(h, IDC_BROWSE, L"", GLY_FOLDER);
            c->hMaxMinus  = MakeBtn(h, IDC_MAXDL_MINUS, L"", GLY_MINUS);
            c->hMax       = MakeEdit(h, IDC_MAXDL, ES_NUMBER | ES_CENTER);
            c->hMaxPlus   = MakeBtn(h, IDC_MAXDL_PLUS, L"", GLY_PLUS);
            c->hStartup   = MakeBtn(h, IDC_STARTUP, L"Start with Windows", GLY_NONE, true);
            c->hTray      = MakeBtn(h, IDC_TRAY, L"Keep running in the tray when closed", GLY_NONE, true);
            c->hClip      = MakeBtn(h, IDC_CLIPBOARD, L"Watch clipboard for download links", GLY_NONE, true);
            c->hConnMinus = MakeBtn(h, IDC_CONN_MINUS, L"", GLY_MINUS);
            c->hConn      = MakeEdit(h, IDC_CONN, ES_NUMBER | ES_CENTER);
            c->hConnPlus  = MakeBtn(h, IDC_CONN_PLUS, L"", GLY_PLUS);
            c->hSpeed     = MakeEdit(h, IDC_SPEED, ES_NUMBER | ES_RIGHT);
            c->hMonitor   = MakeBtn(h, IDC_MONITOR, L"Capture downloads from browsers", GLY_NONE, true);
            c->hFileInfo  = MakeBtn(h, IDC_FILEINFO, L"Show “Download File Info” for browser downloads", GLY_NONE, true);
            c->hChrome    = MakeBtn(h, IDC_CHROME, L"Chrome / Edge");
            c->hFirefox   = MakeBtn(h, IDC_FIREFOX, L"Firefox");
            c->hNDlg      = MakeBtn(h, IDC_NDLG, L"Show “Download complete” window", GLY_NONE, true);
            c->hNTray     = MakeBtn(h, IDC_NTRAY, L"Tray notification when a download finishes", GLY_NONE, true);
            c->hNSound    = MakeBtn(h, IDC_NSOUND, L"Play a sound when a download finishes", GLY_NONE, true);
            c->hNotices   = MakeBtn(h, IDC_NOTICES, L"Third-party notices");
            c->hMail      = MakeBtn(h, IDC_MAIL, NDM_CONTACT_EMAIL);
            c->hCancel    = MakeBtn(h, IDC_CANCEL_BTN, L"Cancel");
            c->hSave      = MakeBtn(h, IDC_SAVE, L"Save");
            // captions are painted by the window: name the controls for screen readers
            SetAccName(c->hFolder, L"Download folder");
            SetAccName(c->hBrowse, L"Choose folder");
            SetAccName(c->hMax, L"Simultaneous downloads");
            SetAccName(c->hMaxMinus, L"Fewer simultaneous downloads");
            SetAccName(c->hMaxPlus, L"More simultaneous downloads");
            SetAccName(c->hConn, L"Connections per download");
            SetAccName(c->hConnMinus, L"Fewer connections");
            SetAccName(c->hConnPlus, L"More connections");
            SetAccName(c->hSpeed, L"Speed limit, KB/s (0 = unlimited)");
            SendMessageW(c->hMax, EM_LIMITTEXT, 2, 0);
            SendMessageW(c->hConn, EM_LIMITTEXT, 2, 0);
            SendMessageW(c->hSpeed, EM_LIMITTEXT, 7, 0);
            SendMessageW(c->hFolder, EM_LIMITTEXT, MAX_PATH, 0);
            Layout(c);
            Load(c);
            ShowPage(c);
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
            NdmBtnKind k = BTN_SECONDARY;
            COLORREF bg = kBg;
            switch (di->CtlID)
            {
            case IDC_SAVE:       k = BTN_PRIMARY; bg = kFootBg; break;
            case IDC_CANCEL_BTN: k = BTN_GHOST;   bg = kFootBg; break;
            case IDC_MAIL:       k = BTN_GHOST; break;
            case IDC_STARTUP: case IDC_TRAY: case IDC_MONITOR: k = BTN_TOGGLE; break;
            case IDC_FILEINFO: case IDC_NDLG: case IDC_NTRAY: case IDC_NSOUND: k = BTN_TOGGLE; break;
            case IDC_CLIPBOARD: k = BTN_TOGGLE; break;
            case IDC_NOTICES:    k = BTN_GHOST; break;
            case IDC_BROWSE:
            case IDC_MAXDL_MINUS: case IDC_MAXDL_PLUS:
            case IDC_CONN_MINUS:  case IDC_CONN_PLUS: k = BTN_STEPPER; break;
            }
            bool light = k == BTN_TOGGLE || di->CtlID == IDC_MAIL || di->CtlID == IDC_NOTICES;
            DrawButton(di, k, bg, light ? c->res.ui : c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_NDM_CTRLTAB:
            // Ctrl+Tab / Ctrl+Shift+Tab anywhere in the window: next / previous page
            SelectTab(c, c->tab + (w ? -1 : 1));
            return 1;
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id)
            {
            case IDC_BROWSE:
            {
                std::wstring d = BrowseFolder(h, GetText(c->hFolder), L"Choose the default download folder");
                if (!d.empty()) SetWindowTextW(c->hFolder, d.c_str());
                return 0;
            }
            case IDC_MAXDL_MINUS: Step(c, c->hMax, -1, 1, 16); return 0;
            case IDC_MAXDL_PLUS:  Step(c, c->hMax, +1, 1, 16); return 0;
            case IDC_CONN_MINUS:  Step(c, c->hConn, -1, 1, 32); return 0;
            case IDC_CONN_PLUS:   Step(c, c->hConn, +1, 1, 32); return 0;
            case IDC_MONITOR:     InvalidateRect(h, nullptr, FALSE); return 0;
            case IDC_MAIL:
                ShellExecuteW(h, L"open", L"mailto:" NDM_CONTACT_EMAIL, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case IDC_NOTICES:
            {
                std::wstring f = JoinPath(NdmFirstRun::InstallDir(), L"THIRD_PARTY_NOTICES.txt");
                if ((INT_PTR)ShellExecuteW(h, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
                    NdmDialogs::Notice(h, L"Third-party notices", L"The file could not be found:\n\n" + f, GLY_FILE);
                return 0;
            }
            case IDC_CHROME:
            case IDC_FIREFOX:
                c->hint = id == IDC_FIREFOX ? 1 : 0;
                InvalidateRect(h, nullptr, FALSE);
                NdmFirstRun::OpenBrowserSetup(h, c->hint == 1);
                return 0;
            case IDOK:
            case IDC_SAVE:
                if (Save(c)) { c->saved = true; Close(c); }
                else InvalidateRect(h, nullptr, FALSE);
                return 0;
            case IDCANCEL:
            case IDC_CANCEL_BTN:
                Close(c);
                return 0;
            case IDC_MAXDL: if (code == EN_KILLFOCUS) Step(c, c->hMax, 0, 1, 16); break;
            case IDC_CONN:  if (code == EN_KILLFOCUS) Step(c, c->hConn, 0, 1, 32); break;
            }
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
            if (code == EN_CHANGE && !c->error.empty()) { c->error.clear(); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_DPICHANGED:
        {
            c->res.Create(HIWORD(w));
            SetWindowIcons(h, HIWORD(w));
            RECT* nr = (RECT*)l;
            SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            Close(c);
            return 0;
        case WM_DESTROY:
            RemoveModeless(h);
            if (c && !c->done && c->owner && IsWindow(c->owner)) EnableWindow(c->owner, TRUE);
            return 0;
        case WM_NCDESTROY:
            if (c) { c->res.Free(); SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
            break;   // Ctx lives on Show()'s stack
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND g_open = nullptr;
}

// ===========================================================================
// Public
// ===========================================================================
bool NdmOptDlg::Show(HWND parent, int initialTab)
{
    if (g_open && IsWindow(g_open)) { ForceForeground(g_open); return false; }

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
        WNDCLASSEXW rc = { sizeof(rc) };
        rc.lpfnWndProc   = RailProc;
        rc.hInstance     = hi;
        rc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        rc.lpszClassName = kRailClass;
        reg = (RegisterClassExW(&rc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) && reg;
    }

    Ctx ctx;
    ctx.owner = parent;
    ctx.tab   = Clamp(initialTab, 0, kTabs - 1);
    bool ownerVisible = parent && IsWindowVisible(parent) && !IsIconic(parent);
    POINT pt = CreationPoint(parent);
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | (ownerVisible ? 0 : WS_EX_APPWINDOW),
                             kClass, L"NDM — Options",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, parent, nullptr, hi, &ctx);
    if (!h) return false;
    g_open = h;
    PlaceWindow(h, kW, kH, parent);
    AddModeless(h);
    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetFocus(ctx.hSave);

    MSG msg;
    while (IsWindow(h))
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage((int)msg.wParam); break; }
        if (r == -1) break;
        if (NdmTheme::PreTranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(h)) { ctx.done = true; DestroyWindow(h); }
    if (parent && IsWindow(parent))
    {
        EnableWindow(parent, TRUE);
        if (IsWindowVisible(parent)) SetForegroundWindow(parent);
    }
    g_open = nullptr;
    return ctx.saved;
}

void NdmOptDlg::ApplyRunKey()
{
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hRun) != ERROR_SUCCESS)
        return;
    if (NdmConfigMgr::GetInstance().GetStartWithWin())
    {
        wchar_t exe[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" -tray";
            RegSetValueExW(hRun, NDM_PRODUCT_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
    }
    else
        RegDeleteValueW(hRun, NDM_PRODUCT_NAME);
    RegCloseKey(hRun);
}

void NdmOptDlg::ApplySettings()
{
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    NdmQueue& q = NdmQueue::GetInstance();
    q.SetMaxActiveDownloads(cfg.GetMaxDownloads());
    q.ScheduleNext();                      // also applies the speed limit
    ApplyRunKey();

    // Publish the capture switch for the browser bridge / extension host.
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, NDM_REG_ROOT, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = cfg.GetBrowserMonitor() ? 1 : 0;
        RegSetValueExW(k, L"BrowserMonitor", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
    EnsureFolder(cfg.GetSavePath());
}
