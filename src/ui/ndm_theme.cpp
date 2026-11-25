// ndm_theme.cpp -- shared premium dark + gold drawing helpers for the NDM UI
// Invented with Care By Devnetra Consultancy

#include "ndm_theme.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objidl.h>
#include <algorithm>
#include <vector>
#include <cmath>

// GDI+ needs min/max, which NOMINMAX removes.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")

// PROPID_ACC_* are DEFINE_GUID constants: define them here (selectany).
#include <initguid.h>
#include <oleacc.h>
#pragma comment(lib, "oleacc.lib")

using namespace Gdiplus;

namespace
{
    ULONG_PTR            g_gdipToken = 0;
    std::vector<HWND>    g_modeless;

    // Undocumented uxtheme dark-mode entry points (Win10 1809+)
    typedef bool (WINAPI* fnAllowDarkModeForWindow)(HWND, bool);
    typedef int  (WINAPI* fnSetPreferredAppMode)(int);
    typedef bool (WINAPI* fnAllowDarkModeForApp)(bool);
    typedef void (WINAPI* fnFlushMenuThemes)();
    fnAllowDarkModeForWindow g_allowDarkWnd = nullptr;
    bool                     g_darkOk       = false;

    const wchar_t* PROP_HOT   = L"NDM.Hot";
    const wchar_t* PROP_CHECK = L"NDM.Check";
    const wchar_t* PROP_GLYPH = L"NDM.Glyph";
    const wchar_t* PROP_TOGG  = L"NDM.Toggle";

    inline Color GC(COLORREF c, BYTE a = 255)
    {
        return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    void RoundPath(GraphicsPath& p, const RectF& r, float rad)
    {
        float d = rad * 2.0f;
        if (d > r.Width)  d = r.Width;
        if (d > r.Height) d = r.Height;
        if (d < 1.0f) { p.AddRectangle(r); return; }
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        p.CloseFigure();
    }

    RectF RF(const RECT& rc)
    {
        return RectF((REAL)rc.left, (REAL)rc.top,
                     (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    }

    void PrepG(Graphics& g)
    {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
    }

    DWORD WinBuild()
    {
        typedef void (WINAPI* fnRtl)(LPDWORD, LPDWORD, LPDWORD);
        static DWORD build = 0xFFFFFFFF;
        if (build != 0xFFFFFFFF) return build;
        build = 0;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        fnRtl rtl = nt ? (fnRtl)GetProcAddress(nt, "RtlGetNtVersionNumbers") : nullptr;
        if (rtl)
        {
            DWORD maj = 0, min = 0, b = 0;
            rtl(&maj, &min, &b);
            if (maj >= 10) build = b & ~0xF0000000;
        }
        return build;
    }

    // ---- accessibility -------------------------------------------------
    IAccPropServices* g_acc      = nullptr;
    bool              g_accTried = false;

    IAccPropServices* AccSvc()
    {
        if (!g_acc && !g_accTried)
        {
            g_accTried = true;
            if (FAILED(CoCreateInstance(__uuidof(CAccPropServices), nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IAccPropServices), (void**)&g_acc)))
                g_acc = nullptr;
        }
        return g_acc;
    }

    const MSAAPROPID kToggleProps[3] = { PROPID_ACC_ROLE, PROPID_ACC_STATE, PROPID_ACC_DEFAULTACTION };

    // Live role / state of an owner-drawn toggle for screen readers (the
    // standard button proxy would call it a push button without a state).
    class ToggleAccServer : public IAccPropServer
    {
    public:
        explicit ToggleAccServer(HWND h) : m_h(h) {}
        virtual ~ToggleAccServer() = default;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IAccPropServer))
            {
                *ppv = static_cast<IAccPropServer*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            LONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return (ULONG)r;
        }
        HRESULT STDMETHODCALLTYPE GetPropValue(const BYTE*, DWORD, MSAAPROPID id,
                                               VARIANT* v, BOOL* got) override
        {
            if (!v || !got) return E_POINTER;
            VariantInit(v);
            *got = FALSE;
            if (!IsWindow(m_h)) return S_OK;
            bool on = GetPropW(m_h, PROP_CHECK) != nullptr;
            if (id == PROPID_ACC_ROLE)
            {
                v->vt = VT_I4; v->lVal = ROLE_SYSTEM_CHECKBUTTON; *got = TRUE;
            }
            else if (id == PROPID_ACC_STATE)
            {
                long s = STATE_SYSTEM_FOCUSABLE;
                if (on)                    s |= STATE_SYSTEM_CHECKED;
                if (GetFocus() == m_h)     s |= STATE_SYSTEM_FOCUSED;
                if (!IsWindowEnabled(m_h)) s |= STATE_SYSTEM_UNAVAILABLE;
                if (!IsWindowVisible(m_h)) s |= STATE_SYSTEM_INVISIBLE;
                v->vt = VT_I4; v->lVal = s; *got = TRUE;
            }
            else if (id == PROPID_ACC_DEFAULTACTION)
            {
                v->bstrVal = SysAllocString(on ? L"Uncheck" : L"Check");
                if (v->bstrVal) { v->vt = VT_BSTR; *got = TRUE; }
            }
            return S_OK;
        }
    private:
        LONG m_ref = 1;
        HWND m_h;
    };

