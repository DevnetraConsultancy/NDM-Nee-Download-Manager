// ndm_power.cpp -- completion action countdown + executor (see ndm_power.h)
// Invented with Care By Devnetra Consultancy
//
// Layout (96-dpi units, client 380 x 118):
//   [tile]  Shutting down in 30 s
//           "file.zip" finished.
//   ------------------------------------------------
//                                          [Cancel]

#include "ndm_power.h"
#include "ndm_theme.h"
#include "ndm_uilogic.h"
#include "ndm_mainwnd.h"
#include "ndm_optdlg.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"

#include <powrprof.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass = L"NDMCountdownWnd";
    const int kW = 380, kH = 118, kM = 16, kTile = 34, kFootH = 46;
    const int kSeconds = 30;
    const UINT kTimer = 1;
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);
    enum { IDC_CD_CANCEL = 600 };

    HWND g_wnd = nullptr;

    struct Ctx
    {
        HWND hwnd = nullptr;
        NdmDlgRes res;
        int action = NDM_DONE_SHUTDOWN;
        std::wstring sub;                  // what finished
        std::wstring error;                // set when the action failed
        ULONGLONG deadline = 0;
        bool acting = false;
        HWND hCancel = nullptr;
        int S(int v) const { return res.S(v); }
    };

    std::wstring SysError(DWORD e)
    {
        wchar_t* buf = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
        std::wstring s = buf ? buf : L"";
        if (buf) LocalFree(buf);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        if (s.empty()) s = L"error " + std::to_wstring(e);
        return s;
    }

    bool EnableShutdownPrivilege()
    {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
                  AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) &&
                  GetLastError() == ERROR_SUCCESS;
        CloseHandle(tok);
        return ok;
    }

    // Save everything, then sleep / hibernate / shut down.
    bool Perform(int action, std::wstring& err)
    {
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        NdmQueue::GetInstance().SaveQueue(cfg.GetAppDataPath() + L"queue.xml");
        cfg.Save();
        NdmOptDlg::ApplyRunKey();          // cfg.Save rewrites the Run key; keep ours (-tray)

        if (!NdmPower::Available(action)) { err = NdmPower::UnavailableText(action); return false; }
        EnableShutdownPrivilege();
        if (action == NDM_DONE_SLEEP || action == NDM_DONE_HIBERNATE)
        {
            if (!SetSuspendState(action == NDM_DONE_HIBERNATE, FALSE, FALSE))
            {
                err = L"Windows did not allow it: " + SysError(GetLastError());
                return false;
            }
            return true;
        }
        if (!ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG,
                           SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED))
        {
            err = L"Windows did not allow it: " + SysError(GetLastError());
            return false;
        }
        return true;
    }

    int SecondsLeft(Ctx* c)
    {
        ULONGLONG now = GetTickCount64();
        if (now >= c->deadline) return 0;
        return (int)((c->deadline - now + 999) / 1000);
    }

    void Layout(Ctx* c)
    {
        int bh = c->S(28), by = c->S(kH - kFootH) + (c->S(kFootH) - bh) / 2;
        int w = c->S(84);
        MoveWindow(c->hCancel, c->S(kW - kM) - w, by, w, bh, TRUE);
        SendMessageW(c->hCancel, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
    }

    void Tick(Ctx* c)
    {
        if (c->acting) return;
        if (SecondsLeft(c) > 0) { InvalidateRect(c->hwnd, nullptr, FALSE); return; }
        KillTimer(c->hwnd, kTimer);
        c->acting = true;
        InvalidateRect(c->hwnd, nullptr, FALSE);
        UpdateWindow(c->hwnd);
        std::wstring err;
        if (Perform(c->action, err))
        {
            // Sleep / hibernate return after the PC wakes up; shut down is
            // asynchronous (WM_QUERYENDSESSION follows). Either way: done.
            DestroyWindow(c->hwnd);
            return;
        }
        c->error = err;
        SetWindowTextW(c->hCancel, L"Close");
        InvalidateRect(c->hwnd, nullptr, FALSE);
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
            RECT tile = { c->S(kM), c->S(kM), c->S(kM + kTile), c->S(kM + kTile) };
            GlyphTile(dc, tile, GLY_CLOCK, c->res.dpi);
            int L = tile.right + c->S(12), R = cl.right - c->S(kM);
            const UINT one = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            std::wstring head;
            if (!c->error.empty())
                head = std::wstring(NdmUi::ActionName(c->action)) + L" did not happen";
            else if (c->acting)
                head = c->action == NDM_DONE_SHUTDOWN ? L"Shutting down…"
                     : c->action == NDM_DONE_HIBERNATE ? L"Hibernating…" : L"Going to sleep…";
            else
                head = NdmUi::CountdownText(c->action, SecondsLeft(c));
            RECT hr = { L, c->S(kM - 2), R, c->S(kM + 18) };
            Text(dc, head, hr, NDM_CLR_FG, c->res.big, one);
            RECT sr = { L, c->S(kM + 20), R, c->S(kM + 38) };
            Text(dc, c->error.empty() ? c->sub : c->error, sr,
                 c->error.empty() ? NDM_CLR_FG_DIM : RGB(236, 110, 110), c->res.sm, one);
            RECT ft = { 0, c->S(kH - kFootH), cl.right, cl.bottom };
            Fill(dc, ft, kFootBg);
            HLine(dc, 0, cl.right, ft.top, RGB(40, 44, 53));
        }
        EndPaint(c->hwnd, &ps);
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        Ctx* c = (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA);
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
            c->hCancel = MakeBtn(h, IDC_CD_CANCEL, L"Cancel");
            Layout(c);
            c->deadline = GetTickCount64() + kSeconds * 1000ULL;
            SetTimer(h, kTimer, 250, nullptr);
            return 0;
        case WM_TIMER:
            if (w == kTimer) Tick(c);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(c);
            return 0;
        case WM_DRAWITEM:
            DrawButton((DRAWITEMSTRUCT*)l, BTN_PRIMARY, kFootBg, c->res.bold, c->res.dpi);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IDOK:                 // Enter: the default button is Cancel
            case IDCANCEL:
            case IDC_CD_CANCEL:
                DestroyWindow(h);
                return 0;
            }
            return 0;
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
            DestroyWindow(h);
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

