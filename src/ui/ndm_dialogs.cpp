// ndm_dialogs.cpp -- small themed dialogs (see ndm_dialogs.h)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Ask / Input are modal (owner disabled, local message loop, Ctx on the
// caller's stack). The refresh window is modeless; its network check runs
// on a worker thread that only posts a bare message back (the result lives
// in a shared object the window and the thread both hold).

#include "ndm_dialogs.h"
#include "ndm_uilogic.h"
#include "ndm_mainwnd.h"
#include "ndm_adddlg.h"
#include "ndm_progdlg.h"
#include "../core/ndm_queue.h"

#include <algorithm>
#include <map>
#include <memory>
#include <thread>

using namespace NdmTheme;

namespace
{
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);
    const int kFootH = 46;

    HWND MakeWindow(const wchar_t* cls, WNDPROC proc, const wchar_t* title, HWND owner,
                    void* ctx, bool modal, bool topmost = false)
    {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = { sizeof(wc) };
        if (!GetClassInfoExW(hi, cls, &wc))
        {
            wc = { sizeof(wc) };
            wc.lpfnWndProc   = proc;
            wc.hInstance     = hi;
            wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = cls;
            RegisterClassExW(&wc);
        }
        bool ownerVisible = owner && IsWindowVisible(owner) && !IsIconic(owner);
        DWORD ex = WS_EX_CONTROLPARENT | (modal ? WS_EX_DLGMODALFRAME : 0) |
                   (ownerVisible && modal ? 0 : WS_EX_APPWINDOW) | (topmost ? WS_EX_TOPMOST : 0);
        DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (modal ? 0 : WS_MINIMIZEBOX);
        POINT pt = CreationPoint(owner);
        return CreateWindowExW(ex, cls, title, st, pt.x, pt.y, 100, 100,
                               modal ? owner : nullptr, nullptr, hi, ctx);
    }

    // Runs a modal loop until done is set or the window is gone.
    void RunModal(HWND h, HWND owner, const bool& done, HWND focus)
    {
        AddModeless(h);
        if (owner) EnableWindow(owner, FALSE);
        ShowWindow(h, SW_SHOW);
        ForceForeground(h);
        if (focus) SetFocus(focus);
        MSG msg;
        while (!done && IsWindow(h))
        {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r == 0) { PostQuitMessage((int)msg.wParam); break; }
            if (r == -1) break;
            if (PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);   // before destroy: activation returns
        if (IsWindow(h)) DestroyWindow(h);
        if (owner && IsWindow(owner) && IsWindowVisible(owner)) SetForegroundWindow(owner);
    }

    int TextWidth(HFONT f, const std::wstring& s)
    {
        HDC dc = GetDC(nullptr);
        SIZE sz = MeasureText(dc, s, f);
        ReleaseDC(nullptr, dc);
        return sz.cx;
    }

    void Footer(HDC dc, const RECT& cl, int top)
    {
        RECT ft = { 0, top, cl.right, cl.bottom };
        Fill(dc, ft, kFootBg);
        HLine(dc, 0, cl.right, top, RGB(40, 44, 53));
    }

    // ======================================================================
    // Ask
    // ======================================================================
    const int kAskW = 400, kAskM = 16, kAskTile = 30;

    struct AskCtx
    {
        HWND hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        std::wstring text;
        std::vector<std::wstring> labels;    // screen order, left to right
        std::vector<HWND> btns;              // btns[i] <-> labels[i]
        int  def = 0, cancel = -1, danger = -1;
        NdmGlyph glyph = GLY_INFO;
        int  result = -1;
        bool done = false;
        int  textH = 0;                      // px at res.dpi
        int S(int v) const { return res.S(v); }
    };

    int AskHeight96(AskCtx* c)
    {
        HDC dc = GetDC(nullptr);
        RECT r = { 0, 0, c->S(kAskW - kAskM * 2 - kAskTile - 12), 0 };
        HGDIOBJ o = SelectObject(dc, c->res.ui);
        DrawTextW(dc, c->text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, o);
        ReleaseDC(nullptr, dc);
        c->textH = (std::max)((int)(r.bottom - r.top), c->S(kAskTile));
        return MulDiv(c->textH, 96, (int)c->res.dpi) + kAskM * 2 + kFootH;
    }

    void AskLayout(AskCtx* c)
    {
        RECT cl; GetClientRect(c->hwnd, &cl);
        int bh = c->S(28), by = cl.bottom - c->S(kFootH) + (c->S(kFootH) - bh) / 2;
        int x = cl.right - c->S(kAskM);
        for (size_t k = c->btns.size(); k-- > 0;)          // right to left
        {
            int w = (std::max)(c->S(76), TextWidth(c->res.bold, c->labels[k]) + c->S(28));
            MoveWindow(c->btns[k], x - w, by, w, bh, TRUE);
            SendMessageW(c->btns[k], WM_SETFONT, (WPARAM)c->res.bold, FALSE);
            x -= w + c->S(8);
        }
    }

    LRESULT CALLBACK AskProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        AskCtx* c = (AskCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m)
        {
        case WM_NCCREATE:
            c = (AskCtx*)((CREATESTRUCTW*)l)->lpCreateParams;
            c->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
            break;
        case WM_CREATE:
        {
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            c->btns.resize(c->labels.size());
            // tab order = screen order (left to right)
            for (size_t k = 0; k < c->labels.size(); ++k)
                c->btns[k] = MakeBtn(h, 100 + (int)k, c->labels[k].c_str());
            return 0;
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
                Fill(dc, cl, kBg);
                RECT tile = { c->S(kAskM), c->S(kAskM), c->S(kAskM + kAskTile), c->S(kAskM + kAskTile) };
                GlyphTile(dc, tile, c->glyph, c->res.dpi);
                RECT tr = { tile.right + c->S(12), c->S(kAskM), cl.right - c->S(kAskM), c->S(kAskM) + c->textH };
                if (c->textH <= c->S(kAskTile))              // short text: centre on the tile
                {
                    RECT probe = tr;
                    HGDIOBJ o = SelectObject(dc, c->res.ui);
                    DrawTextW(dc, c->text.c_str(), -1, &probe, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    SelectObject(dc, o);
                    int th = probe.bottom - probe.top;
                    tr.top += (c->S(kAskTile) - th) / 2;
                }
                Text(dc, c->text, tr, NDM_CLR_FG, c->res.ui, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                Footer(dc, cl, cl.bottom - c->S(kFootH));
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            int i = (int)di->CtlID - 100;
            NdmBtnKind k = BTN_SECONDARY;
            if (i >= 0 && i < (int)c->labels.size())
                switch (NdmUi::AskButtonStyle(i, c->def, c->danger, c->labels[i]))
                {
                case NdmUi::ASK_PRIMARY:   k = BTN_PRIMARY; break;
                case NdmUi::ASK_DANGER:    k = BTN_DANGER;  break;
                case NdmUi::ASK_GHOST:     k = BTN_GHOST;   break;
                case NdmUi::ASK_SECONDARY: k = BTN_SECONDARY; break;
                }
            DrawButton(di, k, kFootBg, c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(w);
            const int n = (int)c->labels.size();
            if (id == IDOK || id == IDCANCEL)
            {
                // Enter: the focused button, else the default one. Esc: cancel.
                HWND f = GetFocus();
                int focused = -1;
                for (int i = 0; i < (int)c->btns.size(); ++i) if (c->btns[i] == f) focused = i;
                c->result = NdmUi::AskKeyResult(id == IDOK, focused, c->def, c->cancel, n);
                c->done = true;
                return 0;
            }
            if (id >= 100 && id < 100 + n)
            {
                c->result = id - 100;
                c->done = true;
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
            int h96 = AskHeight96(c);
            RECT wr; GetWindowRect(h, &wr);
            RECT r = { 0, 0, c->S(kAskW), c->S(h96) };
            AdjustRectForDpi(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE), c->res.dpi);
            SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            AskLayout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            c->result = NdmUi::AskKeyResult(false, -1, c->def, c->cancel, (int)c->labels.size());
            c->done = true;
            return 0;
        case WM_DESTROY:
            RemoveModeless(h);
            return 0;
        case WM_NCDESTROY:
            if (c) { c->res.Free(); SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }

    // ======================================================================
    // Input
    // ======================================================================
    const int kInW = 400, kInH = 134, kInM = 16;
    enum { IDC_IN_EDIT = 200, IDC_IN_OK, IDC_IN_CANCEL };

    struct InCtx
    {
        HWND hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        std::wstring label, suffix, text, error;
        bool numeric = false, selectStem = false;
        bool done = false, ok = false;
        const std::function<std::wstring(const std::wstring&)>* validate = nullptr;
        HWND hEdit = nullptr, hOk = nullptr, hCancel = nullptr;
        RECT rcField{};
        int S(int v) const { return res.S(v); }
    };

    void InLayout(InCtx* c)
    {
        int L = c->S(kInM), R = c->S(kInW - kInM);
        int sw = c->suffix.empty() ? 0 : TextWidth(c->res.ui, c->suffix) + c->S(10);
        c->rcField = { L, c->S(36), R - sw, c->S(36 + 26) };
        PlaceEdit(c->hEdit, c->rcField, c->res.ui, c->S(8));
        int bh = c->S(28), by = c->S(kInH - kFootH) + (c->S(kFootH) - bh) / 2;
        MoveWindow(c->hOk, R - c->S(84), by, c->S(84), bh, TRUE);
        MoveWindow(c->hCancel, R - c->S(84) - c->S(8) - c->S(76), by, c->S(76), bh, TRUE);
        SendMessageW(c->hOk, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
        SendMessageW(c->hCancel, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
    }

    LRESULT CALLBACK InProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        InCtx* c = (InCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m)
        {
        case WM_NCCREATE:
            c = (InCtx*)((CREATESTRUCTW*)l)->lpCreateParams;
            c->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
            break;
        case WM_CREATE:
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            c->hEdit   = MakeEdit(h, IDC_IN_EDIT, c->numeric ? ES_NUMBER : 0);
            SetAccName(c->hEdit, c->label.c_str());
            c->hCancel = MakeBtn(h, IDC_IN_CANCEL, L"Cancel");
            c->hOk     = MakeBtn(h, IDC_IN_OK, L"OK");
            SendMessageW(c->hEdit, EM_LIMITTEXT, c->numeric ? 9 : 240, 0);
            SetWindowTextW(c->hEdit, c->text.c_str());
            InLayout(c);
            return 0;
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
                Fill(dc, cl, kBg);
                int L = c->S(kInM), R = cl.right - c->S(kInM);
                RECT lr = { L, c->S(12), R, c->S(32) };
                Text(dc, c->label, lr, NDM_CLR_FG, c->res.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                DrawField(dc, c->rcField, GetFocus() == c->hEdit, false, c->res.dpi);
                if (!c->suffix.empty())
                {
                    RECT sr = { c->rcField.right + c->S(10), c->rcField.top, R, c->rcField.bottom };
                    Text(dc, c->suffix, sr, NDM_CLR_FG_DIM, c->res.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                if (!c->error.empty())
                {
                    RECT er = { L, c->S(66), R, c->S(84) };
                    Text(dc, c->error, er, RGB(236, 110, 110), c->res.sm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                Footer(dc, cl, c->S(kInH - kFootH));
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return (LRESULT)EditColors((HDC)w, (HWND)l, c->res);
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            DrawButton(di, di->CtlID == IDC_IN_OK ? BTN_PRIMARY : BTN_GHOST, kFootBg, c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == IDOK && GetFocus() == c->hCancel) id = IDCANCEL;
            if (id == IDOK || id == IDC_IN_OK)
            {
                std::wstring t = GetText(c->hEdit);
                std::wstring err = (c->validate && *c->validate) ? (*c->validate)(t) : std::wstring();
                if (!err.empty())
                {
                    c->error = err;
                    InvalidateRect(h, nullptr, FALSE);
                    SetFocus(c->hEdit);
                    return 0;
                }
                c->text = t;
                c->ok = true;
                c->done = true;
                return 0;
            }
            if (id == IDCANCEL || id == IDC_IN_CANCEL) { c->done = true; return 0; }
            if (id == IDC_IN_EDIT)
            {
                if (code == EN_CHANGE && !c->error.empty()) c->error.clear();
                if (code == EN_CHANGE || code == EN_SETFOCUS || code == EN_KILLFOCUS)
                    InvalidateRect(h, nullptr, FALSE);
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
            InLayout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            c->done = true;
            return 0;
        case WM_DESTROY:
            RemoveModeless(h);
            return 0;
        case WM_NCDESTROY:
            if (c) { c->res.Free(); SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }

    // ======================================================================
    // Refresh download address
    // ======================================================================
    const wchar_t* kRefClass = L"NDMRefreshWnd";
    const int kRefW = 460, kRefH = 158, kRefM = 14;
    const UINT WM_REF_DONE = WM_APP + 71;
    const UINT kRefTimer = 1;
    enum { IDC_REF_URL = 300, IDC_REF_OK, IDC_REF_CANCEL, IDC_REF_ADDNEW };

    struct RefShared                       // written by the worker, read after WM_REF_DONE
    {
        NdmRefreshResult res = NDM_REFRESH_NOT_FOUND;
        std::wstring     msg;
    };

    struct RefCtx
    {
        HWND hwnd = nullptr, owner = nullptr;
        DWORD id = 0;
        NdmDlgRes res;
        bool autoMode = false;
        NdmBrowserRequest req;               // auto mode: the browser's request
        std::wstring name, info;             // file name, "paused at 62% · 1.1 GB"
        std::wstring message;                // result / hint line
        int  msgKind = 0;                    // 0 dim, 1 error, 2 ok
        bool checking = false, failedAuto = false;
        int  phase = 0;
        std::shared_ptr<RefShared> shared;
        HWND hUrl = nullptr, hOk = nullptr, hCancel = nullptr, hAddNew = nullptr;
        RECT rcUrl{}, rcMsg{};
        int S(int v) const { return res.S(v); }
    };

    std::map<DWORD, HWND> g_refWins;
    volatile LONG g_refWorkers = 0;            // address checks running on worker threads

    void RefLayout(RefCtx* c)
    {
        int L = c->S(kRefM), R = c->S(kRefW - kRefM);
        c->rcUrl = { L, c->S(56), R, c->S(56 + 26) };
        PlaceEdit(c->hUrl, c->rcUrl, c->res.ui, c->S(8));
        c->rcMsg = { L, c->S(88), R, c->S(108) };
        int footY = kRefH - kFootH;
        int bh = c->S(28), by = c->S(footY) + (c->S(kFootH) - bh) / 2;
        int x = R;
        auto place = [&](HWND b, int w96)
        {
            if (!(GetWindowLongPtrW(b, GWL_STYLE) & WS_VISIBLE)) return;
            int w = (std::max)(c->S(w96), TextWidth(c->res.bold, GetText(b)) + c->S(28));
            MoveWindow(b, x - w, by, w, bh, TRUE);
            SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
            x -= w + c->S(8);
        };
        place(c->hAddNew, 96);
        place(c->hOk, 84);
        place(c->hCancel, 76);
    }

    void RefSetState(RefCtx* c)
    {
        bool showAdd = c->autoMode && c->failedAuto;
        ShowWindow(c->hAddNew, showAdd ? SW_SHOW : SW_HIDE);
        ShowWindow(c->hOk, showAdd ? SW_HIDE : SW_SHOW);
        SetWindowTextW(c->hCancel, showAdd ? L"Close" : L"Cancel");
        EnableWindow(c->hOk, !c->checking);
        SendMessageW(c->hUrl, EM_SETREADONLY, (c->checking || c->autoMode) ? TRUE : FALSE, 0);
        RefLayout(c);
        InvalidateRect(c->hwnd, nullptr, FALSE);
    }

    void RefStart(RefCtx* c)
    {
        std::wstring url = GetText(c->hUrl);
        while (!url.empty() && iswspace(url.back())) url.pop_back();
        while (!url.empty() && iswspace(url.front())) url.erase(url.begin());
        if (!NdmIsHttpUrl(url))
        {
            c->message = L"Enter an http:// or https:// link to this file.";
            c->msgKind = 1;
            InvalidateRect(c->hwnd, &c->rcMsg, FALSE);
            SetFocus(c->hUrl);
            return;
        }
        NdmDownload d;
        bool wasRunning = NdmQueue::GetInstance().GetSnapshot(c->id, d) &&
                          (d.status == NDM_STATUS_DOWNLOADING || d.status == NDM_STATUS_QUEUED);
        if (wasRunning) NdmQueue::GetInstance().PauseDownload(c->id);   // the core needs it stopped

        c->checking = true;
        c->message = L"Checking the new link…";
        c->msgKind = 0;
        c->shared = std::make_shared<RefShared>();
        std::shared_ptr<RefShared> sh = c->shared;
        HWND hwnd = c->hwnd;
        DWORD id = c->id;
        std::wstring ref = c->autoMode ? c->req.referrer : std::wstring();
        std::wstring ck  = c->autoMode ? c->req.cookies  : std::wstring();
        InterlockedIncrement(&g_refWorkers);
        std::thread([sh, hwnd, id, url, ref, ck, wasRunning]() mutable
        {
            NdmQueue& q = NdmQueue::GetInstance();
            std::wstring msg;
            NdmRefreshResult r = q.RefreshAddress(id, url, ref, ck, msg);
            // Paused just now: its worker may need a moment to stop.
            for (int i = 0; wasRunning && r == NDM_REFRESH_BUSY && i < 20; ++i)
            {
                Sleep(300);
                r = q.RefreshAddress(id, url, ref, ck, msg);
            }
            if (!ck.empty()) SecureZeroMemory(&ck[0], ck.size() * sizeof(wchar_t));
            sh->res = r;
            sh->msg = msg;
            PostMessageW(hwnd, WM_REF_DONE, 0, 0);   // no pointer in the message
            InterlockedDecrement(&g_refWorkers);
        }).detach();
        SetTimer(c->hwnd, kRefTimer, 40, nullptr);
        RefSetState(c);
    }

    void RefDone(RefCtx* c)
    {
        KillTimer(c->hwnd, kRefTimer);
        c->checking = false;
        if (!c->shared) return;
        if (c->shared->res == NDM_REFRESH_OK)
        {
            DWORD id = c->id;
            HWND owner = c->owner;
            bool autoMode = c->autoMode;
            NdmQueue::GetInstance().ResumeDownload(id);
            NdmMainWindow::GetInstance().RefreshListView();
            DestroyWindow(c->hwnd);                        // c is freed
            if (autoMode) NdmProgressDlg::Show(owner, id);
            return;
        }
        c->message = NdmUi::RefreshFailText(c->shared->msg);
        c->msgKind = 1;
        if (c->autoMode) c->failedAuto = true;
        RefSetState(c);
        if (c->autoMode) SetFocus(c->hAddNew);
        else { SetFocus(c->hUrl); SendMessageW(c->hUrl, EM_SETSEL, 0, -1); }
    }

    LRESULT CALLBACK RefProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        RefCtx* c = (RefCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m)
        {
        case WM_NCCREATE:
            c = (RefCtx*)((CREATESTRUCTW*)l)->lpCreateParams;
            c->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
            break;
        case WM_CREATE:
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            c->hUrl    = MakeEdit(h, IDC_REF_URL);
            SetAccName(c->hUrl, L"New link");
            c->hCancel = MakeBtn(h, IDC_REF_CANCEL, L"Cancel");
            c->hOk     = MakeBtn(h, IDC_REF_OK, c->autoMode ? L"Continue download" : L"OK");
            c->hAddNew = MakeBtn(h, IDC_REF_ADDNEW, L"Add as new");
            SendMessageW(c->hUrl, EM_LIMITTEXT, 8192, 0);
            RefSetState(c);
            return 0;
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
                Fill(dc, cl, kBg);
                int L = c->S(kRefM), R = cl.right - c->S(kRefM);
                const UINT one = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                RECT nr = { L, c->S(10), R, c->S(30) };
                Text(dc, c->name, nr, NDM_CLR_FG, c->res.title, one);
                RECT ir = { L, c->S(30), R, c->S(48) };
                Text(dc, c->info, ir, NDM_CLR_FG_DIM, c->res.sm, one);
                DrawField(dc, c->rcUrl, GetFocus() == c->hUrl, c->checking || c->autoMode, c->res.dpi);
                RECT mr = c->rcMsg;
                if (c->checking)
                {
                    RECT pb = { mr.left, mr.top + c->S(7), mr.left + c->S(90), mr.top + c->S(13) };
                    ProgressBar(dc, pb, -1.0, NDM_CLR_GOLD_DARK, NDM_CLR_GOLD_LIGHT, c->phase);
                    mr.left = pb.right + c->S(10);
                }
                COLORREF col = c->msgKind == 1 ? RGB(236, 110, 110) : NDM_CLR_FG_DIM;
                Text(dc, c->message, mr, col, c->res.sm, one);
                Footer(dc, cl, c->S(kRefH - kFootH));
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (w == kRefTimer)
            {
                c->phase = (c->phase + 8) % 1200;
                InvalidateRect(h, &c->rcMsg, FALSE);
            }
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return (LRESULT)EditColors((HDC)w, (HWND)l, c->res);
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            NdmBtnKind k = (di->CtlID == IDC_REF_OK || di->CtlID == IDC_REF_ADDNEW) ? BTN_PRIMARY : BTN_GHOST;
            DrawButton(di, k, kFootBg, c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_REF_DONE:
            RefDone(c);
            return 0;
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == IDOK)
            {
                HWND f = GetFocus();
                id = f == c->hCancel ? IDC_REF_CANCEL : ((GetWindowLongPtrW(c->hAddNew, GWL_STYLE) & WS_VISIBLE) ? IDC_REF_ADDNEW : IDC_REF_OK);
            }
            switch (id)
            {
            case IDC_REF_OK:
                if (!c->checking) RefStart(c);
                return 0;
            case IDC_REF_ADDNEW:
            {
                HWND owner = c->owner;
                NdmBrowserRequest req = c->req;
                DestroyWindow(h);                            // c is freed
                NdmAddDlg::ShowBrowserRequest(owner, req);
                return 0;
            }
            case IDCANCEL:
            case IDC_REF_CANCEL:
                DestroyWindow(h);
                return 0;
            case IDC_REF_URL:
                if (code == EN_CHANGE && c->msgKind == 1 && !c->autoMode)
                {
                    c->message.clear();
                    c->msgKind = 0;
                    InvalidateRect(h, &c->rcMsg, FALSE);
                }
                if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(h, &c->rcUrl, FALSE);
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
            RefLayout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            KillTimer(h, kRefTimer);
            RemoveModeless(h);
            if (c) g_refWins.erase(c->id);
            return 0;
        case WM_NCDESTROY:
            if (c)
            {
                c->res.Free();
                if (!c->req.cookies.empty())
                    SecureZeroMemory(&c->req.cookies[0], c->req.cookies.size() * sizeof(wchar_t));
                delete c;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }

    std::wstring ItemInfo(const NdmDownload& d)
    {
        std::wstring s = NdmUi::ProgressWord(d.status, d.downloadedSize, d.totalSize);
        s[0] = (wchar_t)towupper(s[0]);
        if (d.totalSize) s += L"  ·  " + NdmFormatSize(d.totalSize);
        return s;
    }
}

// ===========================================================================
// Public
// ===========================================================================
int NdmDialogs::AskEx(HWND owner, const AskSpec& spec)
{
    if (spec.buttons.empty()) return -1;
    const int n = (int)spec.buttons.size();
    AskCtx c;
    c.owner  = owner;
    c.text   = spec.text;
    c.labels = spec.buttons;
    c.def    = (spec.defaultButton >= 0 && spec.defaultButton < n) ? spec.defaultButton : 0;
    c.cancel = (spec.cancelButton >= 0 && spec.cancelButton < n) ? spec.cancelButton : -1;
    c.danger = (spec.dangerButton >= 0 && spec.dangerButton < n) ? spec.dangerButton : -1;
    c.glyph  = spec.glyph;
    HWND h = MakeWindow(L"NDMAskWnd", AskProc, spec.title ? spec.title : L"NDM", owner, &c, true);
    if (!h) return c.cancel;
    PlaceWindow(h, kAskW, AskHeight96(&c), owner);
    AskLayout(&c);
    RunModal(h, owner, c.done, c.btns[c.def]);
    return c.result;
}

int NdmDialogs::Ask(HWND owner, const wchar_t* title, const std::wstring& text,
                    const std::vector<std::wstring>& buttons, NdmGlyph glyph)
{
    // buttons[0] = default = rightmost: the screen order is reversed.
    const int n = (int)buttons.size();
    AskSpec s;
    s.title = title;
    s.text  = text;
    s.glyph = glyph;
    for (int pos = 0; pos < n; ++pos) s.buttons.push_back(buttons[NdmUi::AskLegacyPos(pos, n)]);
    s.defaultButton = NdmUi::AskLegacyPos(0, n);
    int r = AskEx(owner, s);
    return r < 0 ? -1 : NdmUi::AskLegacyPos(r, n);
}

void NdmDialogs::Notice(HWND owner, const wchar_t* title, const std::wstring& text, NdmGlyph glyph)
{
    AskSpec s;
    s.title = title;
    s.text  = text;
    s.glyph = glyph;
    s.buttons = { L"OK" };
    s.cancelButton = 0;
    AskEx(owner, s);
}

bool NdmDialogs::Input(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& text,
                       const std::function<std::wstring(const std::wstring&)>& validate,
                       bool numeric, const wchar_t* suffix, bool selectStem)
{
    InCtx c;
    c.owner = owner;
    c.label = label ? label : L"";
    c.suffix = suffix ? suffix : L"";
    c.text = text;
    c.numeric = numeric;
    c.selectStem = selectStem;
    c.validate = &validate;
    HWND h = MakeWindow(L"NDMInputWnd", InProc, title, owner, &c, true);
    if (!h) return false;
    PlaceWindow(h, kInW, kInH, owner);
    InLayout(&c);
    int selEnd = -1;
    if (selectStem)
    {
        size_t dot = text.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) selEnd = (int)dot;
    }
    SendMessageW(c.hEdit, EM_SETSEL, 0, selEnd);
    RunModal(h, owner, c.done, c.hEdit);
    if (c.ok) text = c.text;
    return c.ok;
}

int NdmDialogs::AskDuplicate(HWND owner, DWORD existing)
{
    NdmDownload d;
    if (!NdmQueue::GetInstance().GetSnapshot(existing, d)) return 1;
    std::wstring name = d.filename.empty() ? NdmSafeFilename(d.url) : d.filename;
    std::wstring text = L"This link is already in your list (“" + name + L"”, " +
                        NdmUi::ProgressWord(d.status, d.downloadedSize, d.totalSize) + L").";
    return Ask(owner, L"Already in the list", text, { L"Open existing", L"Download again", L"Cancel" }, GLY_LINK);
}

int NdmDialogs::AskContinueWithNewLink(HWND owner, DWORD candidate)
{
    NdmDownload d;
    if (!NdmQueue::GetInstance().GetSnapshot(candidate, d)) return 1;
    std::wstring text = L"“" + d.filename + L"” is already in your list (" +
                        NdmUi::ProgressWord(d.status, d.downloadedSize, d.totalSize) +
                        L"). Use this new link to continue it?";
    return Ask(owner, L"Continue download", text, { L"Continue download", L"Add as new", L"Cancel" }, GLY_REFRESH);
}

void NdmDialogs::ShowRefresh(HWND owner, DWORD id, const NdmBrowserRequest* req)
{
    auto it = g_refWins.find(id);
    if (it != g_refWins.end() && IsWindow(it->second))
    {
        ForceForeground(it->second);
        return;
    }
    NdmDownload d;
    if (!NdmQueue::GetInstance().GetSnapshot(id, d)) return;

    RefCtx* c = new RefCtx();
    c->id = id;
    c->owner = owner;
    c->autoMode = req != nullptr;
    if (req) c->req = *req;
    c->name = d.filename.empty() ? NdmSafeFilename(d.url) : d.filename;
    c->info = ItemInfo(d);
    if (!c->autoMode) c->message = L"Paste a fresh link to the same file. The downloaded part is kept.";
    HWND h = MakeWindow(kRefClass, RefProc, L"Refresh download address", owner, c, false, req != nullptr);
    if (!h) { delete c; return; }
    g_refWins[id] = h;
    PlaceWindow(h, kRefW, kRefH, owner);
    RefLayout(c);
    AddModeless(h);

    std::wstring url;
    if (req) url = req->url;
    else
    {
        std::wstring clip = NdmAddDlg::GetClipboardUrl();
        if (NdmIsHttpUrl(clip) && clip != d.url) url = clip;
    }
    SetWindowTextW(c->hUrl, url.c_str());
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    if (req)
    {
        // A browser link: drop topmost once shown, then check at once.
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RefStart(c);
        SetFocus(c->hCancel);
    }
    else
    {
        SetFocus(c->hUrl);
        SendMessageW(c->hUrl, EM_SETSEL, 0, -1);
    }
}

void NdmDialogs::WaitForWorkers(DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (InterlockedCompareExchange(&g_refWorkers, 0, 0) > 0 && GetTickCount64() < end)
        Sleep(50);
}
