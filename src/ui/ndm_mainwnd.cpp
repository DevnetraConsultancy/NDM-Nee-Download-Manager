// ndm_mainwnd.cpp -- NDM main window (premium dark + gold)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// Layout:  toolbar of large owner-painted icon buttons + live speed sparkline
//          category sidebar | virtual (owner-data) custom-drawn ListView
//          status bar
// Everything outside the ListView is painted by this window into one
// double buffer; the ListView rows and its column header are custom drawn.

#include "ndm_mainwnd.h"
#include "ndm_adddlg.h"
#include "ndm_optdlg.h"
#include "ndm_progdlg.h"
#include "ndm_resource.h"
#include "ndm_firstrun.h"
#include "ndm_dialogs.h"
#include "ndm_power.h"
#include "ndm_scheddlg.h"
#include "ndm_uilogic.h"
#include "../torrent/ndm_torrent.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"
#include "../msghost/ndm_msghost.h"

#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <memory>
#include <set>
#include <mmsystem.h>
#include <ole2.h>
#include <oleidl.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace NdmTheme;

namespace
{
    // Surface colours used only here
    const COLORREF kRowAlt   = RGB(23, 26, 31);
    const COLORREF kRowHot   = RGB(31, 35, 42);
    const COLORREF kRowSel   = RGB(56, 49, 18);
    const COLORREF kRowSelHot= RGB(64, 56, 20);
    const COLORREF kRowLine  = RGB(29, 32, 38);
    const COLORREF kHdrBg    = RGB(24, 27, 33);
    const COLORREF kStatusBg = RGB(20, 22, 27);
    const COLORREF kSideSel  = RGB(48, 43, 18);

    const int kToolbarH = 50;
    const int kStatusH  = 22;
    const int kSidebarW = 168;
    const int kRowH     = 24;
    const int kColHdrH  = 24;
    const int kBtnW     = 54;
    const int kBtnH     = 44;
    const int kCatH     = 26;      // sidebar row
    const int kCatCapH  = 20;      // sidebar caption row

    // Default widths (96 dpi). The Address column takes whatever is left so
    // every column is visible at the default window size.
    const int kColWidth96[COL_COUNT] = { 170, 142, 74, 100, 84, 70, 76, 80 };
    const wchar_t* kColName[COL_COUNT] = {
        L"File Name", L"Date", L"Size", L"Status", L"Progress", L"Speed", L"Time Left", L"Address" };

    const int kSpeedHist = 60;
    std::wstring g_lastTip;

#define NDM_WIDEN2(x) L##x
#define NDM_WIDEN(x)  NDM_WIDEN2(x)
#define NDM_VERSION_WSTR NDM_WIDEN(NDM_VERSION_STR)

    std::wstring Fmt(const wchar_t* f, ...)
    {
        wchar_t buf[512];
        va_list ap; va_start(ap, f);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, f, ap);
        va_end(ap);
        return buf;
    }

    bool CopyToClipboard(HWND h, const std::wstring& s)
    {
        if (!OpenClipboard(h)) return false;
        EmptyClipboard();
        size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (g)
        {
            void* p = GlobalLock(g);
            if (p)
            {
                memcpy(p, s.c_str(), bytes);
                GlobalUnlock(g);
                ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
            }
            if (!ok) GlobalFree(g);
        }
        CloseClipboard();
        // our own copy: the clipboard watcher must not offer it back
        if (ok) NdmMainWindow::GetInstance().NoteOwnClipboard(GetClipboardSequenceNumber());
        return ok;
    }

    // Recycle Bin, never a permanent delete (Windows asks first when an item
    // cannot be recycled). true = gone from its place.
    bool RecycleFile(HWND owner, const std::wstring& path)
    {
        std::wstring from = path;
        from.push_back(L'\0');                 // double NUL terminated list
        SHFILEOPSTRUCTW op = {};
        op.hwnd   = owner;
        op.wFunc  = FO_DELETE;
        op.pFrom  = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
        int rc = SHFileOperationW(&op);
        return rc == 0 && !op.fAnyOperationsAborted &&
               GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    }

    std::wstring FirstLine(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_first_of(L"\r\n", a);
        std::wstring l = s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        while (!l.empty() && (l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        return l;
    }

    // -----------------------------------------------------------------------
    // Drop target: links / text from browsers, .torrent files from Explorer.
    // Only checks and copies the data; the main window acts on it later
    // (posted), so the drag source is never blocked by our dialogs.
    // -----------------------------------------------------------------------
    class DropTarget : public IDropTarget
    {
    public:
        explicit DropTarget(NdmMainWindow* w) : m_w(w) {}
        virtual ~DropTarget() = default;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
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
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* eff) override
        {
            m_items.clear();
            m_files = false;
            Extract(data);
            *eff = Effect(*eff);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override
        {
            *eff = Effect(*eff);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override { m_items.clear(); return S_OK; }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* eff) override
        {
            m_items.clear();
            m_files = false;
            Extract(data);
            *eff = Effect(*eff);
            if (!m_items.empty()) m_w->QueueDrop(m_items, m_files);
            m_items.clear();
            return S_OK;
        }
    private:
        DWORD Effect(DWORD allowed) const
        {
            if (m_items.empty()) return DROPEFFECT_NONE;
            if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
            if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
            return DROPEFFECT_NONE;
        }
        void Extract(IDataObject* data)
        {
            if (!data) return;
            FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM sm = {};
            if (SUCCEEDED(data->GetData(&fe, &sm)))
            {
                HDROP hd = (HDROP)GlobalLock(sm.hGlobal);
                if (hd)
                {
                    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < n && i < 32; ++i)
                    {
                        wchar_t p[MAX_PATH * 2] = {};
                        if (!DragQueryFileW(hd, i, p, (UINT)_countof(p))) continue;
                        std::wstring f = p;
                        if (f.size() > 8 && _wcsicmp(f.c_str() + f.size() - 8, L".torrent") == 0 &&
                            NdmMainWindow::AcceptableSource(f, NDM_ORIGIN_CMDLINE))
                            m_items.push_back(f);
                    }
                    GlobalUnlock(sm.hGlobal);
                }
                ReleaseStgMedium(&sm);
                m_files = true;
                return;
            }
            static const UINT cfUrlW = RegisterClipboardFormatW(L"UniformResourceLocatorW");
            const CLIPFORMAT fmts[2] = { (CLIPFORMAT)cfUrlW, CF_UNICODETEXT };
            for (CLIPFORMAT cf : fmts)
            {
                FORMATETC ft = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                STGMEDIUM st = {};
                if (!cf || FAILED(data->GetData(&ft, &st))) continue;
                std::wstring text;
                if (const wchar_t* p = (const wchar_t*)GlobalLock(st.hGlobal))
                {
                    SIZE_T cap = GlobalSize(st.hGlobal) / sizeof(wchar_t);
                    text.assign(p, wcsnlen(p, (std::min)((SIZE_T)70000, cap)));
                    GlobalUnlock(st.hGlobal);
                }
                ReleaseStgMedium(&st);
                std::wstring url = FirstLine(text);
                if (NdmIsNetOrMagnetUrl(url)) { m_items.push_back(url); return; }
            }
        }
        LONG m_ref = 1;
        NdmMainWindow* m_w;
        std::vector<std::wstring> m_items;
        bool m_files = false;
    };
}

// ===========================================================================
// Singleton / creation
// ===========================================================================
NdmMainWindow& NdmMainWindow::GetInstance()
{
    static NdmMainWindow inst;
    return inst;
}

