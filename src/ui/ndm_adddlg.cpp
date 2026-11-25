// ndm_adddlg.cpp -- "Download File Info" / "Add URL" window (modeless, compact)
// Invented with Care By Devnetra Consultancy
//
// One implementation serves two entry points:
//   * Add URL (Ctrl+N / toolbar): editable address, clipboard prefill.
//   * Browser / shell-extension capture: read-only address, browser-supplied
//     name / size / cookies (and media fields), brought to the foreground.
//
// Layout (96-dpi units, client 460 x 150):
//   URL      [..................................................]
//   Save as  [.............................................] [F]
//   [glyph] Video  ·  1080p MP4  ·  245.3 MB
//   ------------------------------------------------------------
//                    [Start Download] [Download Later] [Cancel]

#include "ndm_adddlg.h"
#include "ndm_theme.h"
#include "ndm_mainwnd.h"
#include "ndm_progdlg.h"
#include "ndm_dialogs.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"
#include "../torrent/ndm_torrent.h"

#include <commctrl.h>
#include <memory>

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass = L"NDMFileInfoWnd";
    const int kW = 460, kH = 150;            // client size, 96-dpi units
    const int kM = 12;                       // outer margin
    const int kLabelW = 56;                  // label column
    const int kFieldH = 26;
    const int kFooterY = 106;
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);

    enum
    {
        IDC_URL = 300, IDC_PATH, IDC_BROWSE, IDC_START, IDC_LATER, IDC_CANCEL_BTN
    };

    HWND g_addWnd = nullptr;                 // single Add URL window

    struct Ctx
    {
        bool         browser = false;
        HWND         hwnd = nullptr, owner = nullptr;
        NdmDlgRes    res;
        HWND hUrl = nullptr, hPath = nullptr, hBrowse = nullptr,
             hStart = nullptr, hLater = nullptr, hCancel = nullptr;
        RECT rcUrl{}, rcPath{}, rcInfo{}, rcFooter{};
        // request data
        std::wstring referrer, cookies, userAgent, mime;
        int          kind = NDM_KIND_HTTP;
        std::wstring pageUrl, mediaFormat, quality, cookieJar;
        UINT64       size = 0;
        int          cat = CAT_GENERAL;
        bool         pathEdited = false;
        int          setting = 0;            // >0 while text is set programmatically
        bool         topmostDropped = false;
        std::wstring error;                  // inline validation message
        int S(int v) const { return res.S(v); }
    };

    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    void SetTextQuiet(Ctx* c, HWND h, const std::wstring& s)
    {
        ++c->setting;
        SetWindowTextW(h, s.c_str());
        --c->setting;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n\"");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_last_not_of(L" \t\r\n\"");
        return s.substr(a, b - a + 1);
    }

    bool IsDownloadUrl(const std::wstring& u)
    {
        std::wstring l = u;
        for (auto& ch : l) ch = (wchar_t)towlower(ch);
        // http(s) only: the engine (WinHTTP) has no FTP.
        return (l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0) && l.size() > 8 &&
               l.find_first_of(L" \t\r\n") == std::wstring::npos;
    }

    // Split "C:\dir\name.ext" into folder + name
    void SplitPath(const std::wstring& full, std::wstring& dir, std::wstring& name)
    {
        size_t p = full.find_last_of(L"\\/");
        if (p == std::wstring::npos) { dir.clear(); name = full; return; }
        dir  = full.substr(0, p);
        name = full.substr(p + 1);
        if (dir.size() == 2 && dir[1] == L':') dir += L'\\';   // "C:" -> "C:\"
    }

    std::wstring DefaultPathFor(const std::wstring& name, int kind = NDM_KIND_HTTP)
    {
        int cat = CategoryOfFile(name);
        if (kind == NDM_KIND_MEDIA && cat == CAT_GENERAL) cat = CAT_VIDEO;
        return JoinPath(CategoryFolder(NdmConfigMgr::GetInstance().GetSavePath(), cat), name);
    }

    void UpdateCategory(Ctx* c)
    {
        std::wstring dir, name;
        SplitPath(Trim(GetText(c->hPath)), dir, name);
        c->cat = CategoryOfFile(name);
        if (c->kind == NDM_KIND_MEDIA && c->cat == CAT_GENERAL) c->cat = CAT_VIDEO;
        if (NdmTorrentIsTorrentLink(Trim(GetText(c->hUrl)))) c->cat = CAT_TORRENT;
        InvalidateRect(c->hwnd, &c->rcInfo, FALSE);
    }

    void Layout(Ctx* c)
    {
        int L = c->S(kM + kLabelW), R = c->S(kW - kM);
        int fh = c->S(kFieldH);
        c->rcUrl  = { L, c->S(12), R, c->S(12) + fh };
        int bw = fh;                                     // square browse button
        c->rcPath = { L, c->S(46), R - bw - c->S(6), c->S(46) + fh };
        MoveWindow(c->hBrowse, R - bw, c->rcPath.top, bw, fh, TRUE);
        c->rcInfo = { c->S(kM), c->S(80), R, c->S(98) };

        PlaceEdit(c->hUrl,  c->rcUrl,  c->res.ui, c->S(8));
        PlaceEdit(c->hPath, c->rcPath, c->res.ui, c->S(8));

        c->rcFooter = { 0, c->S(kFooterY), c->S(kW), c->S(kH) };
        int bh = c->S(28), by = c->rcFooter.top + (c->S(kH - kFooterY) - bh) / 2;
        int x = R;
        int wS = c->S(124), wL = c->S(116), wC = c->S(76), gap = c->S(8);
        MoveWindow(c->hStart,  x - wS, by, wS, bh, TRUE); x -= wS + gap;
        MoveWindow(c->hLater,  x - wL, by, wL, bh, TRUE); x -= wL + gap;
        MoveWindow(c->hCancel, x - wC, by, wC, bh, TRUE);

        HWND btns[] = { c->hBrowse, c->hStart, c->hLater, c->hCancel };
        for (HWND b : btns) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
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

            auto label = [&](const RECT& field, const wchar_t* t)
            {
                RECT lr = { c->S(kM), field.top, field.left - c->S(8), field.bottom };
                Text(dc, t, lr, NDM_CLR_FG_DIM, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            };
            HWND f = GetFocus();
            label(c->rcUrl, L"URL");
            DrawField(dc, c->rcUrl, f == c->hUrl, c->browser, r.dpi);
            label(c->rcPath, L"Save as");
            DrawField(dc, c->rcPath, f == c->hPath, false, r.dpi);

            // one info line: type · quality · size   (or the validation error)
            RECT ir = c->rcInfo;
            if (!c->error.empty())
            {
                Text(dc, c->error, ir, RGB(236, 110, 110), r.ui,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            else
            {
                int gs = c->S(14);
                RECT gr = { ir.left + c->S(kLabelW) - gs - c->S(8), (ir.top + ir.bottom - gs) / 2, 0, 0 };
                gr.right = gr.left + gs; gr.bottom = gr.top + gs;
                Glyph(dc, CategoryGlyph(c->cat), gr, NDM_CLR_GOLD, 1.0f);
                std::wstring info = c->cat == CAT_GENERAL ? L"File" : CategoryName(c->cat);
                if (!c->quality.empty()) info += L"  ·  " + c->quality;
                if (c->size)              info += L"  ·  " + NdmFormatSize(c->size);
                else if (c->browser)      info += L"  ·  Size unknown";
                RECT tr = { c->rcUrl.left, ir.top, ir.right, ir.bottom };
                Text(dc, info, tr, NDM_CLR_FG, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            Fill(dc, c->rcFooter, kFootBg);
            HLine(dc, 0, cl.right, c->rcFooter.top, RGB(40, 44, 53));
        }
        EndPaint(c->hwnd, &ps);
    }

    void SetError(Ctx* c, const wchar_t* msg, HWND focus)
    {
        c->error = msg;
        InvalidateRect(c->hwnd, &c->rcInfo, FALSE);
        if (focus) { SetFocus(focus); SendMessageW(focus, EM_SETSEL, 0, -1); }
    }

    bool Commit(Ctx* c, bool startNow)
    {
        NdmConfigMgr& cfg = NdmConfigMgr::GetInstance();
        std::wstring url = Trim(GetText(c->hUrl));
        if (NdmTorrentIsTorrentLink(url))          // magnet / .torrent -> torrent dialog
        {
            HWND owner = c->owner;
            DestroyWindow(c->hwnd);              // c is freed after this
            NdmTorrentShowAddDialog(owner, url);
            NdmMainWindow::GetInstance().RefreshListView();
            return true;
        }
        if (!IsDownloadUrl(url))
        {
            SetError(c, L"Enter a valid http:// or https:// address (or a magnet link).", c->hUrl);
            return false;
        }
        // Typed / pasted link already in the list? (Browser captures were
        // checked before this window opened.)
        if (!c->browser)
        {
            std::vector<DWORD> dup = NdmQueue::GetInstance().FindByUrl(url);
            if (!dup.empty())
            {
                int a = NdmDialogs::AskDuplicate(c->hwnd, dup[0]);
                if (a < 0) return false;
                if (a == 0)
                {
                    DestroyWindow(c->hwnd);              // c is freed after this
                    NdmMainWindow::GetInstance().ShowExisting(dup[0]);
                    return true;
                }
            }
        }
        std::wstring folder, name;
        SplitPath(Trim(GetText(c->hPath)), folder, name);
        name = SanitizeFilename(Trim(name));
        if (name.empty()) name = NdmSafeFilename(url);
        if (folder.empty()) folder = CategoryFolder(cfg.GetSavePath(), CategoryOfFile(name));
        if (!EnsureFolder(folder))
        {
            SetError(c, L"The folder cannot be created. Choose another location.", c->hPath);
            return false;
        }

        std::wstring full = JoinPath(folder, name);
        bool replace = false;                // only an explicit "Replace" may overwrite
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::wstring other = NdmUi::KeepBothName(name, [&](const std::wstring& n)
            {
                return GetFileAttributesW(JoinPath(folder, n).c_str()) != INVALID_FILE_ATTRIBUTES;
            });
            NdmDialogs::AskSpec ask;
            ask.title = L"File exists";
            ask.text  = L"“" + name + L"” already exists in this folder.";
            if (!other.empty()) ask.text += L"\n\nKeep both saves the new file as “" + other + L"”.";
            ask.buttons = { L"Replace", L"Keep both", L"Cancel" };
            ask.defaultButton = other.empty() ? 2 : 1;   // never overwrite on Enter
            ask.cancelButton  = 2;
            ask.dangerButton  = 0;
            ask.glyph = GLY_FILE;
            int a = NdmDialogs::AskEx(c->hwnd, ask);
            if (a != 0 && a != 1) return false;
            if (a == 1 && other.empty()) return false;
            replace = a == 0;
            if (a == 1) name = other;
        }

        NdmAddParams p;
        p.url         = url;
        p.savePath    = folder;
        p.filename    = name;
        p.connections = cfg.GetMaxConnections();
        p.referrer    = c->referrer;
        p.cookies     = c->cookies;
        p.userAgent   = c->userAgent;
        p.startNow    = startNow;
        p.kind        = c->kind;
        p.pageUrl     = c->pageUrl;
        p.mediaFormat = c->mediaFormat;
        p.cookieJar   = c->cookieJar;
        p.overwriteConfirmed = replace;
        DWORD id = NdmQueue::GetInstance().AddDownloadEx(p);
        if (!id)
        {
            SetError(c, L"The download could not be added.", nullptr);
            return false;
        }
        NdmMainWindow::GetInstance().RefreshListView();
        HWND owner = c->owner;
        DestroyWindow(c->hwnd);              // c is freed after this
        if (startNow) NdmProgressDlg::Show(owner, id);
        return true;
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        Ctx* c = Get(h);
        switch (m)
        {
        case WM_NCCREATE:
        {
            c = (Ctx*)((CREATESTRUCTW*)l)->lpCreateParams;
            c->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
            break;
        }
        case WM_CREATE:
        {
            c->res.Create(GetDpi(h));
            DarkTitleBar(h);
            SetWindowIcons(h, c->res.dpi);
            c->hUrl    = MakeEdit(h, IDC_URL, c->browser ? ES_READONLY : 0);
            c->hPath   = MakeEdit(h, IDC_PATH);
            // the captions are painted: name the fields for screen readers
            SetAccName(c->hUrl,  L"URL");
            SetAccName(c->hPath, L"Save as");
            c->hBrowse = MakeBtn(h, IDC_BROWSE, L"", GLY_FOLDER);
            SetAccName(c->hBrowse, L"Choose folder");
            c->hStart  = MakeBtn(h, IDC_START, L"Start Download");
            c->hLater  = MakeBtn(h, IDC_LATER, L"Download Later");
            c->hCancel = MakeBtn(h, IDC_CANCEL_BTN, L"Cancel");
            SendMessageW(c->hUrl, EM_LIMITTEXT, 8192, 0);
            SendMessageW(c->hPath, EM_LIMITTEXT, MAX_PATH, 0);
            Layout(c);
            return 0;
        }
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
            COLORREF bg = kFootBg;
            switch (di->CtlID)
            {
            case IDC_START:      k = BTN_PRIMARY;   break;
            case IDC_LATER:      k = BTN_SECONDARY; break;
            case IDC_CANCEL_BTN: k = BTN_GHOST;     break;
            case IDC_BROWSE:     k = BTN_STEPPER; bg = kBg; break;
            }
            DrawButton(di, k, bg, c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && c && !c->topmostDropped &&
                (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST))
            {
                c->topmostDropped = true;
                SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            break;
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id)
            {
            case IDC_URL:
                if (code == EN_CHANGE && !c->setting)
                {
                    c->error.clear();
                    if (!c->pathEdited)
                    {
                        std::wstring u = Trim(GetText(c->hUrl));
                        std::wstring n = IsDownloadUrl(u) ? NdmSafeFilename(u) : L"";
                        SetTextQuiet(c, c->hPath, DefaultPathFor(n));
                    }
                    UpdateCategory(c);
                }
                break;
            case IDC_PATH:
                if (code == EN_CHANGE && !c->setting)
                {
                    c->pathEdited = true;
                    c->error.clear();
                    UpdateCategory(c);
                }
                break;
            case IDC_BROWSE:
            {
                std::wstring dir, name;
                SplitPath(Trim(GetText(c->hPath)), dir, name);
                std::wstring d = BrowseFolder(h, dir, L"Choose where to save the file");
                if (!d.empty())
                {
                    SetTextQuiet(c, c->hPath, JoinPath(d, name));
                    c->pathEdited = true;
                    UpdateCategory(c);
                }
                break;
            }
            case IDOK:
            case IDC_START:
                Commit(c, true);
                return 0;
            case IDC_LATER:
                Commit(c, false);
                return 0;
            case IDCANCEL:
            case IDC_CANCEL_BTN:
                DestroyWindow(h);
                return 0;
            }
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS)
                InvalidateRect(h, nullptr, FALSE);
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
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            RemoveModeless(h);
            if (g_addWnd == h) g_addWnd = nullptr;
            if (c->owner && IsWindow(c->owner) && IsWindowVisible(c->owner) &&
                GetForegroundWindow() == h)
                SetForegroundWindow(c->owner);
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

    HWND CreateInfoWindow(HWND owner, Ctx* c, bool topmost)
    {
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
        c->owner = owner;
        POINT pt = CreationPoint(owner);
        DWORD ex = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | (topmost ? WS_EX_TOPMOST : 0);
        HWND h = CreateWindowExW(ex, kClass,
                                 c->browser ? L"Download File Info" : L"Add URL",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                 pt.x, pt.y, 100, 100, nullptr, nullptr, hi, c);
        if (!h) { delete c; return nullptr; }
        PlaceWindow(h, kW, kH, owner);
        AddModeless(h);
        return h;
    }
}

// ===========================================================================
// Public API
// ===========================================================================
std::wstring NdmAddDlg::GetClipboardUrl()
{
    std::wstring result;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return result;
    if (!OpenClipboard(nullptr)) return result;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData)
    {
        const wchar_t* p = (const wchar_t*)GlobalLock(hData);
        if (p)
        {
            size_t n = wcsnlen(p, 8192);
            std::wstring s = Trim(std::wstring(p, n));
            GlobalUnlock(hData);
            if (IsDownloadUrl(s) || (s.rfind(L"magnet:?", 0) == 0 && s.find_first_of(L" \t\r\n") == std::wstring::npos))
                result = s;
        }
    }
    CloseClipboard();
    return result;
}

void NdmAddDlg::ShowAddUrl(HWND owner, const std::wstring& prefill)
{
    if (g_addWnd && IsWindow(g_addWnd))
    {
        if (!prefill.empty())
            if (Ctx* c = Get(g_addWnd)) SetWindowTextW(c->hUrl, prefill.c_str());
        ForceForeground(g_addWnd);
        return;
    }
    Ctx* c = new Ctx();
    c->browser = false;
    HWND h = CreateInfoWindow(owner, c, false);
    if (!h) return;
    g_addWnd = h;

    SetTextQuiet(c, c->hPath, JoinPath(CategoryFolder(NdmConfigMgr::GetInstance().GetSavePath(),
                                                      CAT_GENERAL), L""));
    std::wstring url = prefill.empty() ? GetClipboardUrl() : prefill;
    if (!url.empty())
        SetWindowTextW(c->hUrl, url.c_str());      // EN_CHANGE fills the path
    UpdateCategory(c);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetFocus(c->hUrl);
    SendMessageW(c->hUrl, EM_SETSEL, 0, -1);
}

void NdmAddDlg::ShowBrowserRequest(HWND owner, const NdmBrowserRequest& req)
{
    Ctx* c = new Ctx();
    c->browser     = true;
    c->referrer    = req.referrer;
    c->cookies     = req.cookies;
    c->userAgent   = req.userAgent;
    c->mime        = req.mime;
    c->size        = req.fileSize;
    c->kind        = req.kind;
    c->pageUrl     = req.pageUrl;
    c->mediaFormat = req.mediaFormat;
    c->cookieJar   = req.cookieJar;
    c->quality     = req.kind == NDM_KIND_MEDIA ? req.qualityLabel : std::wstring();
    HWND h = CreateInfoWindow(owner, c, true);
    if (!h) return;

    SetTextQuiet(c, c->hUrl, req.url);
    std::wstring name = SanitizeFilename(req.filename);
    if (name.empty()) name = NdmSafeFilename(req.url);
    SetTextQuiet(c, c->hPath, DefaultPathFor(name, c->kind));
    UpdateCategory(c);

    ShowWindow(h, SW_SHOW);
    MessageBeep(MB_OK);
    ForceForeground(h);
    SetFocus(c->hStart);
}
