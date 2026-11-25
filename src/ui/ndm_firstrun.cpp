// ndm_firstrun.cpp -- first-run questions (IDM-style), notification prefs,
// "Add NDM to your browser" helper.
// Invented with Care By Devnetra Consultancy
//
// Layout (96-dpi units, client 420 x 252):
//   [logo] Welcome to NDM
//   (o) Start NDM with Windows
//   (o) Capture downloads from browsers
//   Add NDM to your browser [Chrome / Edge] [Firefox]
//   hint line (+ Firefox: temporary add-on note)
//   ------------------------------------------------
//                                              [OK]
// Notifications and the other choices live in Options only.

#include "ndm_firstrun.h"
#include "ndm_theme.h"
#include "ndm_optdlg.h"
#include "ndm_dialogs.h"
#include "ndm_uilogic.h"
#include "../core/ndm_config.h"

#include <shellapi.h>

using namespace NdmTheme;

// ===========================================================================
// Preferences (HKCU\Software\Devnetra\NDM)
// ===========================================================================
const wchar_t* const NdmPrefs::kNotifyDialog = L"NotifyCompleteDialog";
const wchar_t* const NdmPrefs::kNotifyTray   = L"NotifyTray";
const wchar_t* const NdmPrefs::kNotifySound  = L"NotifySound";
const wchar_t* const NdmPrefs::kFileInfo     = L"ShowFileInfoDialog";
const wchar_t* const NdmPrefs::kFirstRunDone = L"FirstRunDone";
const wchar_t* const NdmPrefs::kWatchClipboard = L"WatchClipboard";

bool NdmPrefs::Get(const wchar_t* name, bool def)
{
    DWORD v = 0, sz = sizeof(v), type = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NDM_REG_ROOT, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return def;
    LONG rc = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return def;
    return v != 0;
}

void NdmPrefs::Set(const wchar_t* name, bool on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, NDM_REG_ROOT, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = on ? 1 : 0;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}

// ===========================================================================
// Browser setup helper
// ===========================================================================
std::wstring NdmFirstRun::InstallDir()
{
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe, n);
    size_t p = d.find_last_of(L"\\/");
    return p == std::wstring::npos ? d : d.substr(0, p);
}


// Full path of a registered application ("chrome.exe") from
// Software\Microsoft\Windows\CurrentVersion\App Paths\<exe> (HKCU first, then
// HKLM, 64-bit view). Only an absolute, existing *.exe path is returned.
static std::wstring AppPathExe(const wchar_t* exe)
{
    const std::wstring sub = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
    {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
            continue;
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD type = 0, sz = sizeof(buf) - sizeof(wchar_t);
        LONG rc = RegQueryValueExW(k, nullptr, nullptr, &type, (BYTE*)buf, &sz);
        RegCloseKey(k);
        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        buf[sz / sizeof(wchar_t)] = 0;
        std::wstring v = buf;
        if (type == REG_EXPAND_SZ)
        {
            wchar_t ex[MAX_PATH * 2] = {};
            DWORD n = ExpandEnvironmentStringsW(v.c_str(), ex, MAX_PATH * 2);
            if (!n || n > MAX_PATH * 2) continue;
            v = ex;
        }
        size_t a = v.find_first_not_of(L" \t\"");
        size_t b = v.find_last_not_of(L" \t\"");
        if (a == std::wstring::npos) continue;
        v = v.substr(a, b - a + 1);
        // absolute drive path only, ending in .exe, existing regular file
        if (v.size() < 8 || !iswalpha(v[0]) || v[1] != L':' || v[2] != L'\\') continue;
        if (_wcsicmp(v.c_str() + v.size() - 4, L".exe") != 0) continue;
        DWORD at = GetFileAttributesW(v.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) continue;
        return v;
    }
    return std::wstring();
}