HWND NdmMainWindow::Create(HINSTANCE hInst, bool startHidden)
{
    m_hInst       = hInst;
    m_startHidden = startHidden;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;            // fully painted, no erase flicker
    wc.lpszClassName = NDM_WINDOW_CLASS;
    wc.style         = CS_DBLCLKS;
    // Window icons are set per DPI in OnCreate / WM_DPICHANGED.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;

    m_hWnd = CreateWindowExW(0, NDM_WINDOW_CLASS, L"NDM — Nee Download Manager",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 500,
                             nullptr, nullptr, hInst, this);
    if (!m_hWnd) return nullptr;

    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (LoadPlacement(wp))
    {
        UINT show = wp.showCmd;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(m_hWnd, &wp);
        if (!m_startHidden)
            ShowWindow(m_hWnd, show == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }
    else
    {
        // Default: 960x500 client (96-dpi units) centred on the work area
        PlaceWindow(m_hWnd, 960, 500, nullptr);
        if (!m_startHidden) ShowWindow(m_hWnd, SW_SHOWNORMAL);
    }
    if (!m_startHidden)
    {
        UpdateWindow(m_hWnd);
        SetForegroundWindow(m_hWnd);
    }
    return m_hWnd;
}

// ===========================================================================
// Window procedure plumbing
// ===========================================================================
LRESULT CALLBACK NdmMainWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NdmMainWindow* self = nullptr;
    if (m == WM_NCCREATE)
    {
        self = reinterpret_cast<NdmMainWindow*>(((CREATESTRUCTW*)l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->m_hWnd = h;
    }
    else
    {
        self = reinterpret_cast<NdmMainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT NdmMainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    HWND hwnd = m_hWnd;

    if (msg == m_msgTaskbarCreated && m_msgTaskbarCreated)
    {
        m_trayAdded = false;           // explorer restarted
        AddTrayIcon();
        return 0;
    }

    // Setup / uninstall asks us to quit now (posted, no reply needed):
    // persist, stop the engines, close -- bypassing close-to-tray and the
    // "downloads still running" question.
    if (msg == m_msgExitNow && m_msgExitNow)
    {
        ExitNow();
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
        {
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT r = { 0, 0, S(936), S(340) };   // every column fits without a horizontal scrollbar
        AdjustRectForDpi(&r, WS_OVERLAPPEDWINDOW, 0, m_dpi);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED:
    {
        UINT old = m_dpi;
        m_dpi = HIWORD(wp);
        FreeFonts();
        CreateFonts();
        for (auto& kv : m_iconCache) if (kv.second) DestroyIcon(kv.second);
        m_iconCache.clear();
        (void)old;
        // column widths follow the new DPI (the user's widths are kept at 96 dpi)
        for (int c = 0; c < COL_URL; ++c)
            ListView_SetColumnWidth(m_hList, c, S(m_colW96[c]));
        ApplyListMetrics();
        SetWindowIcons(hwnd, m_dpi);
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                     nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        DarkTitleBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (m_hList) InvalidateRect(m_hList, nullptr, FALSE);   // dates follow the locale
        break;

    case WM_TIMECHANGE:
        if (m_hList) InvalidateRect(m_hList, nullptr, FALSE);   // time zone / clock
        break;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) NdmQueue::GetInstance().OnSystemResumed();
        return TRUE;

    case WM_CLIPBOARDUPDATE:
        m_clipRetries = 0;
        OnClipboardUpdate();
        return 0;

    case WM_NDM_DONE_ACTION:
        NdmPower::StartCountdown(hwnd, (int)wp, (DWORD)lp);
        return 0;

    case WM_NDM_DROP:
        OnDrop();
        return 0;

    case WM_CTLCOLOREDIT:
        if ((HWND)lp == m_hSearch)
        {
            SetTextColor((HDC)wp, NDM_CLR_FG);
            SetBkColor((HDC)wp, NDM_CLR_EDIT_BG);
            return (LRESULT)m_brEdit;
        }
        break;

    case WM_TIMER:
        if (wp == NDM_TIMER_ID)
        {
            Refresh(false);
        }
        else if (wp == NDM_CLIP_TIMER)
        {
            KillTimer(hwnd, NDM_CLIP_TIMER);
            OnClipboardUpdate();
        }
        else if (wp == NDM_ANIM_TIMER)
        {
            m_animPhase = (m_animPhase + 3) % 1200;
            bool any = false;
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
            {
                int top = ListView_GetTopIndex(m_hList);
                int cnt = ListView_GetCountPerPage(m_hList) + 1;
                for (int i = top; i < top + cnt && i < (int)m_view.size(); ++i)
                {
                    const Row& r = m_all[m_view[i]];
                    if (r.status == NDM_STATUS_DOWNLOADING && r.total == 0)
                    {
                        any = true;
                        RECT rc;
                        if (ListView_GetSubItemRect(m_hList, i, COL_PROGRESS, LVIR_BOUNDS, &rc))
                            InvalidateRect(m_hList, &rc, FALSE);
                    }
                }
            }
            if (!any) KillTimer(hwnd, NDM_ANIM_TIMER);
        }
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (m_hotBtn >= 0 || m_hotCat >= 0 || m_sideTrackHot)
        {
            m_hotBtn = -1; m_hotCat = -1; m_sideTrackHot = false;
            InvalidateRect(hwnd, &m_rcToolbar, FALSE);
            InvalidateRect(hwnd, &m_rcSidebar, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        OnLButtonDown({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_CAPTURECHANGED:
        if (m_pressBtn >= 0)
        {
            m_pressBtn = -1;
            InvalidateRect(hwnd, &m_rcToolbar, FALSE);
        }
        if (m_sideDrag)
        {
            m_sideDrag = false;
            InvalidateRect(hwnd, &m_rcSidebar, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        // Sidebar under the pointer: scroll it (the list scrolls itself).
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&m_rcSidebar, pt))
        {
            m_wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
            int notches = m_wheelAccum / WHEEL_DELTA;
            m_wheelAccum -= notches * WHEEL_DELTA;
            if (notches) ScrollSidebar(m_sideScroll - notches * 2 * S(kCatH + 1));
            return 0;
        }
        break;
    }

    case WM_ENTERMENULOOP:
        m_menuOpen = true;
        break;

    case WM_EXITMENULOOP:
        m_menuOpen = false;
        break;

    case WM_NDM_COLWIDTH:
    {
        // The user resized a column: keep it (96 dpi) and save. The Address
        // column always fills the rest, so its own width is never kept.
        int c = (int)wp;
        if (m_hList && c >= 0 && c < COL_URL)
        {
            int w96 = MulDiv(ListView_GetColumnWidth(m_hList, c), 96, (int)m_dpi);
            m_colW96[c] = (std::max)(24, (std::min)(w96, 2000));
        }
        FitLastColumn();
        SaveUiState();
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd)
        {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            int b = HitToolbar(pt);
            if ((b >= 0 && m_btns[b].enabled) || HitSidebar(pt) >= 0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_SETFOCUS:
        if (m_hList) SetFocus(m_hList);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SEARCH && (HWND)lp == m_hSearch)
        {
            if (HIWORD(wp) == EN_CHANGE) OnSearchChanged();
            else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)
                InvalidateRect(hwnd, &m_rcSearch, FALSE);
            return 0;
        }
        OnCommand(LOWORD(wp));
        return 0;

    case WM_CONTEXTMENU:
        if ((HWND)wp == m_hList)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            OnContextMenu(pt);
            return 0;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR* nh = (NMHDR*)lp;
        if (m_hHdr && nh->hwndFrom == m_hHdr)
        {
            // forwarded by the list view: remember widths the user sets
            switch (nh->code)
            {
            case HDN_BEGINTRACKW:
            case HDN_BEGINTRACKA:
                m_colTracking = true;
                break;
            case HDN_ENDTRACKW:
            case HDN_ENDTRACKA:
                m_colTracking = false;
                PostMessageW(hwnd, WM_NDM_COLWIDTH, (WPARAM)((NMHEADERW*)lp)->iItem, 0);
                break;
            case HDN_DIVIDERDBLCLICKW:
            case HDN_DIVIDERDBLCLICKA:
                PostMessageW(hwnd, WM_NDM_COLWIDTH, (WPARAM)((NMHEADERW*)lp)->iItem, 0);
                break;
            }
            break;
        }
        if (nh->hwndFrom == m_hList)
        {
            switch (nh->code)
            {
            case NM_CUSTOMDRAW:
                return OnListCustomDraw((NMLVCUSTOMDRAW*)lp);
            case LVN_GETDISPINFOW:
            {
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
                int i = di->item.iItem;
                if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0)
                {
                    di->item.pszText[0] = 0;
                    if (i >= 0 && i < (int)m_view.size())
                    {
                        // the text of each cell (label tips, divider double-click
                        // auto-size, screen readers); rows are painted by DrawRow
                        const Row& r = m_all[m_view[i]];
                        std::wstring s;
                        switch (di->item.iSubItem)
                        {
                        case COL_DATE:   s = NdmUi::FormatListDate(r.dateFt); break;
                        case COL_SIZE:   if (r.total) s = NdmFormatSize(r.total); break;
                        case COL_STATUS: s = StatusText(r.status); break;
                        case COL_PROGRESS:
                            if (r.status == NDM_STATUS_COMPLETED) s = L"100%";
                            else if (r.total) s = Fmt(L"%.1f%%", (double)r.done * 100.0 / (double)r.total);
                            break;
                        case COL_SPEED:
                            if (r.status == NDM_STATUS_DOWNLOADING) s = NdmFormatSpeed(r.speed);
                            break;
                        case COL_TIMELEFT:
                            if (r.status == NDM_STATUS_DOWNLOADING && r.speed > 0 && r.total > r.done)
                                s = NdmFormatTime((DWORD)((r.total - r.done) / r.speed) + 1);
                            else if (r.status == NDM_STATUS_COMPLETED)
                                s = L"Done";
                            break;
                        case COL_URL:    s = r.url; break;
                        default:         s = r.name; break;
                        }
                        wcsncpy_s(di->item.pszText, di->item.cchTextMax, s.c_str(), _TRUNCATE);
                    }
                }
                return 0;
            }
            case LVN_ODFINDITEMW:
                return -1;
            case LVN_ITEMCHANGED:
            case LVN_ODSTATECHANGED:
                if (!m_updating) UpdateToolbarState();
                return 0;
            case LVN_COLUMNCLICK:
                OnColumnClick(((NMLISTVIEW*)lp)->iSubItem);
                return 0;
            case NM_DBLCLK:
                if (((NMITEMACTIVATE*)lp)->iItem >= 0) CmdDefault();
                return 0;
            case NM_RETURN:
                CmdDefault();
                return 0;
            case LVN_KEYDOWN:
            {
                NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lp;
                if (kd->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
                    OnCommand(ID_CMD_SELECTALL);
                return 0;
            }
            }
        }
        break;
    }

    case WM_COPYDATA:
    {
        COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lp;
        if (!cds) return FALSE;
        if (cds->dwData == NDM_COPYDATA_SHOW)
        {
            PostMessageW(hwnd, WM_NDM_SHOW_MAIN, 0, 0);
            return TRUE;
        }
        if ((cds->dwData == WM_NDM_ADD_DOWNLOAD || cds->dwData == NDM_COPYDATA_CMDLINE) &&
            cds->lpData && cds->cbData >= sizeof(wchar_t) && cds->cbData <= 160 * 1024)
        {
            const WPARAM origin = cds->dwData == NDM_COPYDATA_CMDLINE ? NDM_ORIGIN_CMDLINE
                                                                     : NDM_ORIGIN_EXTERNAL;
            // Copy now: the sender's buffer is only valid during SendMessage.
            size_t n = cds->cbData / sizeof(wchar_t);
            const wchar_t* p = (const wchar_t*)cds->lpData;
            std::wstring url(p, wcsnlen(p, n));
            size_t a = url.find_first_not_of(L" \t\r\n\"");
            size_t b = url.find_last_not_of(L" \t\r\n\"");
            if (a != std::wstring::npos)
            {
                url = url.substr(a, b - a + 1);
                if (!AcceptableSource(url, (int)origin)) return FALSE;
                // Queue + post a token so the sender (shell extension) is not
                // blocked by our UI. No pointer travels in the message.
                NdmBrowserRequest* req = new NdmBrowserRequest();
                req->url = url;
                req->origin = origin == NDM_ORIGIN_CMDLINE ? NDM_ORIGIN_CMDLINE : NDM_ORIGIN_EXTERNAL;
                return NdmPostBrowserRequest(hwnd, req) ? TRUE : FALSE;
            }
        }
        return FALSE;
    }

    case WM_NDM_URL_REQUEST:
        return 0;                       // retired (carried a pointer); ignore

    case WM_NDM_BROWSER_MSG:
        // LPARAM = token of a request queued by NdmPostBrowserRequest (pipe
        // thread, command line, WM_COPYDATA). Anything else -- e.g. a value
        // another process posted -- is not a known token and is ignored.
        if (wp == 0)
            if (NdmBrowserRequest* req = NdmTakeBrowserRequest(lp))
                HandleBrowserRequest(req);       // takes ownership
        return 0;

    case WM_NDM_SHOW_MAIN:
        ShowMain();
        return 0;

    case WM_NDM_TRAY_ICON:
        switch (LOWORD(lp))
        {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd)
                HideToTray();
            else
                ShowMain();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        case NIN_BALLOONUSERCLICK:
        {
            // finished: show the file in its folder; failed: open that
            // download's window (there is no file to show yet)
            const Row* r = RowById(m_balloonId);
            if (r && m_balloonKind == BALLOON_COMPLETE)
                OpenFolderSelect(r->savePath, r->name);
            else if (r && m_balloonKind == BALLOON_FAILED)
                ShowExisting(r->id);
            else
                ShowMain();
            break;
        }
        }
        return 0;

    case WM_CLOSE:
        if (!m_exiting && NdmConfigMgr::GetInstance().GetCloseToTray())
            HideToTray();
        else
            ExitApp();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp)
        {
            SavePlacement();
            NdmQueue::GetInstance().SaveQueue(
                NdmConfigMgr::GetInstance().GetAppDataPath() + L"queue.xml");
            NdmConfigMgr::GetInstance().Save();
            NdmTorrentShutdown();                     // aria2: saveSession + graceful stop
            NdmQueue::GetInstance().Shutdown(3000);   // stop workers cleanly; saves again
        }
        return 0;

    case WM_DESTROY:
    {
        NdmMsgHost::SetMainWindow(nullptr);
        RemoveClipboardFormatListener(hwnd);
        if (m_dropTarget)
        {
            RevokeDragDrop(hwnd);
            if (m_hList) RevokeDragDrop(m_hList);
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        SavePlacement();
        SaveUiState();
        KillTimer(hwnd, NDM_TIMER_ID);
        KillTimer(hwnd, NDM_ANIM_TIMER);
        KillTimer(hwnd, NDM_CLIP_TIMER);
        NdmTheme::CloseAllModeless();
        RemoveTrayIcon();
        NdmQueue::GetInstance().SaveQueue(
            NdmConfigMgr::GetInstance().GetAppDataPath() + L"queue.xml");

        // Free requests that were queued but never handled.
        MSG pm;
        while (PeekMessageW(&pm, hwnd, WM_NDM_BROWSER_MSG, WM_NDM_BROWSER_MSG, PM_REMOVE)) {}
        NdmDiscardBrowserRequests();

        if (m_hTip)  { DestroyWindow(m_hTip); m_hTip = nullptr; }
        if (m_hAccel){ DestroyAcceleratorTable(m_hAccel); m_hAccel = nullptr; }
        for (auto& kv : m_iconCache) if (kv.second) DestroyIcon(kv.second);
        m_iconCache.clear();
        if (m_hIconBig) { DestroyIcon(m_hIconBig); m_hIconBig = nullptr; }
        if (m_hIconSm)  { DestroyIcon(m_hIconSm);  m_hIconSm  = nullptr; }
        FreeFonts();
        if (m_brEdit) { DeleteObject(m_brEdit); m_brEdit = nullptr; }
        PostQuitMessage(0);
        return 0;
    }

    case WM_NCDESTROY:
        if (m_hRowIml) { ImageList_Destroy(m_hRowIml); m_hRowIml = nullptr; }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hWnd = nullptr;
        m_hList = nullptr;
        m_hHdr = nullptr;
        m_hSearch = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Setup
// ===========================================================================
void NdmMainWindow::OnCreate()
{
    m_dpi = GetDpi(m_hWnd);
    m_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    m_msgExitNow        = RegisterWindowMessageW(L"NDM_ExitNow_v1");   // posted by setup / uninstall
    DarkTitleBar(m_hWnd);

    // NDM is never elevated: no UIPI message-filter exceptions (WM_COPYDATA
    // from lower-integrity processes stays blocked by Windows).

    // Title bar / taskbar icons at this window's DPI; tray + balloon icons at
    // the shell's (system) DPI. Exact-size frames from ndm.ico, never stretched.
    SetWindowIcons(m_hWnd, m_dpi);
    LoadIconMetric(m_hInst, MAKEINTRESOURCEW(IDI_NDM), LIM_SMALL, &m_hIconSm);
    LoadIconMetric(m_hInst, MAKEINTRESOURCEW(IDI_NDM), LIM_LARGE, &m_hIconBig);

    CreateFonts();

    m_btns = {
        { ID_CMD_ADD,        GLY_ADD,     L"Add URL",  L"Add a new download (Ctrl+N)" },
        { ID_CMD_RESUME,     GLY_RESUME,  L"Resume",   L"Resume the selected downloads" },
        { ID_CMD_PAUSE,      GLY_PAUSE,   L"Pause",    L"Pause the selected downloads" },
        { ID_CMD_STOPALL,    GLY_STOPALL, L"Stop All", L"Pause every active download" },
        { ID_CMD_DELETE,     GLY_DELETE,  L"Delete",   L"Remove the selected downloads from the list (Del)" },
        { ID_CMD_OPENFOLDER, GLY_FOLDER,  L"Folder",   L"Show the file in its folder (Ctrl+E)" },
        { ID_CMD_SCHEDULER,  GLY_CLOCK,   L"Scheduler", L"Start and stop downloads at set times" },
        { ID_CMD_OPTIONS,    GLY_OPTIONS, L"Options",  L"NDM options (Ctrl+O)" },
    };
    m_btns[0].groupEnd = true;
    m_btns[3].groupEnd = true;
    m_btns[5].groupEnd = true;

    // Column widths and sort order from ndm.ini [UI] (defaults otherwise)
    {
        const NdmUiState& ui = NdmConfigMgr::GetInstance().Ui();
        for (int c = 0; c < COL_COUNT; ++c) m_colW96[c] = kColWidth96[c];
        NdmUi::ParseColumnWidths(ui.columnWidths, m_colW96, COL_COUNT, 24, 2000);
        m_sortCol = NdmUi::ClampSortColumn(ui.sortColumn, COL_COUNT);
        m_sortAsc = ui.sortAscending;
    }

    CreateList();

    // Search box (right side of the toolbar)
    m_brEdit  = CreateSolidBrush(NDM_CLR_EDIT_BG);
    m_hSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 10, 10, m_hWnd, (HMENU)(INT_PTR)IDC_SEARCH, m_hInst, nullptr);
    if (m_hSearch)
    {
        SendMessageW(m_hSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(m_hSearch, EM_LIMITTEXT, 200, 0);
        SendMessageW(m_hSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
        SendMessageW(m_hSearch, WM_SETFONT, (WPARAM)m_fUI, FALSE);
        SetWindowSubclass(m_hSearch, SearchSubclass, 3, (DWORD_PTR)this);
    }

    // Tooltips for the painted toolbar
    m_hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                             WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             0, 0, 0, 0, m_hWnd, nullptr, m_hInst, nullptr);
    if (m_hTip)
    {
        DarkTooltip(m_hTip);
        for (size_t i = 0; i < m_btns.size(); ++i)
        {
            TOOLINFOW ti = { sizeof(ti) };
            ti.uFlags   = TTF_SUBCLASS;
            ti.hwnd     = m_hWnd;
            ti.uId      = i + 1;
            ti.lpszText = const_cast<wchar_t*>(m_btns[i].tip);
            SendMessageW(m_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        SendMessageW(m_hTip, TTM_SETMAXTIPWIDTH, 0, 420);
    }

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N',       ID_CMD_ADD },
        { FVIRTKEY,            VK_DELETE, ID_CMD_DELETE },
        { FVIRTKEY | FCONTROL, 'O',       ID_CMD_OPTIONS },
        { FVIRTKEY | FCONTROL, 'E',       ID_CMD_OPENFOLDER },
        { FVIRTKEY | FCONTROL, 'C',       ID_CMD_COPYURL },
        { FVIRTKEY | FALT,     VK_RETURN, ID_CMD_PROPERTIES },
        { FVIRTKEY,            VK_F2,     ID_CMD_RENAME },
        { FVIRTKEY | FSHIFT,   VK_DELETE, ID_CMD_DELETEFILE },
        { FVIRTKEY | FCONTROL, 'F',       ID_CMD_FIND },
    };
    m_hAccel = CreateAcceleratorTableW(acc, _countof(acc));

    // Clipboard watcher (Options > General) and drag and drop
    AddClipboardFormatListener(m_hWnd);
    DropTarget* dt = new DropTarget(this);
    if (SUCCEEDED(RegisterDragDrop(m_hWnd, dt)))
    {
        if (m_hList) RegisterDragDrop(m_hList, dt);
        m_dropTarget = dt;
    }
    else
        dt->Release();

    AddTrayIcon();
    Layout();
    Refresh(true);
    SetTimer(m_hWnd, NDM_TIMER_ID, NDM_TIMER_MS, nullptr);
}

void NdmMainWindow::CreateFonts()
{
    m_fUI      = MakeFont(12, FW_NORMAL,   m_dpi);
    m_fSmall   = MakeFont(11, FW_NORMAL,   m_dpi);
    m_fBold    = MakeFont(12, FW_SEMIBOLD, m_dpi);
    m_fTitle   = MakeFont(14, FW_SEMIBOLD, m_dpi);
    m_fCaption = MakeFont(10, FW_SEMIBOLD, m_dpi);
    m_fSpeed   = MakeFont(13, FW_SEMIBOLD, m_dpi);
    m_fTool    = MakeFont(11, FW_NORMAL,   m_dpi);
    m_fHdr     = MakeFont(11, FW_SEMIBOLD, m_dpi);
    if (m_hList) SendMessageW(m_hList, WM_SETFONT, (WPARAM)m_fUI, FALSE);
    if (m_hTip)  SendMessageW(m_hTip,  WM_SETFONT, (WPARAM)m_fUI, FALSE);
    if (m_hSearch) SendMessageW(m_hSearch, WM_SETFONT, (WPARAM)m_fUI, FALSE);
}

void NdmMainWindow::FreeFonts()
{
    HFONT* fs[] = { &m_fUI, &m_fSmall, &m_fBold, &m_fTitle, &m_fCaption,
                    &m_fSpeed, &m_fTool, &m_fHdr };
    for (HFONT* f : fs) if (*f) { DeleteObject(*f); *f = nullptr; }
}

void NdmMainWindow::CreateList()
{
    m_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                              LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                              LVS_SHAREIMAGELISTS,
                              0, 0, 100, 100, m_hWnd, (HMENU)(INT_PTR)IDC_LISTVIEW,
                              m_hInst, nullptr);
    if (!m_hList) return;
    ListView_SetExtendedListViewStyle(m_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    DarkScrollbars(m_hList);
    ListView_SetBkColor(m_hList, NDM_CLR_BG);
    ListView_SetTextBkColor(m_hList, NDM_CLR_BG);
    ListView_SetTextColor(m_hList, NDM_CLR_FG);
    SendMessageW(m_hList, WM_SETFONT, (WPARAM)m_fUI, FALSE);

    for (int c = 0; c < COL_COUNT; ++c)
    {
        LVCOLUMNW col = {};
        col.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt     = c == COL_SIZE ? LVCFMT_RIGHT : LVCFMT_LEFT;     // sizes line up (IDM style)
        col.cx      = S(m_colW96[c]);
        col.pszText = const_cast<wchar_t*>(kColName[c]);
        col.iSubItem = c;
        ListView_InsertColumn(m_hList, c, &col);
    }

    m_hHdr = ListView_GetHeader(m_hList);
    SetWindowSubclass(m_hList, ListSubclass, 1, (DWORD_PTR)this);
    if (m_hHdr) SetWindowSubclass(m_hHdr, HeaderSubclass, 2, (DWORD_PTR)this);
    ApplyListMetrics();
}

void NdmMainWindow::ApplyListMetrics()
{
    if (!m_hList) return;
    SendMessageW(m_hList, WM_SETFONT, (WPARAM)m_fUI, FALSE);
    // Row height is driven by the small image list height.
    HIMAGELIST iml = ImageList_Create(1, S(kRowH), ILC_COLOR32, 1, 1);
    HIMAGELIST old = ListView_SetImageList(m_hList, iml, LVSIL_SMALL);
    if (old && old != iml) ImageList_Destroy(old);
    m_hRowIml = iml;
    if (m_hHdr)
    {
        // force HDM_LAYOUT so our header height applies
        SetWindowPos(m_hList, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void NdmMainWindow::Layout()
{
    if (!m_hWnd) return;
    RECT rc; GetClientRect(m_hWnd, &rc);
    int cw = rc.right, ch = rc.bottom;

    m_rcHeader  = { 0, 0, 0, 0 };              // brand + speed live in the toolbar
    m_rcToolbar = { 0, 0, cw, S(kToolbarH) };
    m_rcStatus  = { 0, (std::max)((int)m_rcToolbar.bottom, ch - S(kStatusH)), cw, ch };
    m_rcSidebar = { 0, m_rcToolbar.bottom, S(kSidebarW), m_rcStatus.top };
    m_rcList    = { m_rcSidebar.right + 1, m_rcToolbar.bottom, cw, m_rcStatus.top };

    int x = S(10);
    int y = m_rcToolbar.top + (S(kToolbarH) - S(kBtnH)) / 2;
    for (size_t i = 0; i < m_btns.size(); ++i)
    {
        m_btns[i].rc = { x, y, x + S(kBtnW), y + S(kBtnH) };
        x += S(kBtnW) + S(2);
        if (m_btns[i].groupEnd) x += S(10);
        if (m_hTip)
        {
            TOOLINFOW ti = { sizeof(ti) };
            ti.hwnd = m_hWnd; ti.uId = i + 1; ti.rect = m_btns[i].rc;
            SendMessageW(m_hTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }

    // Right side of the toolbar: [search] [speed sparkline + rate]. The
    // speed readout goes first when space is short, then the search box.
    {
        int btnRight = m_btns.empty() ? 0 : (int)m_btns.back().rc.right;
        int right = cw - S(12), minLeft = btnRight + S(16);
        int sw = S(180), spW = S(184);
        if (right - minLeft >= sw + S(14) + spW)
        {
            m_rcSpeed = { right - spW, m_rcToolbar.top, right, m_rcToolbar.bottom - 1 };
            right = m_rcSpeed.left - S(14);
        }
        else
            m_rcSpeed = { 0, 0, 0, 0 };
        int w = (std::min)(sw, right - minLeft);
        int cy = (m_rcToolbar.top + m_rcToolbar.bottom) / 2;
        if (m_hSearch)
        {
            if (w >= S(96))
            {
                m_rcSearch = { right - w, cy - S(13), right, cy + S(13) };
                RECT inner = m_rcSearch;
                inner.left += S(22);
                PlaceEdit(m_hSearch, inner, m_fUI, S(4));
                ShowWindow(m_hSearch, SW_SHOWNA);
            }
            else
            {
                m_rcSearch = { 0, 0, 0, 0 };
                ShowWindow(m_hSearch, SW_HIDE);
            }
        }
    }

    LayoutSidebar();

    if (m_hList)
        MoveWindow(m_hList, m_rcList.left, m_rcList.top,
                   (std::max)(0, (int)(m_rcList.right - m_rcList.left)),
                   (std::max)(0, (int)(m_rcList.bottom - m_rcList.top)), TRUE);
    FitLastColumn();
}

// The Address column absorbs the remaining width so no column is cut off
// and no horizontal scrollbar appears at the default size.
void NdmMainWindow::FitLastColumn()
{
    if (m_colTracking && !(GetKeyState(VK_LBUTTON) & 0x8000)) m_colTracking = false;   // drag ended unseen
    if (!m_hList || m_colTracking) return;          // never fight a drag in progress
    RECT lc; GetClientRect(m_hList, &lc);
    int used = 0;
    for (int c = 0; c < COL_URL; ++c)
        used += c == COL_FILENAME ? S(m_colW96[c]) : ListView_GetColumnWidth(m_hList, c);
    // Narrow list: give up File Name width (down to a minimum) before the
    // Address column would push the list into a horizontal scrollbar. The
    // user's own File Name width comes back when there is room again.
    int nameW = S(m_colW96[COL_FILENAME]);
    int short_ = (int)S(50) - (int)(lc.right - used);
    if (short_ > 0 && nameW > S(130))
    {
        int take = (std::min)(short_, nameW - (int)S(130));
        nameW -= take;
        used -= take;
    }
    if (ListView_GetColumnWidth(m_hList, COL_FILENAME) != nameW)
        ListView_SetColumnWidth(m_hList, COL_FILENAME, nameW);
    int w = (std::max)((int)S(50), (int)(lc.right - used));
    if (ListView_GetColumnWidth(m_hList, COL_URL) != w)
        ListView_SetColumnWidth(m_hList, COL_URL, w);
}

// Sidebar rows at the current scroll offset; the slim scrollbar appears
// only when the window is too short for every category.
void NdmMainWindow::LayoutSidebar()
{
    int top = m_rcSidebar.top;
    int sy = top + S(8) + S(kCatCapH);                 // after "LIBRARY" caption
    RECT base[CAT_COUNT];
    for (int c = 0; c < CAT_COUNT; ++c)
    {
        if (c == CAT_UNFINISHED) sy += S(8) + S(kCatCapH); // gap + "STATUS" caption
        base[c] = { m_rcSidebar.left + S(6), sy, m_rcSidebar.right - S(6), sy + S(kCatH) };
        sy += S(kCatH) + S(1);
    }
    m_sideContentH = sy + S(8) - top;
    int view = m_rcSidebar.bottom - m_rcSidebar.top;
    m_sideScroll = NdmUi::ClampScroll(m_sideScroll, m_sideContentH, view);
    for (int c = 0; c < CAT_COUNT; ++c)
    {
        m_catRc[c] = base[c];
        OffsetRect(&m_catRc[c], 0, -m_sideScroll);
    }
    m_rcSideTrack = m_rcSideThumb = RECT{ 0, 0, 0, 0 };
    if (m_sideContentH > view && view > S(24))
    {
        m_rcSideTrack = { m_rcSidebar.right - S(6), m_rcSidebar.top + S(3),
                          m_rcSidebar.right - S(1), m_rcSidebar.bottom - S(3) };
        int tt = 0, tl = 0;
        NdmUi::ScrollThumb(m_sideScroll, m_sideContentH, view,
                           m_rcSideTrack.bottom - m_rcSideTrack.top, S(18), tt, tl);
        m_rcSideThumb = { m_rcSideTrack.left, m_rcSideTrack.top + tt,
                          m_rcSideTrack.right, m_rcSideTrack.top + tt + tl };
    }
}

void NdmMainWindow::ScrollSidebar(int pos)
{
    int view = m_rcSidebar.bottom - m_rcSidebar.top;
    pos = NdmUi::ClampScroll(pos, m_sideContentH, view);
    if (pos == m_sideScroll) return;
    m_sideScroll = pos;
    LayoutSidebar();
    POINT pt; GetCursorPos(&pt); ScreenToClient(m_hWnd, &pt);
    m_hotCat = HitSidebar(pt);                      // the row under the pointer changed
    InvalidateRect(m_hWnd, &m_rcSidebar, FALSE);
}

bool NdmMainWindow::PointerOverList() const
{
    if (!m_hList || !IsWindowVisible(m_hList)) return false;
    POINT pt; GetCursorPos(&pt);
    HWND w = WindowFromPoint(pt);
    return w && (w == m_hList || IsChild(m_hList, w));
}

void NdmMainWindow::SaveUiState()
{
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    NdmUiState& ui = cfg.Ui();
    ui.columnWidths  = NdmUi::FormatColumnWidths(m_colW96, COL_COUNT);
    ui.sortColumn    = m_sortCol;
    ui.sortAscending = m_sortAsc;
    cfg.SaveUi();
}

void NdmMainWindow::UpdateTooltips() { Layout(); }

// ===========================================================================
// Painting
// ===========================================================================
void NdmMainWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hWnd, &ps);
    {
        NdmMemDC mem(hdc, ps.rcPaint);
        HDC dc = mem.ok() ? mem.dc : hdc;
        RECT tmp;
        if (IntersectRect(&tmp, &ps.rcPaint, &m_rcToolbar)) PaintToolbar(dc, m_rcToolbar);
        if (IntersectRect(&tmp, &ps.rcPaint, &m_rcSidebar)) PaintSidebar(dc, m_rcSidebar);
        if (IntersectRect(&tmp, &ps.rcPaint, &m_rcStatus))  PaintStatus(dc, m_rcStatus);
        // 1px divider between sidebar and list
        VLine(dc, m_rcSidebar.right, m_rcSidebar.top, m_rcSidebar.bottom, NDM_CLR_BORDER);
    }
    EndPaint(m_hWnd, &ps);
}

// Live total speed at the right end of the toolbar: tiny sparkline + rate
void NdmMainWindow::PaintSpeed(HDC dc, const RECT& sp)
{
    if (m_btns.empty() || sp.right <= sp.left || sp.left < m_btns.back().rc.right + S(16)) return;   // too narrow
    std::wstring spd = NdmFormatSpeed(m_totalSpeed);
    SIZE ts = MeasureText(dc, spd, m_fSpeed);
    RECT tr = { sp.right - ts.cx, sp.top, sp.right, sp.bottom };
    Text(dc, spd, tr, m_totalSpeed ? NDM_CLR_GOLD_LIGHT : NDM_CLR_FG_DIM, m_fSpeed,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int gs = S(12);
    RECT ar = { tr.left - S(4) - gs, (sp.top + sp.bottom - gs) / 2, tr.left - S(4), 0 };
    ar.bottom = ar.top + gs;
    Glyph(dc, GLY_DOWNARROW, ar, m_totalSpeed ? NDM_CLR_GREEN : NDM_CLR_FG_DIM, 1.2f);
    RECT spark = { ar.left - S(10) - S(72), sp.top + S(14), ar.left - S(10), sp.bottom - S(14) };
    if (spark.left > sp.left)
        Sparkline(dc, spark, m_speedHist.empty() ? nullptr : m_speedHist.data(), (int)m_speedHist.size());
}

void NdmMainWindow::PaintToolbar(HDC dc, const RECT& rc)
{
    GradientV(dc, rc, NDM_CLR_TB_TOP, NDM_CLR_TB_BOT);
    HLine(dc, rc.left, rc.right, rc.bottom - 1, NDM_CLR_BORDER);

    for (size_t i = 0; i < m_btns.size(); ++i)
    {
        const ToolBtn& b = m_btns[i];
        bool hot = (int)i == m_hotBtn && b.enabled;
        bool prs = (int)i == m_pressBtn && hot;
        int  rad = S(6);
        if (prs)
        {
            RoundFill(dc, b.rc, rad, NDM_CLR_BG_SEL);
            RoundFrame(dc, b.rc, rad, NDM_CLR_BORDER_GOLD, 1.0f);
        }
        else if (hot)
        {
            RoundFillV(dc, b.rc, rad, RGB(46, 50, 60), RGB(36, 40, 48));
            RoundFrame(dc, b.rc, rad, RGB(70, 72, 80), 1.0f);
        }

        int gs = S(20);
        int gx = b.rc.left + (b.rc.right - b.rc.left - gs) / 2;
        int gy = b.rc.top + S(4) + (prs ? 1 : 0);
        RECT gr = { gx, gy, gx + gs, gy + gs };
        COLORREF gc = !b.enabled ? NDM_CLR_DISABLED : (hot ? NDM_CLR_GOLD_LIGHT : NDM_CLR_GOLD);
        if (b.cmd == ID_CMD_ADD)
        {
            // primary action: a plain, slightly bolder plus in the toolbar's
            // line-icon style (no disc), so it reads as "add", not a screw head
            Glyph(dc, GLY_ADD, gr, gc, 1.45f);
        }
        else
        {
            Glyph(dc, (NdmGlyph)b.glyph, gr, gc, 1.0f);
        }
        RECT lr = { b.rc.left - S(4), gr.bottom + S(2), b.rc.right + S(4), b.rc.bottom };
        OffsetRect(&lr, 0, prs ? 1 : 0);
        Text(dc, b.label, lr, !b.enabled ? NDM_CLR_DISABLED : (hot ? RGB(245, 245, 248) : NDM_CLR_FG),
             m_fTool, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (b.groupEnd && i + 1 < m_btns.size())
        {
            int sx = b.rc.right + S(1) + S(5);
            VLine(dc, sx, rc.top + S(12), rc.bottom - S(12), NDM_CLR_BORDER);
        }
    }

    if (m_rcSearch.right > m_rcSearch.left)
    {
        DrawField(dc, m_rcSearch, GetFocus() == m_hSearch, false, m_dpi);
        int gs = S(14);
        RECT gr = { m_rcSearch.left + S(6), (m_rcSearch.top + m_rcSearch.bottom - gs) / 2, 0, 0 };
        gr.right = gr.left + gs; gr.bottom = gr.top + gs;
        Glyph(dc, GLY_SEARCH, gr, m_query.empty() ? NDM_CLR_FG_DIM : NDM_CLR_GOLD, 1.0f);
    }
    PaintSpeed(dc, m_rcSpeed);
}

void NdmMainWindow::PaintSidebar(HDC dc, const RECT& rc)
{
    Fill(dc, rc, NDM_CLR_BG_PANEL);
    // Rows scroll inside the sidebar (short window): clip to it.
    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    int oldEx = SetTextCharacterExtra(dc, S(1));
    RECT cap1 = { rc.left + S(14), rc.top + S(8) - m_sideScroll, rc.right,
                  rc.top + S(8) + S(kCatCapH) - m_sideScroll };
    Text(dc, L"LIBRARY", cap1, NDM_CLR_FG_DIM, m_fCaption, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT cap2 = { rc.left + S(14), m_catRc[CAT_UNFINISHED].top - S(kCatCapH), rc.right,
                  m_catRc[CAT_UNFINISHED].top };
    Text(dc, L"STATUS", cap2, NDM_CLR_FG_DIM, m_fCaption, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextCharacterExtra(dc, oldEx);

    for (int c = 0; c < CAT_COUNT; ++c)
    {
        RECT r = m_catRc[c];
        if (r.bottom <= rc.top || r.top >= rc.bottom) continue;   // scrolled out of view
        bool sel = (c == m_filter);
        bool hot = (c == m_hotCat);
        if (sel)
        {
            RoundFillV(dc, r, S(5), RGB(56, 50, 20), kSideSel);
            RECT bar = { r.left, r.top + S(6), r.left + S(3), r.bottom - S(6) };
            RoundFill(dc, bar, S(1), NDM_CLR_GOLD);
        }
        else if (hot)
        {
            RoundFill(dc, r, S(5), NDM_CLR_BG_HOVER);
        }
        int gs = S(16);
        RECT gr = { r.left + S(8), r.top + (r.bottom - r.top - gs) / 2, 0, 0 };
        gr.right = gr.left + gs; gr.bottom = gr.top + gs;
        Glyph(dc, CategoryGlyph(c), gr, sel ? NDM_CLR_GOLD_LIGHT : (hot ? NDM_CLR_GOLD : RGB(168, 150, 96)), 1.0f);

        std::wstring cnt = std::to_wstring(m_catCount[c]);
        SIZE cs = MeasureText(dc, cnt, m_fSmall);
        int pw = (std::max)((int)cs.cx + S(10), S(22)), ph = S(16);
        RECT pill = { r.right - S(6) - pw, r.top + (r.bottom - r.top - ph) / 2, r.right - S(6), 0 };
        pill.bottom = pill.top + ph;

        RECT tr = { gr.right + S(6), r.top, pill.left - S(4), r.bottom };
        Text(dc, CategoryName(c), tr, sel ? NDM_CLR_GOLD_LIGHT : NDM_CLR_FG,
             sel ? m_fBold : m_fUI, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RoundFill(dc, pill, ph / 2, sel ? RGB(92, 78, 26) : RGB(38, 42, 51));
        Text(dc, cnt, pill, sel ? NDM_CLR_GOLD_LIGHT : NDM_CLR_FG_DIM, m_fSmall,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RestoreDC(dc, saved);

    // slim scrollbar: only while some categories are out of view
    if (m_rcSideThumb.bottom > m_rcSideThumb.top)
    {
        int w = m_rcSideTrack.right - m_rcSideTrack.left;
        RoundFill(dc, m_rcSideTrack, w / 2, RGB(30, 33, 40));
        RoundFill(dc, m_rcSideThumb, w / 2, (m_sideDrag || m_sideTrackHot) ? NDM_CLR_GOLD : RGB(92, 84, 58));
    }
}

void NdmMainWindow::PaintStatus(HDC dc, const RECT& rc)
{
    Fill(dc, rc, kStatusBg);
    HLine(dc, rc.left, rc.right, rc.top, NDM_CLR_BORDER);

    int nDl = 0, nQ = 0, nP = 0, nDone = 0, nErr = 0;
    for (const Row& r : m_all)
    {
        switch (r.status)
        {
        case NDM_STATUS_DOWNLOADING: ++nDl; break;
        case NDM_STATUS_QUEUED:      ++nQ;  break;
        case NDM_STATUS_PAUSED:      ++nP;  break;
        case NDM_STATUS_COMPLETED:   ++nDone; break;
        case NDM_STATUS_ERROR:       ++nErr; break;
        }
    }
    int x = rc.left + S(12);
    int cy = (rc.top + rc.bottom) / 2;
    auto item = [&](COLORREF dot, const std::wstring& s)
    {
        Circle(dc, x + S(3), cy, S(3), dot);
        x += S(10);
        SIZE sz = MeasureText(dc, s, m_fSmall);
        RECT tr = { x, rc.top, x + sz.cx + 2, rc.bottom };
        Text(dc, s, tr, NDM_CLR_FG, m_fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += sz.cx + S(14);
    };
    item(nDl ? NDM_CLR_GREEN : NDM_CLR_DISABLED, Fmt(L"%d downloading", nDl));
    if (nQ) item(NDM_CLR_FG_DIM, Fmt(L"%d queued", nQ));
    if (nP) item(NDM_CLR_AMBER, Fmt(L"%d paused", nP));
    item(NDM_CLR_GOLD, Fmt(L"%d complete", nDone));
    if (nErr) item(NDM_CLR_RED, Fmt(L"%d failed", nErr));

    std::wstring right = !m_query.empty()
        ? Fmt(L"%d of %d item%s", (int)m_view.size(), (int)m_all.size(), m_all.size() == 1 ? L"" : L"s")
        : Fmt(L"%d item%s", (int)m_all.size(), m_all.size() == 1 ? L"" : L"s");
    RECT rr = { x, rc.top, rc.right - S(12), rc.bottom };
    Text(dc, right, rr, NDM_CLR_FG_DIM, m_fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------
// Column header (fully painted -- dark, sort arrow, gold underline)
// ---------------------------------------------------------------------------
void NdmMainWindow::PaintColumnHeader(HWND hHdr, HDC dc)
{
    RECT rc; GetClientRect(hHdr, &rc);
    Fill(dc, rc, kHdrBg);
    HLine(dc, rc.left, rc.right, rc.bottom - 1, NDM_CLR_BORDER);

    int n = Header_GetItemCount(hHdr);
    for (int i = 0; i < n; ++i)
    {
        RECT ir;
        if (!Header_GetItemRect(hHdr, i, &ir)) continue;
        wchar_t txt[64] = {};
        HDITEMW hi = {};
        hi.mask = HDI_TEXT; hi.pszText = txt; hi.cchTextMax = 64;
        Header_GetItem(hHdr, i, &hi);

        bool sorted = (i == m_sortCol);
        // Size is right-aligned like its cells; its sort arrow goes left of the text
        HDITEMW fi = {};
        fi.mask = HDI_FORMAT;
        Header_GetItem(hHdr, i, &fi);
        bool right = (fi.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT;
        RECT tr = ir;
        if (right) { tr.left += S(sorted ? 20 : 8); tr.right -= S(6); }
        else       { tr.left += S(8); tr.right -= S(sorted ? 20 : 6); }
        int oldEx = SetTextCharacterExtra(dc, 0);
        Text(dc, txt, tr, sorted ? NDM_CLR_GOLD_LIGHT : RGB(160, 162, 172), m_fHdr,
             (right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextCharacterExtra(dc, oldEx);
        if (sorted)
        {
            SIZE ts = MeasureText(dc, txt, m_fHdr);
            int ax = right ? (std::max)((int)(tr.right - ts.cx - S(16)), (int)(ir.left + S(6)))
                           : (std::min)((int)(tr.left + ts.cx + S(8)), (int)(ir.right - S(16)));
            int ay = (ir.top + ir.bottom) / 2;
            RECT g = { ax, ay - S(6), ax + S(12), ay + S(6) };
            (void)g;
            POINT p[3];
            if (m_sortAsc) { p[0] = { ax, ay + S(2) }; p[1] = { ax + S(4), ay - S(2) }; p[2] = { ax + S(8), ay + S(2) }; }
            else           { p[0] = { ax, ay - S(2) }; p[1] = { ax + S(4), ay + S(2) }; p[2] = { ax + S(8), ay - S(2) }; }
            HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(2) - 1), NDM_CLR_GOLD);
            HGDIOBJ op = SelectObject(dc, pen);
            Polyline(dc, p, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
            HLine(dc, ir.left + S(6), ir.right - S(6), ir.bottom - 2, NDM_CLR_GOLD);
            HLine(dc, ir.left + S(6), ir.right - S(6), ir.bottom - 1, NDM_CLR_GOLD_DARK);
        }
        VLine(dc, ir.right - 1, ir.top + S(8), ir.bottom - S(8), NDM_CLR_BORDER);
    }
}

// ---------------------------------------------------------------------------
// ListView custom draw
// ---------------------------------------------------------------------------
LRESULT NdmMainWindow::OnListCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
    {
        int i = (int)cd->nmcd.dwItemSpec;
        RECT rc;
        if (i >= 0 && i < (int)m_view.size() &&
            ListView_GetItemRect(m_hList, i, &rc, LVIR_BOUNDS))
        {
            DrawRow(cd->nmcd.hdc, i, rc);
        }
        return CDRF_SKIPDEFAULT;
    }
    case CDDS_POSTPAINT:
        if (m_view.empty()) DrawEmptyState(cd->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

void NdmMainWindow::DrawRow(HDC dc, int i, const RECT& rowRc)
{
    const Row& r = m_all[m_view[i]];
    bool sel = (ListView_GetItemState(m_hList, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    bool hot = (i == m_hotRow);

    RECT cl; GetClientRect(m_hList, &cl);
    RECT full = rowRc;
    full.left  = (std::min)(rowRc.left, cl.left);
    full.right = (std::max)(rowRc.right, cl.right);

    COLORREF bg = sel ? (hot ? kRowSelHot : kRowSel)
                : hot ? kRowHot : ((i & 1) ? kRowAlt : NDM_CLR_BG);
    Fill(dc, full, bg);
    HLine(dc, full.left, full.right, full.bottom - 1, sel ? RGB(86, 74, 26) : kRowLine);
    if (sel)
    {
        RECT bar = { cl.left, full.top, cl.left + S(3), full.bottom };
        Fill(dc, bar, NDM_CLR_GOLD);
    }

    int x = rowRc.left;
    int midY = (full.top + full.bottom) / 2;
    COLORREF fg  = sel ? RGB(246, 238, 210) : NDM_CLR_FG;
    COLORREF dim = sel ? RGB(190, 172, 120) : NDM_CLR_FG_DIM;
    const UINT one = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;

    for (int c = 0; c < COL_COUNT; ++c)
    {
        int w = ListView_GetColumnWidth(m_hList, c);
        RECT cell = { x, full.top, x + w, full.bottom - 1 };
        x += w;
        if (cell.right < cl.left || cell.left > cl.right) continue;
        RECT tr = cell; tr.left += S(8); tr.right -= S(6);
        if (tr.right <= tr.left) continue;

        switch (c)
        {
        case COL_FILENAME:
        {
            int is = S(16);
            RECT ir = { tr.left, midY - is / 2, tr.left + is, midY - is / 2 + is };
            if (HICON ic = FileIcon(r.name, is))
                DrawIconEx(dc, ir.left, ir.top, ic, is, is, 0, nullptr, DI_NORMAL);
            RECT nr = tr; nr.left = ir.right + S(8);
            std::wstring nm = r.name.empty() ? L"(resolving name…)" : r.name;
            if (r.status == NDM_STATUS_ERROR && !r.error.empty())
            {
                // single line: name, then the reason in red
                SIZE ns = MeasureText(dc, nm, m_fUI);
                RECT a = nr; a.right = (std::min)((int)nr.right, (int)(nr.left + ns.cx + 1));
                Text(dc, nm, a, fg, m_fUI, one);
                RECT b = nr; b.left = a.right + S(8);
                if (b.right - b.left > S(24))
                    Text(dc, r.error, b, RGB(230, 110, 110), m_fSmall, one);
            }
            else
                Text(dc, nm, nr, fg, m_fUI, one);
            break;
        }
        case COL_DATE:
            // blank when unknown: never a guessed date
            if (r.dateFt) Text(dc, NdmUi::FormatListDate(r.dateFt), tr, dim, m_fSmall, one);
            break;
        case COL_SIZE:
        {
            std::wstring s = r.total ? NdmFormatSize(r.total) : std::wstring(L"—");
            Text(dc, s, tr, r.total ? fg : dim, m_fUI,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);   // IDM style
            break;
        }
        case COL_STATUS:
        {
            COLORREF sc = StatusColor(r.status);
            Circle(dc, tr.left + S(3), midY, S(3), sc);
            RECT sr = tr; sr.left += S(12);
            Text(dc, StatusText(r.status), sr, sc, m_fBold, one);
            break;
        }
        case COL_PROGRESS:
        {
            double frac = r.total ? (double)r.done / (double)r.total : 0.0;
            if (r.status == NDM_STATUS_COMPLETED) frac = 1.0;
            if (frac > 1.0) frac = 1.0;
            bool indet = (r.status == NDM_STATUS_DOWNLOADING && r.total == 0);
            int bh = S(6);
            int pctW = S(46);
            RECT br = { tr.left, midY - bh / 2, tr.right - pctW, midY - bh / 2 + bh };
            COLORREF f1 = NDM_CLR_GOLD_DARK, f2 = NDM_CLR_GOLD_LIGHT;
            if (r.status == NDM_STATUS_PAUSED) { f1 = RGB(150, 104, 24); f2 = NDM_CLR_AMBER; }
            else if (r.status == NDM_STATUS_ERROR) { f1 = RGB(140, 40, 40); f2 = NDM_CLR_RED; }
            else if (r.status == NDM_STATUS_QUEUED) { f1 = RGB(80, 82, 90); f2 = RGB(120, 122, 130); }
            if (br.right - br.left > S(20))
                ProgressBar(dc, br, indet ? -1.0 : frac, f1, f2, m_animPhase);
            RECT pr = { tr.right - pctW + S(6), full.top, tr.right, full.bottom };
            if (br.right - br.left <= S(20)) pr.left = tr.left;
            std::wstring ps = indet ? NdmFormatSize(r.done)
                            : (r.total || r.status == NDM_STATUS_COMPLETED)
                                ? Fmt(L"%.1f%%", frac * 100.0) : std::wstring(L"—");
            Text(dc, ps, pr, r.status == NDM_STATUS_COMPLETED ? NDM_CLR_GOLD : fg, m_fSmall,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case COL_SPEED:
            if (r.status == NDM_STATUS_DOWNLOADING)
                Text(dc, NdmFormatSpeed(r.speed), tr, r.speed ? NDM_CLR_GREEN : dim, m_fUI, one);
            else
                Text(dc, L"—", tr, dim, m_fUI, one);
            break;
        case COL_TIMELEFT:
        {
            std::wstring s = L"—";
            if (r.status == NDM_STATUS_DOWNLOADING && r.speed > 0 && r.total > r.done)
                s = NdmFormatTime((DWORD)((r.total - r.done) / r.speed) + 1);
            else if (r.status == NDM_STATUS_COMPLETED)
                s = L"Done";
            Text(dc, s, tr, r.status == NDM_STATUS_COMPLETED ? NDM_CLR_GOLD : fg, m_fUI, one);
            break;
        }
        case COL_URL:
            Text(dc, r.url, tr, dim, m_fSmall, one);
            break;
        }
    }
}

void NdmMainWindow::DrawEmptyState(HDC dc)
{
    RECT cl; GetClientRect(m_hList, &cl);
    RECT hr = {};
    if (m_hHdr) GetWindowRect(m_hHdr, &hr);
    int top = cl.top + (hr.bottom - hr.top);
    RECT area = { cl.left, top, cl.right, cl.bottom };
    Fill(dc, area, NDM_CLR_BG);
    int cx = (area.left + area.right) / 2;
    int cy = area.top + (area.bottom - area.top) / 2 - S(24);
    if (area.bottom - area.top < S(140)) cy = area.top + S(44);

    int ts = S(40);
    RECT tile = { cx - ts / 2, cy - ts / 2, cx + ts / 2, cy + ts / 2 };
    GlyphTile(dc, tile, !m_query.empty() ? GLY_SEARCH
                      : m_filter == CAT_ALL ? GLY_DOWNARROW : CategoryGlyph(m_filter), m_dpi);

    std::wstring t1 = !m_query.empty()      ? std::wstring(L"No matches")
                    : (m_filter == CAT_ALL) ? std::wstring(L"No downloads")
                    : Fmt(L"Nothing in %s", CategoryName(m_filter));
    RECT r1 = { area.left + S(20), tile.bottom + S(10), area.right - S(20), tile.bottom + S(30) };
    Text(dc, t1, r1, NDM_CLR_FG, m_fBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (!m_query.empty())
    {
        RECT r2 = { area.left + S(20), r1.bottom, area.right - S(20), r1.bottom + S(18) };
        Text(dc, L"Press Esc in the search box to clear it", r2, NDM_CLR_FG_DIM, m_fSmall,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else if (m_filter == CAT_ALL)
    {
        RECT r2 = { area.left + S(20), r1.bottom, area.right - S(20), r1.bottom + S(18) };
        Text(dc, L"Press Ctrl+N to add a URL", r2, NDM_CLR_FG_DIM, m_fSmall,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// ===========================================================================
// Data refresh -- copies engine state on the UI thread, updates in place
// ===========================================================================
bool NdmMainWindow::RowMatches(const Row& r, int cat) const
{
    switch (cat)
    {
    case CAT_ALL:        return true;
    case CAT_UNFINISHED: return r.status != NDM_STATUS_COMPLETED;
    case CAT_FINISHED:   return r.status == NDM_STATUS_COMPLETED;
    default:             return r.cat == cat;
    }
}

const NdmMainWindow::Row* NdmMainWindow::RowById(DWORD id) const
{
    for (const Row& r : m_all) if (r.id == id) return &r;
    return nullptr;
}

void NdmMainWindow::SortView()
{
    if (m_sortCol < 0)
    {
        // No column chosen: queue order (what starts next comes first)
        std::stable_sort(m_view.begin(), m_view.end(), [&](int ia, int ib)
        {
            const Row& a = m_all[ia]; const Row& b = m_all[ib];
            if (a.queueOrder != b.queueOrder) return a.queueOrder < b.queueOrder;
            return a.id < b.id;
        });
        return;
    }
    auto key = [&](const Row& a, const Row& b) -> int
    {
        switch (m_sortCol)
        {
        case COL_FILENAME: return _wcsicmp(a.name.c_str(), b.name.c_str());
        case COL_DATE:     return a.dateFt < b.dateFt ? -1 : a.dateFt > b.dateFt ? 1 : 0;
        case COL_SIZE:     return a.total < b.total ? -1 : a.total > b.total ? 1 : 0;
        case COL_STATUS:   return (int)a.status - (int)b.status;
        case COL_PROGRESS:
        {
            double fa = a.status == NDM_STATUS_COMPLETED ? 1.0 : (a.total ? (double)a.done / a.total : 0);
            double fb = b.status == NDM_STATUS_COMPLETED ? 1.0 : (b.total ? (double)b.done / b.total : 0);
            return fa < fb ? -1 : fa > fb ? 1 : 0;
        }
        case COL_SPEED:    return a.speed < b.speed ? -1 : a.speed > b.speed ? 1 : 0;
        case COL_TIMELEFT:
        {
            UINT64 ta = (a.speed && a.total > a.done) ? (a.total - a.done) / a.speed : ~0ULL;
            UINT64 tb = (b.speed && b.total > b.done) ? (b.total - b.done) / b.speed : ~0ULL;
            return ta < tb ? -1 : ta > tb ? 1 : 0;
        }
        case COL_URL:      return _wcsicmp(a.url.c_str(), b.url.c_str());
        }
        return 0;
    };
    std::stable_sort(m_view.begin(), m_view.end(), [&](int ia, int ib)
    {
        const Row& a = m_all[ia]; const Row& b = m_all[ib];
        int k = key(a, b);
        if (k == 0) return a.id < b.id;
        return m_sortAsc ? k < 0 : k > 0;
    });
}

void NdmMainWindow::Refresh(bool force)
{
    if (!m_hWnd) return;

    std::vector<Row> prev;
    prev.swap(m_all);
    std::vector<DWORD> prevIds = m_viewIds;

    // ---- consistent value snapshot taken under the queue lock ----
    std::vector<NdmDownload> all = NdmQueue::GetInstance().GetAllSnapshots();
    m_all.reserve(all.size());
    m_totalSpeed  = 0;
    m_activeCount = 0;
    for (const NdmDownload& dl : all)
    {
        Row r;
        r.id       = dl.id;
        r.status   = dl.status;
        r.total    = dl.totalSize;
        r.done     = dl.downloadedSize;
        r.speed    = dl.speed;
        r.conns    = dl.activeConns;       // live: HTTP connections / torrent peers right now
        r.name     = dl.filename;
        r.url      = dl.url;
        r.savePath = dl.savePath;
        if (r.status == NDM_STATUS_ERROR) r.error = dl.errorMsg;
        if (r.total && r.done > r.total) r.done = r.total;
        if (r.status != NDM_STATUS_DOWNLOADING) r.speed = 0;
        r.kind     = dl.kind;
        r.dateFt   = NdmUi::ListDateFt(dl.status, dl.addedTime, dl.completedTime);
        r.priority = dl.priority;
        r.speedLimit = dl.speedLimit;
        r.scheduled = dl.scheduled;
        r.queueOrder = dl.queueOrder;
        r.cat = r.kind == NDM_KIND_TORRENT ? (int)CAT_TORRENT : CategoryOfFile(r.name);
        if (r.status == NDM_STATUS_DOWNLOADING) { ++m_activeCount; m_totalSpeed += r.speed; }
        m_all.push_back(std::move(r));
    }

    // ---- counts ----
    int counts[CAT_COUNT] = {};
    for (const Row& r : m_all)
        for (int c = 0; c < CAT_COUNT; ++c)
            if (RowMatches(r, c)) ++counts[c];
    bool countsChanged = memcmp(counts, m_catCount, sizeof(counts)) != 0;
    memcpy(m_catCount, counts, sizeof(counts));

    // ---- speed history ----
    m_speedHist.push_back(m_totalSpeed);
    if ((int)m_speedHist.size() > kSpeedHist)
        m_speedHist.erase(m_speedHist.begin(), m_speedHist.begin() + (m_speedHist.size() - kSpeedHist));

    // ---- view ----
    m_view.clear();
    for (int i = 0; i < (int)m_all.size(); ++i)
        if (RowMatches(m_all[i], m_filter) && NdmUi::MatchesSearch(m_all[i].name, m_all[i].url, m_query))
            m_view.push_back(i);
    std::vector<DWORD> ids;
    ids.reserve(m_view.size());
    for (int i : m_view) ids.push_back(m_all[i].id);
    // Sorted by a live column (values change every tick): keep the shown
    // order between re-sorts so rows do not jump under the mouse.
    {
        const bool live = m_sortCol == COL_STATUS || m_sortCol == COL_PROGRESS ||
                          m_sortCol == COL_SPEED  || m_sortCol == COL_TIMELEFT;
        const ULONGLONG now = GetTickCount64();
        const bool same = NdmUi::SameIdSet(ids, prevIds);
        if (NdmUi::ShouldResort(force, same, live, live && PointerOverList(), m_menuOpen,
                                now, m_lastSortMs))
        {
            SortView();
            m_lastSortMs = now;
        }
        else
        {
            std::vector<size_t> order = NdmUi::OrderLike(prevIds, ids);
            std::vector<int> v;
            v.reserve(order.size());
            for (size_t k : order) v.push_back(m_view[k]);
            m_view.swap(v);
        }
        ids.clear();
        for (int i : m_view) ids.push_back(m_all[i].id);
    }

    bool anyIndet = false;
    if (force || ids != prevIds)
    {
        // Structure changed: keep selection / focus by download id.
        std::set<DWORD> selIds;
        DWORD focusId = 0;
        int idx = -1;
        while ((idx = ListView_GetNextItem(m_hList, idx, LVNI_SELECTED)) >= 0)
            if (idx < (int)prevIds.size()) selIds.insert(prevIds[idx]);
        int fi = ListView_GetNextItem(m_hList, -1, LVNI_FOCUSED);
        if (fi >= 0 && fi < (int)prevIds.size()) focusId = prevIds[fi];

        m_viewIds = ids;
        m_updating = true;
        ListView_SetItemCountEx(m_hList, (int)ids.size(), LVSICF_NOSCROLL);
        ListView_SetItemState(m_hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        for (int i = 0; i < (int)ids.size(); ++i)
        {
            UINT st = 0;
            if (selIds.count(ids[i])) st |= LVIS_SELECTED;
            if (ids[i] == focusId)    st |= LVIS_FOCUSED;
            if (st) ListView_SetItemState(m_hList, i, st, st);
        }
        if (m_hotRow >= (int)ids.size()) m_hotRow = -1;
        m_updating = false;
        FitLastColumn();                   // vertical scrollbar may have changed
        InvalidateRect(m_hList, nullptr, FALSE);
    }
    else
    {
        // Same rows: repaint only those that changed.
        std::map<DWORD, const Row*> old;
        for (const Row& r : prev) old[r.id] = &r;
        for (int i = 0; i < (int)m_view.size(); ++i)
        {
            const Row& r = m_all[m_view[i]];
            auto it = old.find(r.id);
            if (it == old.end() || !(*it->second == r))
                ListView_RedrawItems(m_hList, i, i);
        }
    }
    for (int i : m_view)
        if (m_all[i].status == NDM_STATUS_DOWNLOADING && m_all[i].total == 0) { anyIndet = true; break; }
    if (anyIndet) SetTimer(m_hWnd, NDM_ANIM_TIMER, 40, nullptr);

    CheckNotifications();
    UpdateToolbarState();
    UpdateTrayTip();

    InvalidateRect(m_hWnd, &m_rcSpeed, FALSE);
    InvalidateRect(m_hWnd, &m_rcStatus, FALSE);
    if (countsChanged || force) InvalidateRect(m_hWnd, &m_rcSidebar, FALSE);
}

void NdmMainWindow::RebuildView(bool) { Refresh(true); }

std::vector<DWORD> NdmMainWindow::SelectedIds() const
{
    std::vector<DWORD> ids;
    int idx = -1;
    while ((idx = ListView_GetNextItem(m_hList, idx, LVNI_SELECTED)) >= 0)
        if (idx < (int)m_viewIds.size()) ids.push_back(m_viewIds[idx]);
    return ids;
}

void NdmMainWindow::UpdateToolbarState()
{
    bool anySel = false, canResume = false, canPause = false, anyActive = false;
    for (DWORD id : SelectedIds())
    {
        const Row* r = RowById(id);
        if (!r) continue;
        anySel = true;
        if (r->status == NDM_STATUS_PAUSED || r->status == NDM_STATUS_ERROR) canResume = true;
        if (r->status == NDM_STATUS_DOWNLOADING || r->status == NDM_STATUS_QUEUED) canPause = true;
    }
    for (const Row& r : m_all)
        if (r.status == NDM_STATUS_DOWNLOADING || r.status == NDM_STATUS_QUEUED) { anyActive = true; break; }

    bool changed = false;
    for (ToolBtn& b : m_btns)
    {
        bool en = true;
        switch (b.cmd)
        {
        case ID_CMD_RESUME:  en = canResume; break;
        case ID_CMD_PAUSE:   en = canPause;  break;
        case ID_CMD_STOPALL: en = anyActive; break;
        case ID_CMD_DELETE:  en = anySel;    break;
        }
        if (b.enabled != en) { b.enabled = en; changed = true; }
    }
    if (changed) InvalidateRect(m_hWnd, &m_rcToolbar, FALSE);
}

void NdmMainWindow::CheckNotifications()
{
    bool first = m_lastStatus.empty();
    std::map<DWORD, NdmStatus> now;
    for (const Row& r : m_all)
    {
        now[r.id] = r.status;
        if (first) continue;
        auto it = m_lastStatus.find(r.id);
        if (it == m_lastStatus.end()) continue;
        if (it->second != NDM_STATUS_COMPLETED && r.status == NDM_STATUS_COMPLETED)
        {
            if (NdmPrefs::NotifyTray())
                Balloon(L"Download complete", r.name + L"\n" + NdmFormatSize(r.total), r.id, BALLOON_COMPLETE);
            if (NdmPrefs::NotifySound())
                PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            if (NdmPrefs::NotifyDialog())
                NdmProgressDlg::Show(m_hWnd, r.id);     // shows Open / Open Folder / Close
        }
        else if (it->second == NDM_STATUS_DOWNLOADING && r.status == NDM_STATUS_ERROR)
        {
            if (NdmPrefs::NotifyTray())
                Balloon(L"Download failed", r.name + (r.error.empty() ? L"" : L"\n" + r.error), r.id,
                        BALLOON_FAILED);
        }
    }
    // keep a non-empty marker so "first" is evaluated only once
    if (now.empty()) now[0] = NDM_STATUS_QUEUED;
    m_lastStatus.swap(now);
}

// ===========================================================================
// Input
// ===========================================================================
int NdmMainWindow::HitToolbar(POINT pt) const
{
    for (size_t i = 0; i < m_btns.size(); ++i)
        if (PtInRect(&m_btns[i].rc, pt)) return (int)i;
    return -1;
}

int NdmMainWindow::HitSidebar(POINT pt) const
{
    if (!PtInRect(&m_rcSidebar, pt) || m_sideDrag) return -1;
    if (m_rcSideThumb.bottom > m_rcSideThumb.top && PtInRect(&m_rcSideTrack, pt)) return -1;
    for (int c = 0; c < CAT_COUNT; ++c)
        if (PtInRect(&m_catRc[c], pt)) return c;     // the visible part of a scrolled row
    return -1;
}

void NdmMainWindow::OnMouseMove(POINT pt)
{
    if (!m_trackingMouse)
    {
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, m_hWnd, 0 };
        TrackMouseEvent(&t);
        m_trackingMouse = true;
    }
    if (m_sideDrag)
    {
        int view = m_rcSidebar.bottom - m_rcSidebar.top;
        ScrollSidebar(NdmUi::ScrollFromThumbDrag(m_sideDragPos, pt.y - m_sideDragY, m_sideContentH, view,
                                                 m_rcSideTrack.bottom - m_rcSideTrack.top,
                                                 m_rcSideThumb.bottom - m_rcSideThumb.top));
        return;
    }
    {
        // the slim scrollbar turns gold under the pointer
        bool over = m_rcSideThumb.bottom > m_rcSideThumb.top && PtInRect(&m_rcSideTrack, pt);
        if (over != m_sideTrackHot) { m_sideTrackHot = over; InvalidateRect(m_hWnd, &m_rcSideTrack, FALSE); }
    }
    int b = HitToolbar(pt);
    if (b != m_hotBtn)
    {
        if (m_hotBtn >= 0) InvalidateRect(m_hWnd, &m_btns[m_hotBtn].rc, FALSE);
        m_hotBtn = b;
        if (b >= 0) InvalidateRect(m_hWnd, &m_btns[b].rc, FALSE);
    }
    int c = HitSidebar(pt);
    if (c != m_hotCat)
    {
        if (m_hotCat >= 0) InvalidateRect(m_hWnd, &m_catRc[m_hotCat], FALSE);
        m_hotCat = c;
        if (c >= 0) InvalidateRect(m_hWnd, &m_catRc[c], FALSE);
    }
}

void NdmMainWindow::OnLButtonDown(POINT pt)
{
    int b = HitToolbar(pt);
    if (b >= 0 && m_btns[b].enabled)
    {
        m_pressBtn = b;
        SetCapture(m_hWnd);
        InvalidateRect(m_hWnd, &m_btns[b].rc, FALSE);
        return;
    }
    if (m_rcSideThumb.bottom > m_rcSideThumb.top && PtInRect(&m_rcSideTrack, pt))
    {
        int view = m_rcSidebar.bottom - m_rcSidebar.top;
        if (PtInRect(&m_rcSideThumb, pt))
        {
            m_sideDrag = true;                       // drag the thumb
            m_sideDragY = pt.y;
            m_sideDragPos = m_sideScroll;
            SetCapture(m_hWnd);
            InvalidateRect(m_hWnd, &m_rcSidebar, FALSE);
        }
        else                                         // page up / down
            ScrollSidebar(m_sideScroll + (pt.y < m_rcSideThumb.top ? -view : view) * 3 / 4);
        return;
    }
    int c = HitSidebar(pt);
    if (c >= 0 && c != m_filter)
    {
        m_filter = c;
        Refresh(true);
        InvalidateRect(m_hWnd, &m_rcSidebar, FALSE);
        if (m_hList) SetFocus(m_hList);
    }
}

void NdmMainWindow::OnLButtonUp(POINT pt)
{
    if (m_sideDrag)
    {
        m_sideDrag = false;
        ReleaseCapture();
        InvalidateRect(m_hWnd, &m_rcSidebar, FALSE);
        return;
    }
    if (m_pressBtn < 0) return;
    int b = m_pressBtn;
    m_pressBtn = -1;
    ReleaseCapture();
    InvalidateRect(m_hWnd, &m_btns[b].rc, FALSE);
    if (HitToolbar(pt) == b && m_btns[b].enabled)
        OnCommand(m_btns[b].cmd);
}

void NdmMainWindow::OnColumnClick(int col)
{
    if (col == m_sortCol) m_sortAsc = !m_sortAsc;
    else { m_sortCol = col; m_sortAsc = true; }
    if (m_hHdr) InvalidateRect(m_hHdr, nullptr, FALSE);
    Refresh(true);                         // sorts now, also for live columns
    SaveUiState();
}

void NdmMainWindow::OnContextMenu(POINT pt)
{
    if (pt.x == -1 && pt.y == -1)
    {
        int fi = ListView_GetNextItem(m_hList, -1, LVNI_FOCUSED | LVNI_SELECTED);
        RECT rc = {};
        if (fi >= 0) ListView_GetItemRect(m_hList, fi, &rc, LVIR_LABEL);
        pt = { rc.left + S(20), rc.bottom };
        ClientToScreen(m_hList, &pt);
    }
    else
    {
        // right-click on an unselected row selects it
        POINT cp = pt; ScreenToClient(m_hList, &cp);
        LVHITTESTINFO ht = {}; ht.pt = cp;
        int hit = ListView_HitTest(m_hList, &ht);
        if (hit >= 0 && !(ListView_GetItemState(m_hList, hit, LVIS_SELECTED) & LVIS_SELECTED))
        {
            ListView_SetItemState(m_hList, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(m_hList, hit, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    std::vector<DWORD> sel = SelectedIds();
    HMENU m = CreatePopupMenu();
    if (!m) return;
    if (sel.empty())
    {
        AppendMenuW(m, MF_STRING, ID_CMD_ADD,       L"Add URL...\tCtrl+N");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_CMD_RESUMEALL, L"Resume all");
        AppendMenuW(m, MF_STRING, ID_CMD_STOPALL,   L"Stop all");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_CMD_SELECTALL, L"Select all\tCtrl+A");
    }
    else
    {
        bool anyDone = false, canRes = false, canPause = false, anyOpen = false, canMove = false;
        bool allSched = true;
        int  prio = -1;                       // shared priority of the unfinished ones (-2 = mixed)
        DWORD lim = 0; bool limMixed = false, limSeen = false;
        for (DWORD id : sel)
            if (const Row* r = RowById(id))
            {
                if (r->status == NDM_STATUS_COMPLETED) anyDone = true;
                else
                {
                    anyOpen = true;
                    if (!r->scheduled) allSched = false;
                    prio = prio == -1 ? r->priority : (prio == r->priority ? prio : -2);
                    if (!limSeen) { lim = r->speedLimit; limSeen = true; }
                    else if (lim != r->speedLimit) limMixed = true;
                }
                if (r->status == NDM_STATUS_PAUSED || r->status == NDM_STATUS_ERROR) canRes = true;
                if (r->status == NDM_STATUS_DOWNLOADING || r->status == NDM_STATUS_QUEUED) canPause = true;
                if (NdmUi::CanMoveInQueue(r->status)) canMove = true;
            }
        const Row* r0 = sel.size() == 1 ? RowById(sel[0]) : nullptr;
        bool canRename  = r0 && NdmUi::CanRename(r0->kind, r0->status, r0->done);
        bool canRefresh = r0 && NdmUi::CanRefreshAddress(r0->kind, r0->status);
        const UINT gOpen = anyOpen ? 0 : MF_GRAYED;

        HMENU pm = CreatePopupMenu();         // Priority
        AppendMenuW(pm, MF_STRING, ID_CMD_PRIO_HIGH,   L"High");
        AppendMenuW(pm, MF_STRING, ID_CMD_PRIO_NORMAL, L"Normal");
        AppendMenuW(pm, MF_STRING, ID_CMD_PRIO_LOW,    L"Low");
        if (prio >= NDM_PRIO_LOW && prio <= NDM_PRIO_HIGH)
            CheckMenuRadioItem(pm, 0, 2, NDM_PRIO_HIGH - prio, MF_BYPOSITION);

        HMENU sm = CreatePopupMenu();         // Speed limit
        const DWORD presets[4] = { 0, 256u * 1024u, 1024u * 1024u, 5u * 1024u * 1024u };
        AppendMenuW(sm, MF_STRING, ID_CMD_SPEED_OFF,  L"Unlimited");
        AppendMenuW(sm, MF_STRING, ID_CMD_SPEED_256K, L"256 KB/s");
        AppendMenuW(sm, MF_STRING, ID_CMD_SPEED_1M,   L"1 MB/s");
        AppendMenuW(sm, MF_STRING, ID_CMD_SPEED_5M,   L"5 MB/s");
        bool isPreset = false;
        for (int i = 0; i < 4; ++i) if (lim == presets[i]) isPreset = true;
        std::wstring custom = L"Custom\u2026";
        if (limSeen && !limMixed && !isPreset) custom += L"  (" + NdmUi::SpeedLabel(lim) + L")";
        AppendMenuW(sm, MF_STRING, ID_CMD_SPEED_CUSTOM, custom.c_str());
        if (limSeen && !limMixed)
        {
            int pos = 4;
            for (int i = 0; i < 4; ++i) if (lim == presets[i]) pos = i;
            CheckMenuRadioItem(sm, 0, 4, pos, MF_BYPOSITION);
        }

        HMENU mm = CreatePopupMenu();         // Move in queue
        AppendMenuW(mm, MF_STRING, ID_CMD_MOVE_TOP,    L"Move to top");
        AppendMenuW(mm, MF_STRING, ID_CMD_MOVE_UP,     L"Move up");
        AppendMenuW(mm, MF_STRING, ID_CMD_MOVE_DOWN,   L"Move down");
        AppendMenuW(mm, MF_STRING, ID_CMD_MOVE_BOTTOM, L"Move to bottom");

        AppendMenuW(m, MF_STRING | (anyDone ? 0 : MF_GRAYED), ID_CMD_OPEN, L"Open\tEnter");
        AppendMenuW(m, MF_STRING, ID_CMD_OPENFOLDER, L"Open folder\tCtrl+E");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (canRes ? 0 : MF_GRAYED),   ID_CMD_RESUME, L"Resume");
        AppendMenuW(m, MF_STRING | (canPause ? 0 : MF_GRAYED), ID_CMD_PAUSE,  L"Stop / Pause");
        AppendMenuW(m, MF_STRING, ID_CMD_REDOWNLOAD, L"Redownload");
        AppendMenuW(m, MF_STRING | (canRefresh ? 0 : MF_GRAYED), ID_CMD_REFRESHADDR, L"Refresh download address\u2026");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_POPUP | gOpen, (UINT_PTR)pm, L"Priority");
        AppendMenuW(m, MF_POPUP | gOpen, (UINT_PTR)sm, L"Speed limit");
        AppendMenuW(m, MF_POPUP | (canMove ? 0 : MF_GRAYED), (UINT_PTR)mm, L"Move in queue");
        AppendMenuW(m, MF_STRING | gOpen | (anyOpen && allSched ? MF_CHECKED : 0), ID_CMD_SCHEDULED,
                    L"Add to scheduler");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (canRename ? 0 : MF_GRAYED), ID_CMD_RENAME, L"Rename\u2026\tF2");
        AppendMenuW(m, MF_STRING, ID_CMD_COPYNAME, L"Copy file name");
        AppendMenuW(m, MF_STRING, ID_CMD_COPYURL,  L"Copy address\tCtrl+C");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_CMD_DELETE,     L"Delete\tDel");
        AppendMenuW(m, MF_STRING, ID_CMD_DELETEFILE, L"Delete with file\u2026\tShift+Del");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, ID_CMD_PROPERTIES, L"Properties\tAlt+Enter");
        SetMenuDefaultItem(m, anyDone ? ID_CMD_OPEN : ID_CMD_PROPERTIES, FALSE);
    }
    m_menuOpen = true;                        // live sort columns hold still meanwhile
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, m_hWnd, nullptr);
    m_menuOpen = false;
    DestroyMenu(m);                           // destroys the submenus too
    if (cmd) OnCommand(cmd);
}

void NdmMainWindow::OnCommand(int id)
{
    switch (id)
    {
    case ID_CMD_ADD:        CmdAdd(); break;
    case ID_CMD_RESUME:     CmdResume(); break;
    case ID_CMD_PAUSE:      CmdPause(); break;
    case ID_CMD_STOPALL:
    case ID_TRAY_PAUSEALL:  CmdStopAll(); break;
    case ID_CMD_RESUMEALL:
    case ID_TRAY_RESUMEALL: CmdResumeAll(); break;
    case ID_CMD_DELETE:     CmdDelete(); break;
    case ID_CMD_OPENFOLDER: CmdOpenFolder(); break;
    case ID_CMD_OPTIONS:
    case ID_TRAY_OPTIONS:   CmdOptions(); break;
    case ID_CMD_OPEN:       CmdOpen(); break;
    case ID_CMD_REDOWNLOAD: CmdRedownload(); break;
    case ID_CMD_COPYURL:    CmdCopyUrl(); break;
    case ID_CMD_PROPERTIES: CmdProperties(); break;
    case ID_CMD_DEFAULT:    CmdDefault(); break;
    case ID_CMD_SELECTALL:
        ListView_SetItemState(m_hList, -1, LVIS_SELECTED, LVIS_SELECTED);
        break;
    case ID_CMD_RENAME:       CmdRename(); break;
    case ID_CMD_COPYNAME:     CmdCopyName(); break;
    case ID_CMD_DELETEFILE:   CmdDeleteWithFile(); break;
    case ID_CMD_REFRESHADDR:  CmdRefreshAddress(); break;
    case ID_CMD_SCHEDULED:    CmdScheduled(); break;
    case ID_CMD_PRIO_LOW:
    case ID_CMD_PRIO_NORMAL:
    case ID_CMD_PRIO_HIGH:    CmdPriority(id - ID_CMD_PRIO_LOW); break;
    case ID_CMD_SPEED_OFF:    CmdSpeed(0, false); break;
    case ID_CMD_SPEED_256K:   CmdSpeed(256u * 1024u, false); break;
    case ID_CMD_SPEED_1M:     CmdSpeed(1024u * 1024u, false); break;
    case ID_CMD_SPEED_5M:     CmdSpeed(5u * 1024u * 1024u, false); break;
    case ID_CMD_SPEED_CUSTOM: CmdSpeed(0, true); break;
    case ID_CMD_MOVE_TOP:
    case ID_CMD_MOVE_UP:
    case ID_CMD_MOVE_DOWN:
    case ID_CMD_MOVE_BOTTOM:  CmdMove(id); break;
    case ID_CMD_SCHEDULER:    NdmSchedDlg::Show(m_hWnd); break;
    case ID_CMD_FIND:
        if (m_hSearch && IsWindowVisible(m_hSearch))
        {
            SetFocus(m_hSearch);
            SendMessageW(m_hSearch, EM_SETSEL, 0, -1);
        }
        break;
    case ID_TRAY_SPEED_OFF:    CmdTraySpeed(0, false); break;
    case ID_TRAY_SPEED_256K:   CmdTraySpeed(256u * 1024u, false); break;
    case ID_TRAY_SPEED_1M:     CmdTraySpeed(1024u * 1024u, false); break;
    case ID_TRAY_SPEED_5M:     CmdTraySpeed(5u * 1024u * 1024u, false); break;
    case ID_TRAY_SPEED_CUSTOM: CmdTraySpeed(0, true); break;
    case ID_TRAY_SHOW:      ShowMain(); break;
    case ID_TRAY_ADD:       CmdAdd(); break;
    case ID_TRAY_EXIT:      ExitApp(); break;
    }
}

// ===========================================================================
// Commands
// ===========================================================================
void NdmMainWindow::CmdAdd()
{
    NdmAddDlg::ShowAddUrl(m_hWnd);
}

void NdmMainWindow::CmdResume()
{
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
            if (r->status == NDM_STATUS_PAUSED || r->status == NDM_STATUS_ERROR)
                NdmQueue::GetInstance().ResumeDownload(id);
    Refresh(false);
}

void NdmMainWindow::CmdResumeAll()
{
    for (const Row& r : m_all)
        if (r.status == NDM_STATUS_PAUSED || r.status == NDM_STATUS_ERROR)
            NdmQueue::GetInstance().ResumeDownload(r.id);
    Refresh(false);
}

void NdmMainWindow::CmdPause()
{
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
            if (r->status == NDM_STATUS_DOWNLOADING || r->status == NDM_STATUS_QUEUED)
                NdmQueue::GetInstance().PauseDownload(id);
    Refresh(false);
}

void NdmMainWindow::CmdStopAll()
{
    // Hold queued items first so freed slots are not refilled, then pause
    // the running ones.
    NdmQueue& q = NdmQueue::GetInstance();
    std::vector<NdmDownload> all = q.GetAllSnapshots();
    for (const NdmDownload& dl : all)
        if (dl.status == NDM_STATUS_QUEUED) q.PauseDownload(dl.id);
    for (const NdmDownload& dl : all)
        if (dl.status == NDM_STATUS_DOWNLOADING) q.PauseDownload(dl.id);
    Refresh(false);
}

void NdmMainWindow::CmdDelete()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.empty()) return;
    std::wstring msg;
    if (sel.size() == 1)
    {
        const Row* r = RowById(sel[0]);
        msg = L"Remove “" + (r ? r->name : std::wstring(L"this download")) +
              L"” from the list?";
    }
    else
        msg = Fmt(L"Remove %d downloads from the list?", (int)sel.size());
    msg += L"\n\nFiles that were already saved to disk are kept.";
    NdmDialogs::AskSpec ask;
    ask.title   = L"Delete downloads";
    ask.text    = msg;
    ask.buttons = { L"Remove", L"Cancel" };
    ask.defaultButton = 1;                 // Enter keeps the list as it is
    ask.cancelButton  = 1;
    ask.dangerButton  = 0;
    ask.glyph   = GLY_DELETE;
    if (NdmDialogs::AskEx(m_hWnd, ask) != 0)
        return;

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (DWORD id : sel)
    {
        NdmProgressDlg::CloseFor(id);
        NdmQueue::GetInstance().RemoveDownload(id);   // frees the NdmDownload
    }
    SetCursor(old);
    Refresh(true);       // rows are value copies: nothing dangles
}

void NdmMainWindow::CmdOpenFolder()
{
    std::vector<DWORD> sel = SelectedIds();
    if (!sel.empty())
        if (const Row* r = RowById(sel[0]))
        {
            OpenFolderSelect(r->savePath.empty() ? NdmConfigMgr::GetInstance().GetSavePath()
                                                 : r->savePath, r->name);
            return;
        }
    std::wstring d = NdmConfigMgr::GetInstance().GetSavePath();
    EnsureFolder(d);
    OpenFolderSelect(d, L"");
}

void NdmMainWindow::CmdOpen()
{
    for (DWORD id : SelectedIds())
    {
        const Row* r = RowById(id);
        if (!r || r->status != NDM_STATUS_COMPLETED) continue;
        std::wstring full = JoinPath(r->savePath, r->name);
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            NdmDialogs::Notice(m_hWnd, L"Open file", L"The file could not be found:\n\n" + full, GLY_FILE);
            continue;
        }
        ShellExecuteW(m_hWnd, L"open", full.c_str(), nullptr, r->savePath.c_str(), SW_SHOWNORMAL);
        break;
    }
}

void NdmMainWindow::CmdRedownload()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.empty()) return;
    NdmQueue& q = NdmQueue::GetInstance();

    // Finished files of the selection that are still on disk.
    int onDisk = 0;
    std::wstring firstName;
    for (DWORD id : sel)
    {
        NdmDownload d;
        if (!q.GetSnapshot(id, d) || d.status != NDM_STATUS_COMPLETED || d.filename.empty()) continue;
        std::wstring f = d.savePath;
        if (!f.empty() && f.back() != L'\\') f += L'\\';
        f += d.filename;
        DWORD a = GetFileAttributesW(f.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
        {
            if (!onDisk) firstName = d.filename;
            ++onDisk;
        }
    }

    // Replace the old file (default), keep both, or cancel.
    bool replace = false;
    {
        std::wstring text = sel.size() == 1 ? L"Download this file again from the beginning?"
                                            : L"Download the selected files again from the beginning?";
        NdmDialogs::AskSpec ask;
        ask.title = L"Redownload";
        ask.glyph = GLY_REFRESH;
        if (onDisk)
        {
            text += L"\n\n";
            text += onDisk == 1 ? L"“" + firstName + L"” is already in the download folder."
                                : std::to_wstring(onDisk) + L" of these files are already in the download folder.";
            text += L" Keep both saves the new download under a new name.";
            ask.buttons = { L"Replace", L"Keep both", L"Cancel" };
            ask.defaultButton = 0;
            ask.cancelButton  = 2;
        }
        else
        {
            ask.buttons = { L"Redownload", L"Cancel" };
            ask.defaultButton = 0;
            ask.cancelButton  = 1;
        }
        ask.text = text;
        int a = NdmDialogs::AskEx(m_hWnd, ask);
        if (a < 0 || a == ask.cancelButton) return;
        replace = onDisk && a == 0;
    }

    for (DWORD id : sel)
    {
        NdmDownload dl;
        if (!q.GetSnapshot(id, dl)) continue;    // value copy: safe after Remove
        // A running download first stops, so its partial files are released
        // and removed before the new one picks its file name.
        if (dl.status == NDM_STATUS_DOWNLOADING)
        {
            q.PauseDownload(id);
            for (int i = 0; i < 100; ++i)
            {
                NdmDownload cur;
                if (!q.GetSnapshot(id, cur) || cur.status != NDM_STATUS_DOWNLOADING) break;
                Sleep(50);
            }
        }
        NdmAddParams p;
        p.url         = dl.url;
        p.savePath    = dl.savePath;
        p.filename    = dl.filename;
        p.connections = dl.numConnections;
        p.referrer    = dl.referrer;
        p.cookies     = dl.cookies;
        p.userAgent   = dl.userAgent;
        p.startNow    = true;
        p.kind        = dl.kind;
        p.pageUrl     = dl.pageUrl;
        p.mediaFormat = dl.mediaFormat;
        p.torrentFiles = dl.torrentFiles;
        p.cookieJar   = dl.cookieJar;
        p.overwriteConfirmed = replace && dl.status == NDM_STATUS_COMPLETED;
        NdmProgressDlg::CloseFor(id);
        q.RemoveDownload(id);
        q.AddDownloadEx(p);
    }
    Refresh(true);
}

void NdmMainWindow::CmdCopyUrl()
{
    std::wstring s;
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
        {
            if (!s.empty()) s += L"\r\n";
            s += r->url;
        }
    if (!s.empty()) CopyToClipboard(m_hWnd, s);
}

void NdmMainWindow::CmdProperties()
{
    std::vector<DWORD> sel = SelectedIds();
    if (!sel.empty()) NdmProgressDlg::Show(m_hWnd, sel[0]);
}

void NdmMainWindow::CmdDefault()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.empty()) return;
    const Row* r = RowById(sel[0]);
    if (r && r->status == NDM_STATUS_COMPLETED) CmdOpen();
    else NdmProgressDlg::Show(m_hWnd, sel[0]);
}

void NdmMainWindow::CmdOptions()
{
    NdmOptDlg::Show(m_hWnd);
    Refresh(true);
}

void NdmMainWindow::CmdRename()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.size() != 1) return;
    NdmDownload d;
    if (!NdmQueue::GetInstance().GetSnapshot(sel[0], d)) return;
    if (!NdmUi::CanRename(d.kind, d.status, d.downloadedSize))
    {
        std::wstring why = d.kind == NDM_KIND_TORRENT ? L"Torrent downloads cannot be renamed."
                         : d.status == NDM_STATUS_DOWNLOADING
                             ? L"Pause the download first. A download can be renamed before it starts or after it finishes."
                             : L"Part of this file is already downloaded. Rename it when it has finished.";
        NdmDialogs::Ask(m_hWnd, L"Rename", why, { L"OK" });
        return;
    }
    const DWORD id = d.id;
    const std::wstring old = d.filename;
    std::wstring name = old;
    auto validate = [id, &old](const std::wstring& t) -> std::wstring
    {
        std::wstring v = t;
        while (!v.empty() && iswspace(v.back())) v.pop_back();
        while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
        if (v.empty()) return L"Enter a file name.";
        if (v == old) return L"";
        std::wstring msg;
        if (!NdmQueue::GetInstance().RenameDownload(id, v, msg)) return msg;
        return L"";
    };
    if (NdmDialogs::Input(m_hWnd, L"Rename", d.status == NDM_STATUS_COMPLETED ? L"New name for the file"
                                                                             : L"Save the file as",
                          name, validate, false, nullptr, true))
        Refresh(true);
}

void NdmMainWindow::CmdCopyName()
{
    std::wstring s;
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
        {
            if (r->name.empty()) continue;
            if (!s.empty()) s += L"\r\n";
            s += r->name;
        }
    if (!s.empty()) CopyToClipboard(m_hWnd, s);
}

void NdmMainWindow::CmdDeleteWithFile()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.empty()) return;
    NdmQueue& q = NdmQueue::GetInstance();
    int unfinished = 0;
    for (DWORD id : sel)
        if (const Row* r = RowById(id)) if (r->status != NDM_STATUS_COMPLETED) ++unfinished;
    std::wstring msg;
    if (sel.size() == 1)
    {
        const Row* r = RowById(sel[0]);
        msg = L"Delete \u201C" + (r ? r->name : std::wstring(L"this download")) + L"\u201D";
        msg += unfinished ? L" and its downloaded part?" : L" and move the file to the Recycle Bin?";
    }
    else
        msg = Fmt(L"Delete %d downloads and move their files to the Recycle Bin?", (int)sel.size());
    if (unfinished && sel.size() > 1) msg += L" Unfinished parts are removed.";
    if (NdmDialogs::Ask(m_hWnd, L"Delete with file", msg, { L"Delete", L"Cancel" }, GLY_DELETE) != 0)
        return;

    std::vector<std::wstring> failed;
    for (DWORD id : sel)
    {
        NdmDownload d;
        if (!q.GetSnapshot(id, d)) continue;
        if (d.status == NDM_STATUS_COMPLETED && NdmUi::IsLeafName(d.filename) && !d.savePath.empty())
        {
            std::wstring full = JoinPath(d.savePath, d.filename);
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES && !RecycleFile(m_hWnd, full))
            {
                failed.push_back(d.filename);          // keep the item: nothing was lost
                continue;
            }
        }
        NdmProgressDlg::CloseFor(id);
        q.RemoveDownload(id);                         // unfinished: its temp files go with it
    }
    Refresh(true);
    if (!failed.empty())
    {
        std::wstring t = failed.size() == 1
            ? L"\u201C" + failed[0] + L"\u201D could not be moved to the Recycle Bin, so it stays in the list."
            : Fmt(L"%d files could not be moved to the Recycle Bin, so they stay in the list.", (int)failed.size());
        NdmDialogs::Ask(m_hWnd, L"Delete with file", t, { L"OK" });
    }
}

void NdmMainWindow::CmdRefreshAddress()
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.size() != 1) return;
    const Row* r = RowById(sel[0]);
    if (!r || !NdmUi::CanRefreshAddress(r->kind, r->status)) return;
    NdmDialogs::ShowRefresh(m_hWnd, sel[0]);
}

void NdmMainWindow::CmdPriority(int prio)
{
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
            if (r->status != NDM_STATUS_COMPLETED) NdmQueue::GetInstance().SetPriority(id, prio);
    Refresh(true);
}

void NdmMainWindow::CmdSpeed(DWORD bps, bool custom)
{
    std::vector<DWORD> sel = SelectedIds();
    if (sel.empty()) return;
    if (custom)
    {
        const Row* r0 = nullptr;
        for (DWORD id : sel) if (const Row* r = RowById(id)) if (r->status != NDM_STATUS_COMPLETED) { r0 = r; break; }
        if (!r0) return;
        std::wstring t = r0->speedLimit ? std::to_wstring((r0->speedLimit + 512) / 1024) : std::wstring(L"512");
        auto validate = [](const std::wstring& v) -> std::wstring
        {
            return NdmUi::SpeedFromInput(v, false) ? L"" : L"Enter a speed of at least 1 KB/s.";
        };
        if (!NdmDialogs::Input(m_hWnd, L"Speed limit", L"Limit for the selected downloads", t, validate,
                               true, L"KB/s"))
            return;
        bps = NdmUi::SpeedFromInput(t, false);
    }
    for (DWORD id : sel)
        if (const Row* r = RowById(id))
            if (r->status != NDM_STATUS_COMPLETED) NdmQueue::GetInstance().SetDownloadSpeedLimit(id, bps);
    Refresh(true);
}

void NdmMainWindow::CmdMove(int cmd)
{
    NdmQueue& q = NdmQueue::GetInstance();
    std::vector<DWORD> order = q.GetQueueOrder();
    std::map<DWORD, size_t> pos;
    for (size_t i = 0; i < order.size(); ++i) pos[order[i]] = i;
    std::vector<DWORD> sel;
    for (DWORD id : SelectedIds())
        if (const Row* r = RowById(id))
            if (NdmUi::CanMoveInQueue(r->status) && pos.count(id)) sel.push_back(id);
    // Keep the selection's own order: top / down go from the last one,
    // up / bottom from the first one.
    std::sort(sel.begin(), sel.end(), [&](DWORD a, DWORD b) { return pos[a] < pos[b]; });
    if (cmd == ID_CMD_MOVE_TOP || cmd == ID_CMD_MOVE_DOWN) std::reverse(sel.begin(), sel.end());
    for (DWORD id : sel)
    {
        switch (cmd)
        {
        case ID_CMD_MOVE_TOP:    q.MoveToTop(id); break;
        case ID_CMD_MOVE_UP:     q.MoveUp(id); break;
        case ID_CMD_MOVE_DOWN:   q.MoveDown(id); break;
        case ID_CMD_MOVE_BOTTOM: q.MoveToBottom(id); break;
        }
    }
    bool sortReset = m_sortCol >= 0;
    if (sortReset)
    {
        // Show the queue order the move just changed.
        m_sortCol = -1;
        if (m_hHdr) InvalidateRect(m_hHdr, nullptr, FALSE);
    }
    q.ScheduleNext();
    Refresh(true);
    if (sortReset) SaveUiState();
}

void NdmMainWindow::CmdScheduled()
{
    std::vector<DWORD> sel = SelectedIds();
    bool allOn = true;
    for (DWORD id : sel)
        if (const Row* r = RowById(id)) if (r->status != NDM_STATUS_COMPLETED && !r->scheduled) allOn = false;
    for (DWORD id : sel)
        if (const Row* r = RowById(id))
            if (r->status != NDM_STATUS_COMPLETED) NdmQueue::GetInstance().SetScheduled(id, !allOn);
    Refresh(true);
}

// Global limit: the same setting as Options > Connection.
void NdmMainWindow::CmdTraySpeed(DWORD bps, bool custom)
{
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    if (custom)
    {
        DWORD cur = cfg.GetSpeedLimit();
        std::wstring t = cur ? std::to_wstring((cur + 512) / 1024) : std::wstring(L"1024");
        auto validate = [](const std::wstring& v) -> std::wstring
        {
            return NdmUi::SpeedFromInput(v, false) ? L"" : L"Enter a speed of at least 1 KB/s.";
        };
        HWND owner = IsWindowVisible(m_hWnd) ? m_hWnd : nullptr;
        if (!NdmDialogs::Input(owner, L"Speed limit", L"Limit for all downloads together", t, validate,
                               true, L"KB/s"))
            return;
        bps = NdmUi::SpeedFromInput(t, false);
    }
    cfg.SetSpeedLimit(bps);
    cfg.Save();
    NdmOptDlg::ApplySettings();            // applies the limit, restores the Run key
    Refresh(false);
}

void NdmMainWindow::OnSearchChanged()
{
    std::wstring q = GetText(m_hSearch);
    while (!q.empty() && iswspace(q.back())) q.pop_back();
    while (!q.empty() && iswspace(q.front())) q.erase(q.begin());
    if (q == m_query) return;
    m_query = q;
    InvalidateRect(m_hWnd, &m_rcSearch, FALSE);
    Refresh(true);                         // selection is kept by download id
}

void NdmMainWindow::ShowExisting(DWORD id)
{
    if (!m_hWnd) return;
    bool visible = false;
    for (DWORD v : m_viewIds) if (v == id) visible = true;
    if (!visible)
    {
        m_filter = CAT_ALL;
        if (m_hSearch && GetWindowTextLengthW(m_hSearch)) SetWindowTextW(m_hSearch, L"");   // EN_CHANGE refreshes
        m_query.clear();
        Refresh(true);
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    ShowMain();
    for (size_t i = 0; i < m_viewIds.size(); ++i)
        if (m_viewIds[i] == id)
        {
            ListView_SetItemState(m_hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(m_hList, (int)i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(m_hList, (int)i, FALSE);
            SetFocus(m_hList);
            break;
        }
    NdmProgressDlg::Show(m_hWnd, id);
}

void NdmMainWindow::QueueDrop(const std::vector<std::wstring>& items, bool files)
{
    if (!m_hWnd || items.empty()) return;
    bool pending = !m_drops.empty();
    m_drops = items;
    m_dropFiles = files;
    if (!pending) PostMessageW(m_hWnd, WM_NDM_DROP, 0, 0);
}

void NdmMainWindow::OnDrop()
{
    std::vector<std::wstring> items;
    items.swap(m_drops);
    bool files = m_dropFiles;
    for (const std::wstring& it : items)
    {
        if (!m_hWnd) return;
        if (files || NdmTorrentIsTorrentLink(it))
        {
            // .torrent file / magnet / .torrent link: the torrent dialog
            NdmBrowserRequest* req = new NdmBrowserRequest();
            req->url = it;
            req->origin = NDM_ORIGIN_CMDLINE;           // the user's own action: local .torrent allowed
            HandleBrowserRequest(req);                  // takes ownership
        }
        else if (NdmIsHttpUrl(it))
        {
            NdmAddDlg::ShowAddUrl(m_hWnd, it);
            break;                                      // one Add URL window
        }
    }
}

void NdmMainWindow::OnClipboardUpdate()
{
    if (!NdmPrefs::WatchClipboard()) return;
    if (GetClipboardSequenceNumber() == m_ownClipSeq) return;          // NDM copied it
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;                          // copied inside NDM
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(m_hWnd))
    {
        // another program still holds it: try again shortly
        if (m_clipRetries++ < 5) SetTimer(m_hWnd, NDM_CLIP_TIMER, 150, nullptr);
        return;
    }
    std::wstring text;
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
    {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(hd))
        {
            SIZE_T cap = GlobalSize(hd) / sizeof(wchar_t);
            text.assign(p, wcsnlen(p, (std::min)((SIZE_T)8193, cap)));
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    std::wstring url = NdmUi::ClipboardDownloadLink(text);
    if (url.empty() || !m_clipGate.Offer(url, GetTickCount64())) return;
    NdmAddDlg::ShowAddUrl(m_hWnd, url);                                // prefilled, not started
}

// Before a browser download is shown / queued: an existing paused or
// failed item with the same name and size may continue with the new link,
// and a link already in the list is not added twice without asking.
// Returns false when the request was handled here or cancelled.
bool NdmMainWindow::CheckBrowserDuplicates(NdmBrowserRequest& req)
{
    NdmQueue& q = NdmQueue::GetInstance();
    if (req.kind == NDM_KIND_HTTP && !req.silent && req.fileSize)
    {
        std::wstring name = SanitizeFilename(req.filename);
        if (name.empty()) name = NdmSafeFilename(req.url);
        if (DWORD cand = q.FindRefreshCandidate(req.url, name, req.fileSize))
        {
            int a = NdmDialogs::AskContinueWithNewLink(m_hWnd, cand);
            if (a == 0) { NdmDialogs::ShowRefresh(m_hWnd, cand, &req); return false; }
            return a == 1;                    // add as new: no second question
        }
    }
    std::vector<DWORD> dup = q.FindByUrl(req.url);
    if (dup.empty()) return true;
    if (req.silent) return false;             // batch adds skip what is already listed
    int a = NdmDialogs::AskDuplicate(m_hWnd, dup[0]);
    if (a == 0) { ShowExisting(dup[0]); return false; }
    return a == 1;
}

// Immediate exit requested by setup / uninstall (NDM_ExitNow_v1): no
// questions, no tray. Saves the queue and settings, shuts the torrent engine
// down (saveSession), stops the download workers, then destroys the window;
// WinMain's own shutdown steps are no-ops afterwards.
void NdmMainWindow::ExitNow()
{
    if (m_exitNowDone) return;
    m_exitNowDone = true;
    m_exiting = true;
    NdmMsgHost::SetMainWindow(nullptr);
    SavePlacement();
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    const std::wstring queuePath = cfg.GetAppDataPath() + L"queue.xml";
    NdmQueue::GetInstance().SaveQueue(queuePath);
    cfg.Save();
    NdmTorrentShutdown();
    NdmQueue::GetInstance().Shutdown(5000);     // stops workers, writes the final state
    NdmTheme::CloseAllModeless();
    if (m_hWnd) DestroyWindow(m_hWnd);
}

void NdmMainWindow::ExitApp()
{
    if (m_exiting) return;
    int active = 0;
    for (const Row& r : m_all) if (r.status == NDM_STATUS_DOWNLOADING) ++active;
    if (active)
    {
        if (!IsWindowVisible(m_hWnd)) ShowMain();
        std::wstring msg = Fmt(L"%d download%s still running.\n\nExit NDM anyway? "
                               L"Unfinished downloads can be resumed next time.",
                               active, active == 1 ? L" is" : L"s are");
        NdmDialogs::AskSpec ask;
        ask.title   = L"Exit NDM";
        ask.text    = msg;
        ask.buttons = { L"Exit", L"Keep running" };
        ask.defaultButton = 1;             // Enter / Esc keep the downloads going
        ask.cancelButton  = 1;
        ask.glyph   = GLY_DOWNARROW;
        if (NdmDialogs::AskEx(m_hWnd, ask) != 0)
            return;
    }
    m_exiting = true;
    DestroyWindow(m_hWnd);
}

// ===========================================================================
// Browser capture / shell extension entry point
// ===========================================================================
// http/https/magnet always; a local "*.torrent" file only from our own
// command line (drive-letter path or file:/// without host; no \\server\ or
// \\?\ paths, no device names).
bool NdmMainWindow::AcceptableSource(const std::wstring& url, int origin)
{
    if (NdmIsNetOrMagnetUrl(url)) return true;
    if (origin != NDM_ORIGIN_CMDLINE) return false;
    std::wstring p = url;
    if (_wcsnicmp(p.c_str(), L"file:///", 8) == 0) p = p.substr(8);
    else if (_wcsnicmp(p.c_str(), L"file:", 5) == 0) return false;      // file://server/...
    for (auto& ch : p) if (ch == L'/') ch = L'\\';
    if (p.size() < 12 || p.size() >= 32768) return false;
    if (!iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\') return false;  // C:\... only
    if (p.find(L"\\\\") != std::wstring::npos) return false;
    for (wchar_t ch : p) if (ch < 0x20) return false;
    return _wcsicmp(p.c_str() + p.size() - 8, L".torrent") == 0;
}

void NdmMainWindow::HandleBrowserRequest(NdmBrowserRequest* raw)
{
    std::unique_ptr<NdmBrowserRequest> req(raw);
    if (!req || req->url.empty()) return;

    // Only network addresses; a local .torrent path only from our own command
    // line (file association / second instance), never a UNC path.
    if (!AcceptableSource(req->url, req->origin)) return;

    // magnet: / .torrent (command line, file association, second instance,
    // browser capture, silent or not) -> torrent dialog
    if (req->kind == NDM_KIND_TORRENT || NdmTorrentIsTorrentLink(req->url))
    {
        // A page cannot stack torrent dialogs: one at a time, and a short
        // pause after one closed, for anything not from our command line.
        static bool      s_open = false;
        static ULONGLONG s_closed = 0;
        const ULONGLONG now = GetTickCount64();
        if (req->origin != NDM_ORIGIN_CMDLINE && (s_open || (s_closed && now - s_closed < 1500)))
            return;
        s_open = true;
        NdmTorrentShowAddDialog(m_hWnd, req->url);
        s_open = false;
        s_closed = GetTickCount64();
        Refresh(true);
        return;
    }

    // Options -> Browser capture governs AUTOMATIC captures only (downloads
    // the extension intercepted). An explicit "Download with NDM" (context
    // menu, video / photo button, link picker, popup, shell extension) is
    // always taken: the extension already told the user "Sent to NDM".
    if (req->autoCapture && !NdmConfigMgr::GetInstance().GetBrowserMonitor())
    {
        // Capture is switched off in Options: do not add anything, but tell
        // the user instead of dropping the link silently.
        std::wstring name = req->filename.empty() ? NdmSafeFilename(req->url) : req->filename;
        Balloon(L"Browser capture is off",
                L"NDM ignored “" + name + L"”. Turn capture on in Options → Browser.", 0);
        return;
    }

    // Already in the list / same file with a fresh link?
    if (!CheckBrowserDuplicates(*req)) return;

    // "Show Download File Info" off (Options / first run): queue directly.
    // Links from the shell extension / command line (no browser data) still
    // show the dialog so the user can pick the location.
    bool fromBrowser = !req->referrer.empty() || !req->cookies.empty() ||
                       !req->userAgent.empty() || !req->mime.empty() || req->fileSize ||
                       req->kind == NDM_KIND_MEDIA;
    if (req->silent || (fromBrowser && !NdmPrefs::FileInfo()))
    {
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        std::wstring name = SanitizeFilename(req->filename);
        if (name.empty()) name = NdmSafeFilename(req->url);
        int cat = CategoryOfFile(name);
        if (req->kind == NDM_KIND_MEDIA && cat == CAT_GENERAL) cat = CAT_VIDEO;
        std::wstring folder = CategoryFolder(cfg.GetSavePath(), cat);
        EnsureFolder(folder);
        NdmAddParams p;
        p.url         = req->url;
        p.savePath    = folder;
        p.filename    = name;
        p.connections = cfg.GetMaxConnections();
        p.referrer    = req->referrer;
        p.cookies     = req->cookies;
        p.userAgent   = req->userAgent;
        p.startNow    = true;
        p.kind        = req->kind;
        p.pageUrl     = req->pageUrl;
        p.mediaFormat = req->mediaFormat;
        p.cookieJar   = req->cookieJar;
        NdmQueue::GetInstance().AddDownloadEx(p);   // never overwrites (overwriteConfirmed = false)
        Refresh(true);
        return;
    }
    NdmAddDlg::ShowBrowserRequest(m_hWnd, *req);
}

// ===========================================================================
// Tray
// ===========================================================================
void NdmMainWindow::AddTrayIcon()
{
    if (m_trayAdded) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd             = m_hWnd;
    nid.uID              = NDM_TRAY_ICON_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_NDM_TRAY_ICON;
    nid.hIcon            = m_hIconSm ? m_hIconSm : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"NDM — Nee Download Manager");
    m_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    g_lastTip.clear();
}

void NdmMainWindow::RemoveTrayIcon()
{
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = m_hWnd;
    nid.uID  = NDM_TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayAdded = false;
}

void NdmMainWindow::UpdateTrayTip()
{
    if (!m_trayAdded) return;
    std::wstring& last = g_lastTip;
    std::wstring tip = m_activeCount
        ? Fmt(L"NDM — %d downloading · %s", m_activeCount, NdmFormatSpeed(m_totalSpeed).c_str())
        : std::wstring(L"NDM — Nee Download Manager");
    if (tip == last) return;
    last = tip;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd   = m_hWnd;
    nid.uID    = NDM_TRAY_ICON_ID;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void NdmMainWindow::Balloon(const std::wstring& title, const std::wstring& text, DWORD id, int kind)
{
    if (!m_trayAdded) AddTrayIcon();
    if (!m_trayAdded) return;
    m_balloonId = id;
    m_balloonKind = kind;
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd   = m_hWnd;
    nid.uID    = NDM_TRAY_ICON_ID;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = m_hIconBig;
    if (!nid.hBalloonIcon) nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void NdmMainWindow::ShowTrayMenu()
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    bool vis = IsWindowVisible(m_hWnd) && !IsIconic(m_hWnd);
    AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, vis ? L"Show NDM" : L"Open NDM");
    AppendMenuW(m, MF_STRING, ID_TRAY_ADD,  L"Add URL...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_RESUMEALL, L"Resume all");
    AppendMenuW(m, MF_STRING | (m_activeCount ? 0 : MF_GRAYED), ID_TRAY_PAUSEALL, L"Stop all");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    {
        HMENU sm = CreatePopupMenu();
        DWORD cur = NdmConfigMgr::GetInstance().GetSpeedLimit();
        const DWORD presets[4] = { 0, 256u * 1024u, 1024u * 1024u, 5u * 1024u * 1024u };
        AppendMenuW(sm, MF_STRING, ID_TRAY_SPEED_OFF,  L"Off");
        AppendMenuW(sm, MF_STRING, ID_TRAY_SPEED_256K, L"256 KB/s");
        AppendMenuW(sm, MF_STRING, ID_TRAY_SPEED_1M,   L"1 MB/s");
        AppendMenuW(sm, MF_STRING, ID_TRAY_SPEED_5M,   L"5 MB/s");
        int pos = 4;
        for (int i = 0; i < 4; ++i) if (cur == presets[i]) pos = i;
        std::wstring custom = L"Custom\u2026";
        if (pos == 4) custom += L"  (" + NdmUi::SpeedLabel(cur) + L")";
        AppendMenuW(sm, MF_STRING, ID_TRAY_SPEED_CUSTOM, custom.c_str());
        CheckMenuRadioItem(sm, 0, 4, pos, MF_BYPOSITION);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sm, L"Speed limit");
    }
    AppendMenuW(m, MF_STRING, ID_TRAY_OPTIONS, L"Options...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_TRAY_SHOW, FALSE);

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(m_hWnd);          // required so the menu closes properly
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                             TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hWnd, nullptr);
    PostMessageW(m_hWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd);
}

void NdmMainWindow::HideToTray()
{
    AddTrayIcon();
    ShowWindow(m_hWnd, SW_HIDE);
    // Once ever (ndm.ini [UI] TrayHintShown), not once per session.
    NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
    if (!cfg.Ui().trayHintShown)
    {
        cfg.Ui().trayHintShown = true;
        cfg.SaveUi();
        Balloon(L"NDM is still running",
                L"Downloads continue in the background. Click the tray icon to open NDM.", 0);
    }
}

void NdmMainWindow::ShowMain()
{
    if (!m_hWnd) return;
    if (!IsWindowVisible(m_hWnd)) ShowWindow(m_hWnd, SW_SHOW);
    if (IsIconic(m_hWnd)) ShowWindow(m_hWnd, SW_RESTORE);
    ForceForeground(m_hWnd);
    Refresh(false);
}

// ===========================================================================
// Placement persistence (HKCU\Software\Devnetra\NDM\WindowPlacement)
// ===========================================================================
void NdmMainWindow::SavePlacement()
{
    if (!m_hWnd) return;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(m_hWnd, &wp)) return;
    if (!IsWindowVisible(m_hWnd) && wp.showCmd == SW_HIDE) wp.showCmd = SW_SHOWNORMAL;
    if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWNORMAL;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, NDM_REG_ROOT, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(k, L"WindowPlacement", 0, REG_BINARY, (const BYTE*)&wp, sizeof(wp));
        RegCloseKey(k);
    }
}

bool NdmMainWindow::LoadPlacement(WINDOWPLACEMENT& wp)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NDM_REG_ROOT, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, sz = sizeof(wp);
    LONG rc = RegQueryValueExW(k, L"WindowPlacement", nullptr, &type, (BYTE*)&wp, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_BINARY || sz != sizeof(wp) || wp.length != sizeof(wp))
        return false;
    RECT& r = wp.rcNormalPosition;
    if (r.right - r.left < 400 || r.bottom - r.top < 300) return false;
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;   // monitor gone
    wp.flags = 0;
    return true;
}

// ===========================================================================
// File-type icons (cached per extension; destroyed on exit / DPI change)
// ===========================================================================
HICON NdmMainWindow::FileIcon(const std::wstring& name, int size)
{
    std::wstring ext = L".";
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && name.size() - dot <= 12) ext = name.substr(dot);
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    auto it = m_iconCache.find(ext);
    if (it != m_iconCache.end()) return it->second;

    SHFILEINFOW sfi = {};
    UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES |
                 (size > GetSystemMetrics(SM_CXSMICON) ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    std::wstring probe = L"file" + (ext == L"." ? std::wstring(L".bin") : ext);
    HICON ic = nullptr;
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags))
        ic = sfi.hIcon;
    m_iconCache[ext] = ic;
    return ic;
}

// ===========================================================================
// Subclasses: ListView (hover row) and its header (dark paint, height)
// ===========================================================================
LRESULT CALLBACK NdmMainWindow::ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                             UINT_PTR id, DWORD_PTR ref)
{
    NdmMainWindow* self = (NdmMainWindow*)ref;
    switch (m)
    {
    case WM_MOUSEWHEEL:
    {
        // Wheel over the sidebar while the list has focus: scroll the sidebar.
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HWND main = GetParent(h);
        ScreenToClient(main, &pt);
        if (PtInRect(&self->m_rcSidebar, pt)) return SendMessageW(main, m, w, l);
        break;
    }
    case WM_MOUSEMOVE:
    {
        LVHITTESTINFO ht = {};
        ht.pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hit = ListView_HitTest(h, &ht);
        if (hit != self->m_hotRow)
        {
            int old = self->m_hotRow;
            self->m_hotRow = hit;
            if (old >= 0) ListView_RedrawItems(h, old, old);
            if (hit >= 0) ListView_RedrawItems(h, hit, hit);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (self->m_hotRow >= 0)
        {
            int old = self->m_hotRow;
            self->m_hotRow = -1;
            ListView_RedrawItems(h, old, old);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, ListSubclass, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK NdmMainWindow::HeaderSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                               UINT_PTR id, DWORD_PTR ref)
{
    NdmMainWindow* self = (NdmMainWindow*)ref;
    switch (m)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        {
            NdmMemDC mem(hdc, rc);
            self->PaintColumnHeader(h, mem.ok() ? mem.dc : hdc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case HDM_LAYOUT:
    {
        LRESULT r = DefSubclassProc(h, m, w, l);
        HDLAYOUT* hl = (HDLAYOUT*)l;
        if (hl && hl->pwpos && hl->prc)
        {
            int hh = self->S(kColHdrH);
            hl->pwpos->cy = hh;
            hl->prc->top  = hl->pwpos->y + hh;
        }
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, HeaderSubclass, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// Search box: Esc clears (then goes back to the list), Enter / Down go to
// the list. Filtering itself happens on EN_CHANGE.
LRESULT CALLBACK NdmMainWindow::SearchSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                               UINT_PTR id, DWORD_PTR ref)
{
    NdmMainWindow* self = (NdmMainWindow*)ref;
    switch (m)
    {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            if (GetWindowTextLengthW(h)) SetWindowTextW(h, L"");
            else if (self->m_hList) SetFocus(self->m_hList);
            return 0;
        }
        if (w == VK_RETURN || w == VK_DOWN)
        {
            if (self->m_hList)
            {
                SetFocus(self->m_hList);
                if (!self->m_viewIds.empty() && ListView_GetNextItem(self->m_hList, -1, LVNI_SELECTED) < 0)
                    ListView_SetItemState(self->m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == VK_ESCAPE || w == VK_RETURN) return 0;     // no beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, SearchSubclass, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}