    void AccStateChanged(HWND h)
    {
        NotifyWinEvent(EVENT_OBJECT_STATECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
    }

    LRESULT CALLBACK AccNameSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
    {
        if (m == WM_NCDESTROY)
        {
            MSAAPROPID p = PROPID_ACC_NAME;
            if (g_acc) g_acc->ClearHwndProps(h, (DWORD)OBJID_CLIENT, CHILDID_SELF, &p, 1);
            RemoveWindowSubclass(h, AccNameSubclass, id);
        }
        return DefSubclassProc(h, m, w, l);
    }

    LRESULT CALLBACK BtnSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                 UINT_PTR id, DWORD_PTR)
    {
        switch (m)
        {
        case WM_MOUSEMOVE:
            if (!GetPropW(h, PROP_HOT))
            {
                SetPropW(h, PROP_HOT, (HANDLE)1);
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, PROP_HOT);
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_SETCURSOR:
            if (IsWindowEnabled(h))
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDBLCLK:
            // Owner-draw buttons turn the 2nd click into BN_DOUBLECLICKED;
            // treat it as a normal press so fast clicking works (steppers).
            m = WM_LBUTTONDOWN;
            break;
        case BM_GETCHECK:
            if (GetPropW(h, PROP_TOGG))
                return GetPropW(h, PROP_CHECK) ? BST_CHECKED : BST_UNCHECKED;
            break;
        case BM_SETCHECK:
            if (GetPropW(h, PROP_TOGG))
            {
                bool was = GetPropW(h, PROP_CHECK) != nullptr;
                if (w == BST_CHECKED) SetPropW(h, PROP_CHECK, (HANDLE)1);
                else                  RemovePropW(h, PROP_CHECK);
                InvalidateRect(h, nullptr, FALSE);
                if (was != (w == BST_CHECKED)) AccStateChanged(h);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_KEYUP:
            if (GetPropW(h, PROP_TOGG) && (m == WM_LBUTTONUP || w == VK_SPACE))
            {
                bool pushed = (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
                bool inside = true;
                if (m == WM_LBUTTONUP)
                {
                    RECT rc; GetClientRect(h, &rc);
                    POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
                    inside = PtInRect(&rc, pt) != FALSE;
                }
                if (pushed && inside)
                {
                    if (GetPropW(h, PROP_CHECK)) RemovePropW(h, PROP_CHECK);
                    else                         SetPropW(h, PROP_CHECK, (HANDLE)1);
                    InvalidateRect(h, nullptr, FALSE);
                    AccStateChanged(h);
                }
            }
            break;
        case WM_NCDESTROY:
            if (GetPropW(h, PROP_TOGG) && g_acc)
                g_acc->ClearHwndProps(h, (DWORD)OBJID_CLIENT, CHILDID_SELF, kToggleProps, 3);
            RemovePropW(h, PROP_HOT);
            RemovePropW(h, PROP_CHECK);
            RemovePropW(h, PROP_GLYPH);
            RemovePropW(h, PROP_TOGG);
            RemoveWindowSubclass(h, BtnSubclass, id);
            break;
        }
        return DefSubclassProc(h, m, w, l);
    }
}

// ===========================================================================
// Lifetime
// ===========================================================================
void NdmTheme::Init()
{
    if (!g_gdipToken)
    {
        GdiplusStartupInput in;
        GdiplusStartup(&g_gdipToken, &in, nullptr);
    }

    DWORD build = WinBuild();
    if (build >= 17763)
    {
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (ux)
        {
            g_allowDarkWnd = (fnAllowDarkModeForWindow)
                GetProcAddress(ux, MAKEINTRESOURCEA(133));
            FARPROC p135 = GetProcAddress(ux, MAKEINTRESOURCEA(135));
            fnFlushMenuThemes flush = (fnFlushMenuThemes)
                GetProcAddress(ux, MAKEINTRESOURCEA(136));
            if (p135)
            {
                if (build >= 18362) ((fnSetPreferredAppMode)p135)(2); // ForceDark
                else                ((fnAllowDarkModeForApp)p135)(true);
                g_darkOk = true;
            }
            if (flush) flush();
        }
    }
}

void NdmTheme::Shutdown()
{
    if (g_acc) { g_acc->Release(); g_acc = nullptr; }
    g_accTried = false;
    if (g_gdipToken)
    {
        GdiplusShutdown(g_gdipToken);
        g_gdipToken = 0;
    }
}

// ===========================================================================
// DPI
// ===========================================================================
UINT NdmTheme::GetDpi(HWND hwnd)
{
    typedef UINT (WINAPI* fn)(HWND);
    static fn p = (fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (p && hwnd)
    {
        UINT d = p(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(nullptr);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

BOOL NdmTheme::AdjustRectForDpi(RECT* rc, DWORD style, DWORD ex, UINT dpi)
{
    typedef BOOL (WINAPI* fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static fn p = (fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (p) return p(rc, style, FALSE, ex, dpi);
    return AdjustWindowRectEx(rc, style, FALSE, ex);
}

int NdmTheme::SysMetric(int idx, UINT dpi)
{
    typedef int (WINAPI* fn)(int, UINT);
    static fn p = (fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (p) return p(idx, dpi);
    return GetSystemMetrics(idx);
}

// ===========================================================================
// Window chrome
// ===========================================================================
void NdmTheme::DarkTitleBar(HWND hwnd)
{
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, 19, &on, sizeof(on));
    if (WinBuild() >= 22000)
    {
        COLORREF cap = NDM_CLR_HDR_TOP;          // DWMWA_CAPTION_COLOR (Win11)
        DwmSetWindowAttribute(hwnd, 35, &cap, sizeof(cap));
        COLORREF brd = NDM_CLR_BORDER_GOLD;      // DWMWA_BORDER_COLOR
        DwmSetWindowAttribute(hwnd, 34, &brd, sizeof(brd));
    }
}

void NdmTheme::DarkScrollbars(HWND hwnd)
{
    if (g_allowDarkWnd) g_allowDarkWnd(hwnd, true);
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

void NdmTheme::DarkTooltip(HWND hTip)
{
    if (g_allowDarkWnd) g_allowDarkWnd(hTip, true);
    SetWindowTheme(hTip, L"DarkMode_Explorer", nullptr);
}

void NdmTheme::ForceForeground(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);

    HWND  fg    = GetForegroundWindow();
    DWORD me    = GetCurrentThreadId();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool  att   = false;
    if (fgTid && fgTid != me)
        att = AttachThreadInput(me, fgTid, TRUE) != FALSE;

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (att) AttachThreadInput(me, fgTid, FALSE);

    if (GetForegroundWindow() != hwnd)
    {
        FLASHWINFO fi = { sizeof(fi), hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0 };
        FlashWindowEx(&fi);
    }
}

POINT NdmTheme::CreationPoint(HWND owner)
{
    POINT pt = {};
    RECT rc;
    if (owner && IsWindowVisible(owner) && !IsIconic(owner) && GetWindowRect(owner, &rc))
    {
        pt.x = (rc.left + rc.right) / 2;
        pt.y = (rc.top + rc.bottom) / 2;
    }
    else
    {
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi))
        {
            pt.x = (mi.rcWork.left + mi.rcWork.right) / 2;
            pt.y = (mi.rcWork.top + mi.rcWork.bottom) / 2;
        }
    }
    return pt;
}

void NdmTheme::PlaceWindow(HWND hwnd, int cw96, int ch96, HWND owner)
{
    UINT  dpi   = GetDpi(hwnd);
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT  r     = { 0, 0, Scale(cw96, dpi), Scale(ch96, dpi) };
    AdjustRectForDpi(&r, style, ex, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;

    POINT c = CreationPoint(owner);
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int x = c.x - w / 2, y = c.y - h / 2;
    if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ===========================================================================
// Modeless registry
// ===========================================================================
void NdmTheme::AddModeless(HWND hwnd)
{
    if (std::find(g_modeless.begin(), g_modeless.end(), hwnd) == g_modeless.end())
        g_modeless.push_back(hwnd);
}

void NdmTheme::RemoveModeless(HWND hwnd)
{
    g_modeless.erase(std::remove(g_modeless.begin(), g_modeless.end(), hwnd),
                     g_modeless.end());
}

bool NdmTheme::PreTranslate(MSG* msg)
{
    // Copy: IsDialogMessage may destroy a window (Esc -> close).
    std::vector<HWND> wins = g_modeless;
    for (HWND h : wins)
    {
        if (!IsWindow(h)) continue;
        if (msg->hwnd == h || IsChild(h, msg->hwnd))
        {
            if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB &&
                (GetKeyState(VK_CONTROL) & 0x8000) &&
                SendMessageW(h, WM_NDM_CTRLTAB, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, 0))
                return true;
            return IsDialogMessageW(h, msg) != FALSE;
        }
    }
    return false;
}

void NdmTheme::CloseAllModeless()
{
    std::vector<HWND> wins = g_modeless;
    for (HWND h : wins)
        if (IsWindow(h)) DestroyWindow(h);
    g_modeless.clear();
}

// ===========================================================================
// Fonts
// ===========================================================================
HFONT NdmTheme::MakeFont(int px96, int weight, UINT dpi, const wchar_t* face)
{
    return CreateFontW(-MulDiv(px96, (int)dpi, 96), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

// ===========================================================================
// GDI primitives
// ===========================================================================
void NdmTheme::Fill(HDC hdc, const RECT& rc, COLORREF c)
{
    COLORREF old = SetBkColor(hdc, c);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
    SetBkColor(hdc, old);
}

void NdmTheme::GradientV(HDC hdc, const RECT& rc, COLORREF top, COLORREF bot)
{
    TRIVERTEX v[2] = {
        { rc.left,  rc.top,    (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8),
                               (COLOR16)(GetBValue(top) << 8), 0xFF00 },
        { rc.right, rc.bottom, (COLOR16)(GetRValue(bot) << 8), (COLOR16)(GetGValue(bot) << 8),
                               (COLOR16)(GetBValue(bot) << 8), 0xFF00 } };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

void NdmTheme::HLine(HDC hdc, int x1, int x2, int y, COLORREF c)
{
    RECT r = { x1, y, x2, y + 1 };
    Fill(hdc, r, c);
}

void NdmTheme::VLine(HDC hdc, int x, int y1, int y2, COLORREF c)
{
    RECT r = { x, y1, x + 1, y2 };
    Fill(hdc, r, c);
}

void NdmTheme::Text(HDC hdc, const std::wstring& s, RECT rc, COLORREF c,
                    HFONT f, UINT fmt)
{
    HGDIOBJ old = f ? SelectObject(hdc, f) : nullptr;
    int      bm = SetBkMode(hdc, TRANSPARENT);
    COLORREF oc = SetTextColor(hdc, c);
    DrawTextW(hdc, s.c_str(), (int)s.size(), &rc, fmt | DT_NOPREFIX);
    SetTextColor(hdc, oc);
    SetBkMode(hdc, bm);
    if (old) SelectObject(hdc, old);
}

SIZE NdmTheme::MeasureText(HDC hdc, const std::wstring& s, HFONT f)
{
    SIZE sz = {};
    HGDIOBJ old = f ? SelectObject(hdc, f) : nullptr;
    GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    if (old) SelectObject(hdc, old);
    return sz;
}

COLORREF NdmTheme::Blend(COLORREF a, COLORREF b, int t)
{
    if (t < 0) t = 0; if (t > 255) t = 255;
    return RGB((GetRValue(a) * (255 - t) + GetRValue(b) * t) / 255,
               (GetGValue(a) * (255 - t) + GetGValue(b) * t) / 255,
               (GetBValue(a) * (255 - t) + GetBValue(b) * t) / 255);
}

// ===========================================================================
// GDI+ shapes
// ===========================================================================
void NdmTheme::RoundFill(HDC hdc, const RECT& rc, int radius, COLORREF c, BYTE a)
{
    Graphics g(hdc); PrepG(g);
    GraphicsPath p; RoundPath(p, RF(rc), (float)radius);
    SolidBrush b(GC(c, a));
    g.FillPath(&b, &p);
}

void NdmTheme::RoundFillV(HDC hdc, const RECT& rc, int radius, COLORREF top, COLORREF bot)
{
    if (rc.bottom <= rc.top || rc.right <= rc.left) return;
    Graphics g(hdc); PrepG(g);
    RectF r = RF(rc);
    GraphicsPath p; RoundPath(p, r, (float)radius);
    LinearGradientBrush b(PointF(r.X, r.Y - 1), PointF(r.X, r.Y + r.Height + 1),
                          GC(top), GC(bot));
    g.FillPath(&b, &p);
}

void NdmTheme::RoundFrame(HDC hdc, const RECT& rc, int radius, COLORREF c,
                          float width, BYTE a)
{
    Graphics g(hdc); PrepG(g);
    RectF r = RF(rc);
    float in = width / 2.0f;
    r.X += in; r.Y += in; r.Width -= width; r.Height -= width;
    GraphicsPath p; RoundPath(p, r, (float)radius - in);
    Pen pen(GC(c, a), width);
    g.DrawPath(&pen, &p);
}

void NdmTheme::Circle(HDC hdc, int cx, int cy, int r, COLORREF c)
{
    Graphics g(hdc); PrepG(g);
    SolidBrush b(GC(c));
    g.FillEllipse(&b, (REAL)(cx - r), (REAL)(cy - r), (REAL)(2 * r), (REAL)(2 * r));
}

void NdmTheme::Glow(HDC hdc, const RECT& rc, COLORREF c, BYTE alpha)
{
    Graphics g(hdc); PrepG(g);
    GraphicsPath p;
    p.AddEllipse(RF(rc));
    PathGradientBrush b(&p);
    b.SetCenterColor(GC(c, alpha));
    Color edge = GC(c, 0);
    INT n = 1;
    b.SetSurroundColors(&edge, &n);
    g.FillPath(&b, &p);
}

void NdmTheme::ProgressBar(HDC hdc, const RECT& rc, double frac,
                           COLORREF fill, COLORREF fillLight, int phase)
{
    int h = rc.bottom - rc.top, w = rc.right - rc.left;
    if (h <= 0 || w <= 0) return;
    Graphics g(hdc); PrepG(g);
    RectF tr = RF(rc);
    float rad = h / 2.0f;
    GraphicsPath track; RoundPath(track, tr, rad);
    SolidBrush tb(GC(RGB(38, 42, 51)));
    g.FillPath(&tb, &track);

    RectF fr = tr;
    if (frac < 0)
    {
        // indeterminate: moving gold capsule clipped to the track
        float seg = (std::max)(tr.Width * 0.28f, (float)h * 2);
        float pos = (float)((phase % 120) / 120.0) * (tr.Width + seg) - seg;
        fr.X = tr.X + pos; fr.Width = seg;
        g.SetClip(&track);
    }
    else
    {
        if (frac > 1) frac = 1;
        fr.Width = (REAL)(tr.Width * frac);
        if (fr.Width <= 0.5f) return;
        if (fr.Width < h) fr.Width = (REAL)(std::min)((float)h, tr.Width);
    }
    GraphicsPath fp; RoundPath(fp, fr, rad);
    LinearGradientBrush gb(PointF(fr.X - 1, 0), PointF(fr.X + fr.Width + 1, 0),
                           GC(fill), GC(fillLight));
    g.FillPath(&gb, &fp);
    // glossy top highlight
    if (h >= 6)
    {
        RectF hr = fr; hr.Height = fr.Height / 2.0f;
        GraphicsPath hp; RoundPath(hp, hr, hr.Height / 2.0f);
        SolidBrush hb(Color(46, 255, 255, 255));
        g.FillPath(&hb, &hp);
    }
}

void NdmTheme::GoldRule(HDC hdc, int x1, int x2, int y)
{
    if (x2 <= x1) return;
    Graphics g(hdc);
    LinearGradientBrush b(PointF((REAL)x1 - 1, 0), PointF((REAL)x2 + 1, 0),
                          Color(0, 212, 175, 55), Color(0, 212, 175, 55));
    Color cols[4] = { Color(0, 212, 175, 55), Color(255, 212, 175, 55),
                      Color(255, 240, 210, 100), Color(0, 212, 175, 55) };
    REAL  pos[4]  = { 0.0f, 0.25f, 0.6f, 1.0f };
    b.SetInterpolationColors(cols, pos, 4);
    g.FillRectangle(&b, (REAL)x1, (REAL)y, (REAL)(x2 - x1), 1.0f);
}

void NdmTheme::Sparkline(HDC hdc, const RECT& rc, const DWORD* v, int n)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 2 || h <= 2) return;
    Graphics g(hdc); PrepG(g);
    // baseline
    SolidBrush base(Color(60, 140, 110, 28));
    g.FillRectangle(&base, (REAL)rc.left, (REAL)rc.bottom - 1, (REAL)w, 1.0f);
    if (!v || n < 2) return;
    DWORD mx = 1;
    bool any = false;
    for (int i = 0; i < n; ++i) { mx = (std::max)(mx, v[i]); if (v[i]) any = true; }
    if (!any) return;                       // idle: baseline only
    std::vector<PointF> pts;
    pts.reserve(n + 2);
    for (int i = 0; i < n; ++i)
    {
        float x = rc.left + (float)i * (w - 1) / (float)(n - 1);
        float y = rc.bottom - 2 - (float)v[i] / (float)mx * (h - 4);
        pts.push_back(PointF(x, y));
    }
    std::vector<PointF> area = pts;
    area.push_back(PointF((REAL)rc.right - 1, (REAL)rc.bottom));
    area.push_back(PointF((REAL)rc.left, (REAL)rc.bottom));
    LinearGradientBrush fill(PointF(0, (REAL)rc.top), PointF(0, (REAL)rc.bottom + 1),
                             Color(90, 212, 175, 55), Color(0, 212, 175, 55));
    g.FillPolygon(&fill, area.data(), (INT)area.size());
    Pen pen(Color(255, 236, 200, 90), 1.6f);
    pen.SetLineJoin(LineJoinRound);
    g.DrawLines(&pen, pts.data(), (INT)pts.size());
}

// ---------------------------------------------------------------------------
// Brand mark: gold-rimmed dark rounded tile, gold download arrow + base bar.
//
// THE single definition of the NDM logo. tools/make_icons.ps1 mirrors
// LogoGeom() exactly to render ndm.ico and the browser-extension PNGs, so the
// in-app logo, the window/taskbar/tray icon and the extension icon are the
// same picture. Coordinates are pixel edges (PixelOffsetModeHalf), every
// straight edge lands on the pixel grid; 16/20/24 px use hand-tuned layouts.
// ---------------------------------------------------------------------------
namespace
{
    struct LogoG
    {
        float r, bw, sx0, sx1, sy0, hy, hx0, hx1, ty, bx0, bx1, by0, by1, cap;
    };

    int RoundPx(double v) { return (int)std::floor(v + 0.5); }

    LogoG LogoGeom(int s)
    {
        switch (s)
        {
        case 16: return { 3, 1, 6, 10, 3,  6, 4, 12, 10, 3, 13, 11, 13, 0 };
        case 20: return { 4, 1, 8, 12, 4,  8, 5, 15, 13, 4, 16, 14, 16, 0 };
        case 24: return { 5, 1, 9, 15, 4, 11, 6, 18, 17, 5, 19, 18, 20, 0 };
        }
        int r  = RoundPx(0.22 * s);
        int bw = (std::max)(1, RoundPx(s / 26.0));
        int w  = RoundPx(0.19 * s);
        if (((s - w) % 2) != 0) w += 1;
        int sx0 = (s - w) / 2;
        int hh  = RoundPx(0.27 * s);
        int T   = RoundPx(0.17 * s);
        int hy  = RoundPx(0.43 * s);
        int ty  = hy + hh;
        int gap = (std::max)(1, RoundPx(0.05 * s));
        int by0 = ty + gap;
        int bh  = (std::max)(2, RoundPx(0.08 * s));
        int bot = by0 + bh;
        int dy  = RoundPx(((s - bot) - T) / 2.0);
        int bx0 = RoundPx(0.19 * s);
        LogoG g;
        g.r = (float)r; g.bw = (float)bw;
        g.sx0 = (float)sx0; g.sx1 = (float)(sx0 + w); g.sy0 = (float)(T + dy);
        g.hy = (float)(hy + dy);
        g.hx0 = s / 2.0f - hh; g.hx1 = s / 2.0f + hh; g.ty = (float)(ty + dy);
        g.bx0 = (float)bx0; g.bx1 = (float)(s - bx0);
        g.by0 = (float)(by0 + dy); g.by1 = (float)(bot + dy);
        g.cap = s >= 32 ? bh / 2.0f : 0.0f;
        return g;
    }

    void VBrushFill(Graphics& g, GraphicsPath& p, float y0, float y1, Color top, Color bot)
    {
        LinearGradientBrush b(PointF(0, y0 - 1), PointF(0, y1 + 1), top, bot);
        g.FillPath(&b, &p);
    }
}

void NdmTheme::Logo(HDC hdc, const RECT& rc)
{
    int s = (std::min)(rc.right - rc.left, rc.bottom - rc.top);
    if (s < 8) return;
    // whole-pixel origin keeps every edge on the grid
    int ox = rc.left + ((rc.right - rc.left) - s) / 2;
    int oy = rc.top  + ((rc.bottom - rc.top) - s) / 2;
    LogoG L = LogoGeom(s);

    Graphics g(hdc); PrepG(g);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.TranslateTransform((REAL)ox, (REAL)oy);
    float fs = (float)s;

    GraphicsPath outer; RoundPath(outer, RectF(0, 0, fs, fs), L.r);
    VBrushFill(g, outer, 0, fs, Color(255, 246, 214, 110), Color(255, 176, 136, 34));
    GraphicsPath inner;
    RoundPath(inner, RectF(L.bw, L.bw, fs - 2 * L.bw, fs - 2 * L.bw), (std::max)(0.0f, L.r - L.bw));
    VBrushFill(g, inner, 0, fs, Color(255, 46, 50, 60), Color(255, 17, 19, 23));

    Color aT(255, 255, 228, 130), aB(255, 222, 176, 52);
    GraphicsPath shaft; RoundPath(shaft, RectF(L.sx0, L.sy0, L.sx1 - L.sx0, L.hy + 1 - L.sy0), 0);
    VBrushFill(g, shaft, L.sy0, L.by1, aT, aB);
    PointF head[3] = { PointF(L.hx0, L.hy), PointF(L.hx1, L.hy), PointF(fs / 2.0f, L.ty) };
    GraphicsPath hp; hp.AddPolygon(head, 3);
    VBrushFill(g, hp, L.sy0, L.by1, aT, aB);
    GraphicsPath bar; RoundPath(bar, RectF(L.bx0, L.by0, L.bx1 - L.bx0, L.by1 - L.by0), L.cap);
    VBrushFill(g, bar, L.sy0, L.by1, aT, aB);
}

// ---------------------------------------------------------------------------
// Application icon at exact sizes (resource IDI_NDM = 101)
// ---------------------------------------------------------------------------
HICON NdmTheme::AppIcon(int px)
{
    static std::vector<std::pair<int, HICON>> cache;
    for (auto& e : cache) if (e.first == px) return e.second;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HICON ic = nullptr;
    // picks the matching frame, or scales DOWN from a larger one (never up)
    if (FAILED(LoadIconWithScaleDown(hi, MAKEINTRESOURCEW(101), px, px, &ic)) || !ic)
        ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, 0);
    if (ic) cache.push_back({ px, ic });
    return ic;
}

void NdmTheme::SetWindowIcons(HWND hwnd, UINT dpi)
{
    if (!hwnd) return;
    if (!dpi) dpi = GetDpi(hwnd);
    HICON sm = AppIcon(SysMetric(SM_CXSMICON, dpi));
    HICON bg = AppIcon(SysMetric(SM_CXICON, dpi));
    if (sm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    if (bg) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)bg);
}

// ---------------------------------------------------------------------------
// Glyphs -- designed on a 24x24 grid
// ---------------------------------------------------------------------------
void NdmTheme::Glyph(HDC hdc, NdmGlyph gl, const RECT& rc, COLORREF c, float strokeScale)
{
    if (gl == GLY_NONE) return;
    Graphics g(hdc); PrepG(g);
    float side = (float)(std::min)(rc.right - rc.left, rc.bottom - rc.top);
    float s  = side / 24.0f;
    float ox = rc.left + ((rc.right - rc.left) - side) / 2.0f;
    float oy = rc.top  + ((rc.bottom - rc.top) - side) / 2.0f;
    auto P = [&](float u, float v) { return PointF(ox + u * s, oy + v * s); };
    auto R = [&](float u, float v, float w, float h) { return RectF(ox + u * s, oy + v * s, w * s, h * s); };

    Pen pen(GC(c), (std::max)(1.0f, 1.9f * s * strokeScale));
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush br(GC(c));

    auto Line  = [&](float a, float b, float c2, float d) { g.DrawLine(&pen, P(a, b), P(c2, d)); };
    auto Poly  = [&](std::initializer_list<PointF> pts, bool closed, bool fill)
    {
        std::vector<PointF> v(pts);
        if (fill) { g.FillPolygon(&br, v.data(), (INT)v.size()); g.DrawPolygon(&pen, v.data(), (INT)v.size()); }
        else if (closed) g.DrawPolygon(&pen, v.data(), (INT)v.size());
        else g.DrawLines(&pen, v.data(), (INT)v.size());
    };
    auto RoundR = [&](RectF r, float rad, bool fill)
    {
        GraphicsPath p; RoundPath(p, r, rad * s);
        if (fill) g.FillPath(&br, &p); else g.DrawPath(&pen, &p);
    };
    auto Circ = [&](float cx, float cy, float r, bool fill)
    {
        RectF e = R(cx - r, cy - r, 2 * r, 2 * r);
        if (fill) g.FillEllipse(&br, e); else g.DrawEllipse(&pen, e);
    };

    switch (gl)
    {
    case GLY_ADD:                       // plain plus, Lucide geometry (no ring)
        Line(12, 5, 12, 19); Line(5, 12, 19, 12);
        break;
    case GLY_PLUS:
        Line(12, 6, 12, 18); Line(6, 12, 18, 12);
        break;
    case GLY_MINUS:
        Line(6, 12, 18, 12);
        break;
    case GLY_RESUME:
        Poly({ P(8, 5.5f), P(18.5f, 12), P(8, 18.5f) }, true, true);
        break;
    case GLY_PAUSE:
        RoundR(R(6.5f, 5, 4, 14), 1.2f, true);
        RoundR(R(13.5f, 5, 4, 14), 1.2f, true);
        break;
    case GLY_STOPALL:
        RoundR(R(9, 4, 11, 11), 2.0f, false);
        RoundR(R(4, 9, 11, 11), 2.0f, true);
        break;
    case GLY_DELETE:
        Line(4.5f, 6.5f, 19.5f, 6.5f);
        Poly({ P(9.5f, 6.5f), P(9.5f, 4), P(14.5f, 4), P(14.5f, 6.5f) }, false, false);
        Poly({ P(6.5f, 6.5f), P(7.5f, 20), P(16.5f, 20), P(17.5f, 6.5f) }, false, false);
        Line(10.3f, 10.5f, 10.3f, 16.5f); Line(13.7f, 10.5f, 13.7f, 16.5f);
        break;
    case GLY_FOLDER:
        Poly({ P(3, 6), P(9.5f, 6), P(11.5f, 8.5f), P(21, 8.5f), P(21, 19), P(3, 19) }, true, false);
        Line(3, 11.5f, 21, 11.5f);
        break;
    case GLY_OPTIONS:
    {
        std::vector<PointF> pts;
        const float rin = 7.0f, rout = 9.8f, PI = 3.14159265f;
        for (int k = 0; k < 8; ++k)
        {
            float cdeg = k * 45.0f;
            float a[4] = { cdeg - 15, cdeg - 8, cdeg + 8, cdeg + 15 };
            float r[4] = { rin, rout, rout, rin };
            for (int i = 0; i < 4; ++i)
            {
                float rad = a[i] * PI / 180.0f;
                pts.push_back(P(12 + r[i] * cosf(rad), 12 + r[i] * sinf(rad)));
            }
        }
        g.DrawPolygon(&pen, pts.data(), (INT)pts.size());
        Circ(12, 12, 3.2f, false);
        break;
    }
    case GLY_ALL:
        Line(12, 3.5f, 12, 13.5f);
        Poly({ P(8, 9.5f), P(12, 13.5f), P(16, 9.5f) }, false, false);
        Poly({ P(4, 13), P(4, 19.5f), P(20, 19.5f), P(20, 13) }, false, false);
        break;
    case GLY_ZIP:
        RoundR(R(5, 3, 14, 18), 2.0f, false);
        Line(10.5f, 5.5f, 12, 5.5f); Line(12, 7.5f, 13.5f, 7.5f);
        Line(10.5f, 9.5f, 12, 9.5f); Line(12, 11.5f, 13.5f, 11.5f);
        RoundR(R(10.2f, 13.5f, 3.6f, 4.5f), 0.8f, true);
        break;
    case GLY_DOC:
    case GLY_FILE:
        Poly({ P(6, 3), P(14, 3), P(19, 8), P(19, 21), P(6, 21) }, true, false);
        Poly({ P(14, 3), P(14, 8), P(19, 8) }, false, false);
        if (gl == GLY_DOC)
        {
            Line(9, 12, 16, 12); Line(9, 15, 16, 15); Line(9, 18, 13, 18);
        }
        break;
    case GLY_MUSIC:
        Poly({ P(9.5f, 17), P(9.5f, 5.5f), P(19, 3.5f), P(19, 15) }, false, false);
        Line(9.5f, 9, 19, 7);
        Circ(7.3f, 17.2f, 2.6f, true);
        Circ(16.8f, 15.2f, 2.6f, true);
        break;
    case GLY_PROGRAM:
        RoundR(R(3, 4.5f, 18, 15), 2.0f, false);
        Line(3, 8.5f, 21, 8.5f);
        Poly({ P(7, 11.5f), P(9.5f, 13.7f), P(7, 16) }, false, false);
        Line(11.5f, 16, 16, 16);
        break;
    case GLY_VIDEO:
        RoundR(R(2.5f, 6, 13.5f, 12), 2.0f, false);
        Poly({ P(16, 10.5f), P(21.5f, 7), P(21.5f, 17), P(16, 13.5f) }, true, false);
        break;
    case GLY_PICTURE:
        RoundR(R(3, 4.5f, 18, 15), 2.0f, false);
        Circ(8.5f, 9.5f, 1.8f, true);
        Poly({ P(3.5f, 17.5f), P(9.5f, 12.5f), P(13, 15.5f), P(16, 13), P(20.5f, 17) }, false, false);
        break;
    case GLY_UNFINISHED:
        Circ(12, 12, 9, false);
        Poly({ P(12, 7), P(12, 12), P(15.5f, 14.5f) }, false, false);
        break;
    case GLY_FINISHED:
        Circ(12, 12, 9, false);
        Poly({ P(7.8f, 12.3f), P(10.7f, 15.2f), P(16.4f, 9.2f) }, false, false);
        break;
    case GLY_CHECK:
        Poly({ P(5, 12.5f), P(10, 17.5f), P(19, 7) }, false, false);
        break;
    case GLY_DOWNARROW:
        Line(12, 3.5f, 12, 18);
        Poly({ P(6.5f, 12.5f), P(12, 18), P(17.5f, 12.5f) }, false, false);
        Line(5, 21, 19, 21);
        break;
    case GLY_GLOBE:
    case GLY_LINK:
        Circ(12, 12, 9, false);
        g.DrawEllipse(&pen, R(8, 3, 8, 18));
        Line(3, 12, 21, 12);
        break;
    case GLY_INFO:
        Circ(12, 12, 9, false);
        Line(12, 11, 12, 16.5f);
        Circ(12, 7.6f, 1.2f, true);
        break;
    case GLY_OPEN:
        Poly({ P(10, 4.5f), P(4.5f, 4.5f), P(4.5f, 19.5f), P(19.5f, 19.5f), P(19.5f, 14) }, false, false);
        Poly({ P(13.5f, 4.5f), P(19.5f, 4.5f), P(19.5f, 10.5f) }, false, false);
        Line(19.5f, 4.5f, 11, 13);
        break;
    case GLY_CLOSE:
        Line(6.5f, 6.5f, 17.5f, 17.5f); Line(17.5f, 6.5f, 6.5f, 17.5f);
        break;
    case GLY_REFRESH:
        g.DrawArc(&pen, R(4, 4, 16, 16), -60, 300);
        Poly({ P(15.5f, 3), P(20, 5.2f), P(17.5f, 9.5f) }, false, false);
        break;
    case GLY_MAGNET:
        // horseshoe magnet: U arc with two pole caps
        g.DrawArc(&pen, R(5, 6, 14, 14), 0, 180);
        Line(5, 13, 5, 5); Line(19, 13, 19, 5);
        RoundR(R(3.6f, 3.5f, 2.8f, 3.2f), 0.5f, true); RoundR(R(17.6f, 3.5f, 2.8f, 3.2f), 0.5f, true);
        break;
    case GLY_BROWSER:
        RoundR(R(3, 4.5f, 18, 15), 2.0f, false);
        Line(3, 8.5f, 21, 8.5f);
        Circ(6, 6.5f, 0.8f, true); Circ(8.5f, 6.5f, 0.8f, true);
        break;
    case GLY_CLOCK:
        Circ(12, 12, 9, false);
        Line(12, 7, 12, 12); Line(12, 12, 15.5f, 14);
        break;
    case GLY_SEARCH:
        Circ(10.5f, 10.5f, 6, false);
        Line(15, 15, 20, 20);
        break;
    default: break;
    }
}

void NdmTheme::GlyphTile(HDC hdc, const RECT& rc, NdmGlyph gl, UINT dpi)
{
    int rad = Scale(8, dpi);
    RoundFillV(hdc, rc, rad, RGB(60, 52, 18), RGB(34, 30, 14));
    RoundFrame(hdc, rc, rad, NDM_CLR_BORDER_GOLD, 1.0f);
    RECT gr = rc;
    int pad = (rc.right - rc.left) / 4;
    InflateRect(&gr, -pad, -pad);
    Glyph(hdc, gl, gr, NDM_CLR_GOLD_LIGHT, 1.1f);
}

// ===========================================================================
// Controls
// ===========================================================================
void NdmTheme::MakeButton(HWND hBtn)
{
    if (hBtn) SetWindowSubclass(hBtn, BtnSubclass, 0x4E44, 0);
}

void NdmTheme::MakeToggle(HWND hBtn)
{
    if (!hBtn) return;
    SetPropW(hBtn, PROP_TOGG, (HANDLE)1);
    SetWindowSubclass(hBtn, BtnSubclass, 0x4E44, 0);
    if (IAccPropServices* acc = AccSvc())
    {
        ToggleAccServer* srv = new ToggleAccServer(hBtn);
        acc->SetHwndPropServer(hBtn, (DWORD)OBJID_CLIENT, CHILDID_SELF, kToggleProps, 3, srv, ANNO_THIS);
        srv->Release();                    // oleacc keeps its own reference
    }
}

void NdmTheme::SetAccName(HWND hCtl, const wchar_t* name)
{
    if (!hCtl) return;
    IAccPropServices* acc = AccSvc();
    if (!acc) return;
    MSAAPROPID p = PROPID_ACC_NAME;
    if (!name || !*name)
    {
        acc->ClearHwndProps(hCtl, (DWORD)OBJID_CLIENT, CHILDID_SELF, &p, 1);
        return;
    }
    if (SUCCEEDED(acc->SetHwndPropStr(hCtl, (DWORD)OBJID_CLIENT, CHILDID_SELF, p, name)))
    {
        SetWindowSubclass(hCtl, AccNameSubclass, 0x4E45, 0);   // clears it on destroy
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hCtl, OBJID_CLIENT, CHILDID_SELF);
    }
}

bool NdmTheme::IsHot(HWND hBtn)
{
    return GetPropW(hBtn, PROP_HOT) != nullptr;
}

void NdmTheme::SetBtnGlyph(HWND hBtn, NdmGlyph gl)
{
    if (hBtn) SetPropW(hBtn, PROP_GLYPH, (HANDLE)(INT_PTR)gl);
}

void NdmTheme::DrawButton(const DRAWITEMSTRUCT* di, NdmBtnKind kind,
                          COLORREF parentBg, HFONT font, UINT dpi)
{
    if (!di || di->CtlType != ODT_BUTTON) return;
    RECT rc = di->rcItem;
    NdmMemDC mem(di->hDC, rc);
    HDC dc = mem.ok() ? mem.dc : di->hDC;

    bool pressed  = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    bool focus    = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    bool hot      = !disabled && IsHot(di->hwndItem);
    int  rad      = Scale(4, dpi);

    wchar_t txt[128] = {};
    GetWindowTextW(di->hwndItem, txt, 128);
    NdmGlyph gl = (NdmGlyph)(INT_PTR)GetPropW(di->hwndItem, PROP_GLYPH);

    Fill(dc, rc, parentBg);
    RECT body = rc;
    COLORREF fg = NDM_CLR_FG;

    switch (kind)
    {
    case BTN_PRIMARY:
        if (disabled)
        {
            RoundFill(dc, body, rad, RGB(58, 54, 40));
            fg = RGB(120, 114, 96);
        }
        else
        {
            COLORREF t = pressed ? NDM_CLR_GOLD_DARK : (hot ? RGB(250, 222, 120) : NDM_CLR_GOLD_LIGHT);
            COLORREF b = pressed ? RGB(140, 110, 26) : (hot ? NDM_CLR_GOLD : RGB(190, 152, 40));
            RoundFillV(dc, body, rad, t, b);
            RoundFrame(dc, body, rad, RGB(255, 236, 160), 1.0f, pressed ? 40 : 90);
            fg = RGB(24, 20, 8);
        }
        break;
    case BTN_SECONDARY:
    case BTN_STEPPER:
    case BTN_DANGER:
    {
        COLORREF bg = disabled ? NDM_CLR_BG_PANEL
                    : pressed ? NDM_CLR_BG_SEL : (hot ? NDM_CLR_BG_HOVER : RGB(34, 38, 46));
        COLORREF bd = disabled ? NDM_CLR_BORDER
                    : (kind == BTN_DANGER ? NDM_CLR_RED
                    : ((hot || pressed || focus) ? NDM_CLR_BORDER_GOLD : NDM_CLR_BORDER));
        RoundFill(dc, body, rad, bg);
        RoundFrame(dc, body, rad, bd, 1.0f);
        fg = disabled ? NDM_CLR_DISABLED
           : (kind == BTN_DANGER ? RGB(240, 120, 120) : (hot ? NDM_CLR_GOLD_LIGHT : NDM_CLR_FG));
        break;
    }
    case BTN_GHOST:
        if (!disabled && (hot || pressed))
            RoundFill(dc, body, rad, pressed ? NDM_CLR_BG_SEL : NDM_CLR_BG_HOVER);
        fg = disabled ? NDM_CLR_DISABLED : (hot ? NDM_CLR_GOLD_LIGHT : NDM_CLR_GOLD);
        break;
    case BTN_TOGGLE:
    {
        bool on = GetPropW(di->hwndItem, PROP_CHECK) != nullptr;
        int th = Scale(16, dpi), tw = Scale(30, dpi);
        int ty = rc.top + ((rc.bottom - rc.top) - th) / 2;
        RECT tr = { rc.left + Scale(2, dpi), ty, rc.left + Scale(2, dpi) + tw, ty + th };
        if (on)
        {
            RoundFillV(dc, tr, th / 2, disabled ? RGB(110, 94, 40) : NDM_CLR_GOLD_LIGHT,
                       disabled ? RGB(90, 76, 30) : NDM_CLR_GOLD_DARK);
        }
        else
        {
            RoundFill(dc, tr, th / 2, hot ? RGB(62, 67, 80) : RGB(50, 54, 64));
            RoundFrame(dc, tr, th / 2, NDM_CLR_BORDER, 1.0f);
        }
        if (focus) RoundFrame(dc, { tr.left - 2, tr.top - 2, tr.right + 2, tr.bottom + 2 },
                              th / 2 + 2, NDM_CLR_GOLD, 1.0f, 160);
        int kr = th / 2 - Scale(2, dpi);
        int kx = on ? tr.right - th / 2 : tr.left + th / 2;
        Circle(dc, kx, ty + th / 2, kr, on ? RGB(30, 26, 10) : RGB(200, 202, 210));
        RECT lr = rc; lr.left = tr.right + Scale(10, dpi);
        Text(dc, txt, lr, disabled ? NDM_CLR_DISABLED : (hot ? RGB(240, 240, 244) : NDM_CLR_FG),
             font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;   // mem dtor blits
    }
    }

    if (focus && kind != BTN_PRIMARY)
        RoundFrame(dc, body, rad, NDM_CLR_GOLD, 1.0f, 200);
    else if (focus)
        RoundFrame(dc, { body.left + 2, body.top + 2, body.right - 2, body.bottom - 2 },
                   rad - 1, RGB(60, 48, 10), 1.0f, 120);

    int off = pressed ? 1 : 0;
    std::wstring t = txt;
    if (gl != GLY_NONE)
    {
        int gs = Scale(kind == BTN_STEPPER ? 12 : 14, dpi);
        SIZE ts = t.empty() ? SIZE{ 0, 0 } : MeasureText(dc, t, font);
        int gap = t.empty() ? 0 : Scale(6, dpi);
        int total = gs + gap + ts.cx;
        int x = rc.left + ((rc.right - rc.left) - total) / 2;
        int y = rc.top + ((rc.bottom - rc.top) - gs) / 2 + off;
        RECT gr = { x, y, x + gs, y + gs };
        Glyph(dc, gl, gr, fg, kind == BTN_STEPPER ? 1.1f : 1.0f);
        if (!t.empty())
        {
            RECT tr = { x + gs + gap, rc.top + off, rc.right, rc.bottom + off };
            Text(dc, t, tr, fg, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    else
    {
        RECT tr = rc; OffsetRect(&tr, 0, off);
        Text(dc, t, tr, fg, font, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void NdmTheme::DrawField(HDC hdc, const RECT& rc, bool focused, bool readOnly, UINT dpi)
{
    int rad = Scale(4, dpi);
    RoundFill(hdc, rc, rad, readOnly ? RGB(25, 28, 34) : NDM_CLR_EDIT_BG);
    RoundFrame(hdc, rc, rad, focused ? NDM_CLR_GOLD : NDM_CLR_BORDER, focused ? 1.5f : 1.0f);
}

// ===========================================================================
// Categories / helpers
// ===========================================================================
int NdmTheme::CategoryOfFile(const std::wstring& filename)
{
    size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos) return CAT_GENERAL;
    std::wstring ext = filename.substr(dot + 1);
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);

    static const wchar_t* zip[] = { L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz",
        L"tgz", L"iso", L"cab", L"arj", L"lzh", L"zst", L"img" };
    static const wchar_t* doc[] = { L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt",
        L"pptx", L"txt", L"rtf", L"odt", L"ods", L"odp", L"epub", L"csv", L"md", L"chm" };
    static const wchar_t* mus[] = { L"mp3", L"wav", L"flac", L"aac", L"ogg", L"m4a",
        L"wma", L"opus", L"aiff", L"mid" };
    static const wchar_t* prg[] = { L"exe", L"msi", L"apk", L"dmg", L"deb", L"rpm",
        L"msix", L"appx", L"bat", L"jar", L"appimage", L"pkg" };
    static const wchar_t* vid[] = { L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv",
        L"webm", L"m4v", L"mpg", L"mpeg", L"3gp", L"ts", L"vob", L"m2ts" };
    auto in = [&](const wchar_t* const* list, size_t n)
    {
        for (size_t i = 0; i < n; ++i) if (ext == list[i]) return true;
        return false;
    };
    if (in(zip, _countof(zip))) return CAT_COMPRESSED;
    if (in(doc, _countof(doc))) return CAT_DOCUMENTS;
    if (in(mus, _countof(mus))) return CAT_MUSIC;
    if (in(prg, _countof(prg))) return CAT_PROGRAMS;
    if (in(vid, _countof(vid))) return CAT_VIDEO;
    static const wchar_t* pic[] = { L"jpg", L"jpeg", L"png", L"gif", L"webp", L"bmp",
        L"tif", L"tiff", L"heic", L"heif", L"avif", L"svg", L"ico", L"raw", L"cr2", L"nef", L"dng" };
    if (in(pic, _countof(pic))) return CAT_PICTURES;
    return CAT_GENERAL;
}

const wchar_t* NdmTheme::CategoryName(int cat)
{
    switch (cat)
    {
    case CAT_ALL:        return L"All Downloads";
    case CAT_COMPRESSED: return L"Compressed";
    case CAT_DOCUMENTS:  return L"Documents";
    case CAT_MUSIC:      return L"Music";
    case CAT_PROGRAMS:   return L"Programs";
    case CAT_VIDEO:      return L"Video";
    case CAT_PICTURES:   return L"Pictures";
    case CAT_TORRENT:    return L"Torrents";
    case CAT_UNFINISHED: return L"Unfinished";
    case CAT_FINISHED:   return L"Finished";
    default:             return L"General";
    }
}

NdmGlyph NdmTheme::CategoryGlyph(int cat)
{
    switch (cat)
    {
    case CAT_ALL:        return GLY_ALL;
    case CAT_COMPRESSED: return GLY_ZIP;
    case CAT_DOCUMENTS:  return GLY_DOC;
    case CAT_MUSIC:      return GLY_MUSIC;
    case CAT_PROGRAMS:   return GLY_PROGRAM;
    case CAT_VIDEO:      return GLY_VIDEO;
    case CAT_PICTURES:   return GLY_PICTURE;
    case CAT_TORRENT:    return GLY_MAGNET;
    case CAT_UNFINISHED: return GLY_UNFINISHED;
    case CAT_FINISHED:   return GLY_FINISHED;
    default:             return GLY_FILE;
    }
}

std::wstring NdmTheme::JoinPath(const std::wstring& dir, const std::wstring& file)
{
    if (dir.empty()) return file;
    std::wstring r = dir;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    return r + file;
}

std::wstring NdmTheme::CategoryFolder(const std::wstring& base, int cat)
{
    std::wstring b = base;
    while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
    if (cat >= CAT_COMPRESSED && cat <= CAT_PICTURES)
        return JoinPath(b, CategoryName(cat));
    return b;
}

std::wstring NdmTheme::HostOf(const std::wstring& url)
{
    size_t p = url.find(L"://");
    size_t s = (p == std::wstring::npos) ? 0 : p + 3;
    size_t e = url.find_first_of(L"/?#", s);
    std::wstring h = url.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
    size_t at = h.find(L'@');
    if (at != std::wstring::npos) h = h.substr(at + 1);
    return h;
}

std::wstring NdmTheme::SanitizeFilename(const std::wstring& name)
{
    std::wstring r;
    for (wchar_t c : name)
    {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) r += L'_';
        else r += c;
    }
    size_t a = r.find_first_not_of(L" .");
    size_t b = r.find_last_not_of(L" .");
    if (a == std::wstring::npos) return L"";
    return r.substr(a, b - a + 1);
}

bool NdmTheme::EnsureFolder(const std::wstring& dir)
{
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS;
}

std::wstring NdmTheme::StatusText(NdmStatus st)
{
    switch (st)
    {
    case NDM_STATUS_QUEUED:      return L"Queued";
    case NDM_STATUS_DOWNLOADING: return L"Downloading";
    case NDM_STATUS_PAUSED:      return L"Paused";
    case NDM_STATUS_COMPLETED:   return L"Complete";
    case NDM_STATUS_ERROR:       return L"Error";
    }
    return L"Unknown";
}

COLORREF NdmTheme::StatusColor(NdmStatus st)
{
    switch (st)
    {
    case NDM_STATUS_DOWNLOADING: return NDM_CLR_GREEN;
    case NDM_STATUS_PAUSED:      return NDM_CLR_AMBER;
    case NDM_STATUS_COMPLETED:   return NDM_CLR_GOLD;
    case NDM_STATUS_ERROR:       return NDM_CLR_RED;
    default:                     return NDM_CLR_FG_DIM;
    }
}

std::wstring NdmTheme::BrowseFolder(HWND owner, const std::wstring& initial, const wchar_t* title)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (title) dlg->SetTitle(title);
        std::wstring start = initial;
        while (!start.empty() && GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            size_t p = start.find_last_of(L"\\/");
            if (p == std::wstring::npos) { start.clear(); break; }
            start.resize(p);
        }
        if (!start.empty())
        {
            IShellItem* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si))))
            {
                dlg->SetFolder(si);
                si->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(owner)))
        {
            IShellItem* si = nullptr;
            if (SUCCEEDED(dlg->GetResult(&si)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
                {
                    result = p;
                    CoTaskMemFree(p);
                }
                si->Release();
            }
        }
        dlg->Release();
        return result;
    }

    // Fallback (very old systems / COM failure)
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl)
    {
        wchar_t buf[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, buf)) result = buf;
        CoTaskMemFree(pidl);
    }
    return result;
}

void NdmTheme::OpenFolderSelect(const std::wstring& dir, const std::wstring& file)
{
    std::wstring full = JoinPath(dir, file);
    if (!file.empty() && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(full.c_str());
        if (pidl)
        {
            HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            if (SUCCEEDED(hr)) return;
        }
    }
    std::wstring d = dir;
    while (!d.empty() && GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        size_t p = d.find_last_of(L"\\/");
        if (p == std::wstring::npos) { d.clear(); break; }
        d.resize(p);
    }
    if (!d.empty())
        ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ===========================================================================
// Dialog helpers
// ===========================================================================
void NdmDlgRes::Create(UINT d)
{
    Free();
    dpi     = d ? d : 96;
    ui      = NdmTheme::MakeFont(12, FW_NORMAL,   dpi);
    bold    = NdmTheme::MakeFont(12, FW_SEMIBOLD, dpi);
    title   = NdmTheme::MakeFont(14, FW_SEMIBOLD, dpi);
    sm      = NdmTheme::MakeFont(11, FW_NORMAL,   dpi);
    caption = NdmTheme::MakeFont(10, FW_SEMIBOLD, dpi);
    big     = NdmTheme::MakeFont(17, FW_SEMIBOLD, dpi);
    edit    = CreateSolidBrush(NDM_CLR_EDIT_BG);
    read    = CreateSolidBrush(RGB(25, 28, 34));
}

void NdmDlgRes::Free()
{
    HFONT* fs[] = { &ui, &bold, &title, &sm, &caption, &big };
    for (HFONT* f : fs) if (*f) { DeleteObject(*f); *f = nullptr; }
    if (edit) { DeleteObject(edit); edit = nullptr; }
    if (read) { DeleteObject(read); read = nullptr; }
}

HWND NdmTheme::MakeEdit(HWND parent, int id, DWORD extra)
{
    HWND h = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
    if (h) SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    return h;
}

HWND NdmTheme::MakeBtn(HWND parent, int id, const wchar_t* text, NdmGlyph glyph, bool toggle)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
    if (!h) return nullptr;
    if (toggle) MakeToggle(h); else MakeButton(h);
    if (glyph != GLY_NONE) SetBtnGlyph(h, glyph);
    return h;
}

void NdmTheme::PlaceEdit(HWND edit, const RECT& f, HFONT font, int padX)
{
    if (!edit) return;
    HDC dc = GetDC(edit);
    TEXTMETRICW tm = {};
    HGDIOBJ o = SelectObject(dc, font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(edit, dc);
    // Single-line EDIT text is drawn at the top of the control: keep it
    // exactly one line tall so the text is vertically centred in the field.
    int h = tm.tmHeight;
    int y = f.top + ((f.bottom - f.top) - h + 1) / 2;
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    MoveWindow(edit, f.left + padX, y, (f.right - f.left) - 2 * padX, h, TRUE);
}

HBRUSH NdmTheme::EditColors(HDC hdc, HWND ctl, const NdmDlgRes& r)
{
    bool ro = (GetWindowLongPtrW(ctl, GWL_STYLE) & ES_READONLY) != 0 || !IsWindowEnabled(ctl);
    SetTextColor(hdc, ro ? RGB(176, 178, 186) : NDM_CLR_FG);
    SetBkColor(hdc, ro ? RGB(25, 28, 34) : NDM_CLR_EDIT_BG);
    return ro ? r.read : r.edit;
}

void NdmTheme::Caption(HDC hdc, const wchar_t* text, RECT rc, const NdmDlgRes& r, COLORREF c)
{
    int old = SetTextCharacterExtra(hdc, r.S(1));
    Text(hdc, text, rc, c, r.caption, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextCharacterExtra(hdc, old);
}

std::wstring NdmTheme::GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    if (n > 0) GetWindowTextW(h, &s[0], n + 1);
    s.resize(n > 0 ? wcslen(s.c_str()) : 0);
    return s;
}

// ===========================================================================
// NdmMemDC
// ===========================================================================
NdmMemDC::NdmMemDC(HDC target, const RECT& rc) : m_target(target), m_rc(rc)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    dc = CreateCompatibleDC(target);
    if (!dc) return;
    m_bmp = CreateCompatibleBitmap(target, w, h);
    if (!m_bmp) { DeleteDC(dc); dc = nullptr; return; }
    m_old = SelectObject(dc, m_bmp);
    SetWindowOrgEx(dc, rc.left, rc.top, nullptr);
}

NdmMemDC::~NdmMemDC()
{
    if (!dc) return;
    SetWindowOrgEx(dc, 0, 0, nullptr);
    BitBlt(m_target, m_rc.left, m_rc.top, m_rc.right - m_rc.left,
           m_rc.bottom - m_rc.top, dc, 0, 0, SRCCOPY);
    SelectObject(dc, m_old);
    DeleteObject(m_bmp);
    DeleteDC(dc);
}