// ===========================================================================
// Public
// ===========================================================================
void NdmPower::OnQueueAction(int action, DWORD id, void* ctx)
{
    HWND h = (HWND)ctx;
    if (h && action != NDM_DONE_NONE) PostMessageW(h, WM_NDM_DONE_ACTION, (WPARAM)action, (LPARAM)id);
}

bool NdmPower::Available(int action)
{
    if (action == NDM_DONE_SLEEP)     return IsPwrSuspendAllowed() != FALSE;
    if (action == NDM_DONE_HIBERNATE) return IsPwrHibernateAllowed() != FALSE;
    return true;
}

std::wstring NdmPower::UnavailableText(int action)
{
    if (Available(action)) return L"";
    return action == NDM_DONE_HIBERNATE ? L"Hibernate is not available on this PC."
                                        : L"Sleep is not available on this PC.";
}

void NdmPower::StartCountdown(HWND owner, int action, DWORD id)
{
    if (action < NDM_DONE_SLEEP || action > NDM_DONE_SHUTDOWN) return;
    if (g_wnd && IsWindow(g_wnd)) return;

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
    c->action = action;
    NdmDownload d;
    if (id && NdmQueue::GetInstance().GetSnapshot(id, d) && !d.filename.empty())
        c->sub = L"“" + d.filename + L"” finished.";
    else
        c->sub = L"All downloads finished.";
    POINT pt = CreationPoint(owner);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kClass,
                             NdmUi::ActionName(action),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, nullptr, nullptr, hi, c);
    if (!h) { delete c; return; }
    g_wnd = h;
    PlaceWindow(h, kW, kH, owner);
    AddModeless(h);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // ForceForeground drops topmost
    SetFocus(c->hCancel);
    MessageBeep(MB_ICONEXCLAMATION);
}