void NdmFirstRun::OpenBrowserSetup(HWND owner, bool firefox)
{
    std::wstring ext = JoinPath(InstallDir(), L"ext");
    std::wstring dir = JoinPath(ext, firefox ? L"firefox" : L"chrome");
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        NdmDialogs::Notice(owner, L"Add NDM to your browser",
                           L"The extension folder was not found:\n\n" + dir, GLY_BROWSER);
        return;
    }
    // Never launch a browser by bare name (ShellExecute would search the
    // current folder / PATH first): the full path registered under
    // App Paths (HKCU, then HKLM), and it must exist.
    auto run = [&](const wchar_t* exe, const std::wstring& arg)
    {
        std::wstring full = AppPathExe(exe);
        if (full.empty()) return false;
        return (INT_PTR)ShellExecuteW(owner, L"open", full.c_str(), arg.c_str(), nullptr, SW_SHOWNORMAL) > 32;
    };
    if (firefox)
    {
        run(L"firefox.exe", L"about:debugging#/runtime/this-firefox");
    }
    else
    {
        std::wstring page = L"\"" + JoinPath(dir, L"welcome.html") + L"\"";
        if (!run(L"chrome.exe", page) && !run(L"msedge.exe", page))
            ShellExecuteW(owner, L"open", JoinPath(dir, L"welcome.html").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    OpenFolderSelect(ext, firefox ? L"firefox" : L"chrome");
}

// ===========================================================================
// First-run window
// ===========================================================================
namespace
{
    const wchar_t* kClass = L"NDMFirstRunWnd";
    const int kW = 420, kH = 252, kM = 16, kFooterY = 206;
    const int kBrowserY = 124;                 // "Add NDM to your browser" row
    const COLORREF kBg = RGB(24, 27, 33), kFootBg = RGB(20, 22, 27);
    enum { IDC_T0 = 700, IDC_CHROME = 720, IDC_FIREFOX, IDC_OKBTN };
    enum { T_STARTUP, T_CAPTURE, kToggles };
    const wchar_t* kToggleText[kToggles] = {
        L"Start NDM with Windows",
        L"Capture downloads from browsers" };

    HWND g_wnd = nullptr;

    struct Ctx
    {
        HWND hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        HWND hT[kToggles] = {};
        HWND hChrome = nullptr, hFirefox = nullptr, hOk = nullptr;
        int  hint = -1;              // -1 none, 0 chrome, 1 firefox
        bool saved = false;
        int S(int v) const { return res.S(v); }
    };
    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    void Layout(Ctx* c)
    {
        int L = c->S(kM), R = c->S(kW - kM);
        for (int i = 0; i < kToggles; ++i)
        {
            MoveWindow(c->hT[i], L, c->S(60) + i * c->S(28), R - L, c->S(24), TRUE);
            SendMessageW(c->hT[i], WM_SETFONT, (WPARAM)c->res.ui, FALSE);
        }
        int y = c->S(kBrowserY), bh = c->S(26);
        MoveWindow(c->hFirefox, R - c->S(80), y, c->S(80), bh, TRUE);
        MoveWindow(c->hChrome,  R - c->S(80) - c->S(8) - c->S(108), y, c->S(108), bh, TRUE);
        int by = c->S(kFooterY) + (c->S(kH - kFooterY) - c->S(28)) / 2;
        MoveWindow(c->hOk, R - c->S(84), by, c->S(84), c->S(28), TRUE);
        HWND b[] = { c->hChrome, c->hFirefox, c->hOk };
        for (HWND h : b) SendMessageW(h, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
    }

    void Save(Ctx* c)
    {
        if (c->saved) return;
        c->saved = true;
        auto on = [&](int i) { return SendMessageW(c->hT[i], BM_GETCHECK, 0, 0) == BST_CHECKED; };
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        cfg.SetStartWithWin(on(T_STARTUP));
        cfg.SetBrowserMonitor(on(T_CAPTURE));
        cfg.Save();
        NdmPrefs::Set(NdmPrefs::kFirstRunDone, true);
        NdmOptDlg::ApplySettings();          // Run key, capture switch for the extension host
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
            RECT lg = { L, c->S(14), L + c->S(28), c->S(14) + c->S(28) };
            Logo(dc, lg);
            RECT t = { lg.right + c->S(10), lg.top, R, lg.bottom };
            Text(dc, L"Welcome to NDM", t, NDM_CLR_FG, r.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            HLine(dc, L, R, c->S(50), RGB(40, 44, 53));
            RECT br = { L, c->S(kBrowserY), R - c->S(200), c->S(kBrowserY + 26) };
            Text(dc, L"Add NDM to your browser", br, NDM_CLR_FG, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (c->hint >= 0)
            {
                RECT hr = { L, c->S(kBrowserY + 32), R, c->S(kBrowserY + 48) };
                Text(dc, NdmUi::BrowserHint(c->hint == 1), hr, RGB(176, 150, 76), r.sm,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (const wchar_t* note = NdmUi::BrowserNote(c->hint == 1))
                {
                    RECT nr = { L, c->S(kBrowserY + 50), R, c->S(kFooterY - 2) };
                    Text(dc, note, nr, NDM_CLR_FG_DIM, r.sm, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                }
            }
            RECT ft = { 0, c->S(kFooterY), cl.right, cl.bottom };
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
            NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
            bool def[kToggles] = { NdmFirstRun::IsNeeded() || cfg.GetStartWithWin(),
                                   cfg.GetBrowserMonitor() };
            for (int i = 0; i < kToggles; ++i)
            {
                c->hT[i] = MakeBtn(h, IDC_T0 + i, kToggleText[i], GLY_NONE, true);
                SendMessageW(c->hT[i], BM_SETCHECK, def[i] ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            c->hChrome  = MakeBtn(h, IDC_CHROME,  L"Chrome / Edge");
            c->hFirefox = MakeBtn(h, IDC_FIREFOX, L"Firefox");
            c->hOk      = MakeBtn(h, IDC_OKBTN,   L"OK");
            Layout(c);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(c); return 0;
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
            NdmBtnKind k = BTN_SECONDARY; COLORREF bg = kBg; HFONT f = c->res.bold;
            if (di->CtlID >= IDC_T0 && di->CtlID < IDC_T0 + kToggles) { k = BTN_TOGGLE; f = c->res.ui; }
            if (di->CtlID == IDC_OKBTN) { k = BTN_PRIMARY; bg = kFootBg; }
            DrawButton(di, k, bg, f, c->res.dpi);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IDC_CHROME:
            case IDC_FIREFOX:
                c->hint = LOWORD(w) == IDC_FIREFOX ? 1 : 0;
                InvalidateRect(h, nullptr, FALSE);
                NdmFirstRun::OpenBrowserSetup(h, c->hint == 1);
                return 0;
            case IDOK:
            case IDC_OKBTN:
            case IDCANCEL:
                Save(c);
                DestroyWindow(h);
                return 0;
            }
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
            return 0;
        }
        case WM_CLOSE:
            Save(c);                          // closing = keep the shown choices
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            RemoveModeless(h);
            if (g_wnd == h) g_wnd = nullptr;
            return 0;
        case WM_NCDESTROY:
            if (c) { c->res.Free(); delete c; SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

bool NdmFirstRun::IsNeeded()
{
    return !NdmPrefs::Get(NdmPrefs::kFirstRunDone, false);
}

void NdmFirstRun::Show(HWND owner)
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
    HWND h = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kClass, L"NDM Setup",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, nullptr, nullptr, hi, c);
    if (!h) { delete c; return; }
    g_wnd = h;
    PlaceWindow(h, kW, kH, owner);
    AddModeless(h);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetFocus(c->hOk);
}
