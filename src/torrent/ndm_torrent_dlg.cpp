// ndm_torrent_dlg.cpp -- compact "Add Torrent" window (modal, dark + gold)
// Invented with Care By Devnetra Consultancy
//
// Layout (96-dpi units, client 460 x 320):
//   [tile]  Torrent name ..........................................
//           1.46 GB  ·  3 files            (or "Fetching metadata…")
//   ------------------------------------------------------------
//   FILES                                                    SIZE
//   [x] file one.iso                                     1.20 GB
//   [ ] readme.txt                                         2.1 KB
//   2 of 3 selected  ·  1.20 GB                 [Select all] [None]
//   Save to  [.............................................] [F]
//   ------------------------------------------------------------
//                    [Start Download] [Download Later] [Cancel]
//
// .torrent files are parsed locally (remote ones are fetched first); magnet
// links fetch their metadata through aria2 (bt-metadata-only) on a worker
// thread. "Start" is allowed before the metadata arrives (= all files).

#include "ndm_torrent.h"
#include "ndm_torrent_internal.h"
#include "../ui/ndm_theme.h"
#include "../ui/ndm_progdlg.h"
#include "../core/ndm_queue.h"
#include "../core/ndm_config.h"

#include <commctrl.h>
#include <memory>
#include <atomic>

using namespace NdmTheme;

namespace
{
    const wchar_t* kClass = L"NDMTorrentAddWnd";
    const int kW = 460, kH = 320;
    const int kM = 12;
    const int kFooterY = 280;
    const COLORREF kBg     = RGB(24, 27, 33);
    const COLORREF kFootBg = RGB(20, 22, 27);
    const COLORREF kListBg = RGB(28, 31, 38);
    const UINT WM_TOR_META = WM_APP + 0x51;

    enum { IDC_LIST = 400, IDC_PATH, IDC_BROWSE, IDC_ALL, IDC_NONE, IDC_START, IDC_LATER, IDC_CANCEL_BTN };

    struct MetaJob                                  // shared with the worker thread
    {
        std::wstring      src;
        bool              magnet = false;
        HANDLE            cancel = nullptr;
        std::atomic<HWND> hwnd{ nullptr };
        std::string       bytes;
        std::wstring      err;
        bool              ok = false;
        ~MetaJob() { if (cancel) CloseHandle(cancel); }
    };

    struct Ctx
    {
        HWND hwnd = nullptr, owner = nullptr;
        NdmDlgRes res;
        HWND hList = nullptr, hPath = nullptr, hBrowse = nullptr, hAll = nullptr, hNone = nullptr,
             hStart = nullptr, hLater = nullptr, hCancel = nullptr;
        HIMAGELIST hChecks = nullptr;
        RECT rcHead{}, rcCap{}, rcList{}, rcSum{}, rcPath{}, rcFooter{};

        std::wstring src;
        bool         magnet = false;
        std::string  hash;
        std::wstring name;
        bool         haveInfo = false;
        NdmTorrentInfo info;
        std::vector<int> rowToFile;                 // list row -> info.files index
        bool         fetching = false;
        std::wstring status;                        // subtitle text when no info
        std::wstring error;                         // red line
        std::shared_ptr<MetaJob> job;
        int          filling = 0;
        bool         done = false, added = false;
        int S(int v) const { return res.S(v); }
    };

    Ctx* Get(HWND h) { return (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA); }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n\"");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_last_not_of(L" \t\r\n\"");
        return s.substr(a, b - a + 1);
    }

    // ---- gold checkbox state images ---------------------------------------
    void BuildChecks(Ctx* c)
    {
        if (c->hChecks) ImageList_Destroy(c->hChecks);
        int sz = c->S(16);
        c->hChecks = ImageList_Create(sz, sz, ILC_COLOR32, 2, 0);
        HDC screen = GetDC(nullptr);
        for (int i = 1; i <= 2; ++i)                // state image 1 = unchecked, 2 = checked (list slots 0, 1)
        {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HDC dc = CreateCompatibleDC(screen);
            HGDIOBJ old = SelectObject(dc, bmp);
            RECT rc = { 0, 0, sz, sz };
            Fill(dc, rc, kListBg);
            RECT box = { c->S(2), c->S(2), sz - c->S(2), sz - c->S(2) };
            if (i == 1)
            {
                RoundFill(dc, box, c->S(3), NDM_CLR_EDIT_BG);
                RoundFrame(dc, box, c->S(3), RGB(92, 98, 112), 1.0f);
            }
            else if (i == 2)
            {
                RoundFillV(dc, box, c->S(3), NDM_CLR_GOLD_LIGHT, NDM_CLR_GOLD);
                RECT g = box; InflateRect(&g, -c->S(2), -c->S(2));
                Glyph(dc, GLY_CHECK, g, RGB(28, 24, 8), 1.3f);
            }
            SelectObject(dc, old);
            DeleteDC(dc);
            // opaque alpha so the 32-bit image list does not treat it as transparent
            for (int p = 0; p < sz * sz; ++p) ((DWORD*)bits)[p] |= 0xFF000000;
            ImageList_Add(c->hChecks, bmp, nullptr);
            DeleteObject(bmp);
        }
        ReleaseDC(nullptr, screen);
        ListView_SetImageList(c->hList, c->hChecks, LVSIL_STATE);
    }

    // ---- selection summary ------------------------------------------------
    void Selection(Ctx* c, int& nSel, int& nAll, UINT64& bytes)
    {
        nSel = nAll = 0; bytes = 0;
        int rows = ListView_GetItemCount(c->hList);
        for (int r = 0; r < rows; ++r)
        {
            ++nAll;
            if (ListView_GetCheckState(c->hList, r))
            {
                ++nSel;
                bytes += c->info.files[c->rowToFile[r]].size;
            }
        }
    }

    void Layout(Ctx* c)
    {
        int L = c->S(kM), R = c->S(kW - kM);
        c->rcHead = { L, c->S(12), R, c->S(50) };
        c->rcCap  = { L, c->S(62), R, c->S(78) };
        c->rcList = { L, c->S(80), R, c->S(212) };
        MoveWindow(c->hList, c->rcList.left + 1, c->rcList.top + 1,
                   c->rcList.right - c->rcList.left - 2, c->rcList.bottom - c->rcList.top - 2, TRUE);
        int colSize = c->S(84);
        int inner = c->rcList.right - c->rcList.left - 2 - SysMetric(SM_CXVSCROLL, c->res.dpi) - c->S(2);
        ListView_SetColumnWidth(c->hList, 0, inner - colSize);
        ListView_SetColumnWidth(c->hList, 1, colSize);

        c->rcSum = { L, c->S(218), R, c->S(240) };
        int bh = c->S(22);
        MoveWindow(c->hNone, R - c->S(52), c->rcSum.top, c->S(52), bh, TRUE);
        MoveWindow(c->hAll,  R - c->S(52) - c->S(6) - c->S(74), c->rcSum.top, c->S(74), bh, TRUE);

        int fh = c->S(26);
        c->rcPath = { L + c->S(56), c->S(248), R - fh - c->S(6), c->S(248) + fh };
        MoveWindow(c->hBrowse, R - fh, c->rcPath.top, fh, fh, TRUE);
        PlaceEdit(c->hPath, c->rcPath, c->res.ui, c->S(8));

        c->rcFooter = { 0, c->S(kFooterY), c->S(kW), c->S(kH) };
        int bb = c->S(28), by = c->rcFooter.top + (c->S(kH - kFooterY) - bb) / 2;
        int x = R, wS = c->S(124), wL = c->S(116), wC = c->S(76), gap = c->S(8);
        MoveWindow(c->hStart,  x - wS, by, wS, bb, TRUE); x -= wS + gap;
        MoveWindow(c->hLater,  x - wL, by, wL, bb, TRUE); x -= wL + gap;
        MoveWindow(c->hCancel, x - wC, by, wC, bb, TRUE);

        HWND btns[] = { c->hBrowse, c->hStart, c->hLater, c->hCancel, c->hAll, c->hNone };
        for (HWND b : btns) SendMessageW(b, WM_SETFONT, (WPARAM)c->res.bold, FALSE);
        SendMessageW(c->hList, WM_SETFONT, (WPARAM)c->res.ui, TRUE);
    }

    void FillList(Ctx* c)
    {
        ++c->filling;
        SendMessageW(c->hList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(c->hList);
        c->rowToFile.clear();
        if (c->haveInfo)
        {
            int row = 0;
            for (size_t i = 0; i < c->info.files.size(); ++i)
            {
                const NdmTorrentFile& f = c->info.files[i];
                if (f.padding) continue;
                std::wstring nm = f.path;
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = row;
                it.pszText = (LPWSTR)nm.c_str();
                ListView_InsertItem(c->hList, &it);
                std::wstring sz = NdmFormatSize(f.size);
                ListView_SetItemText(c->hList, row, 1, (LPWSTR)sz.c_str());
                ListView_SetCheckState(c->hList, row, TRUE);
                c->rowToFile.push_back((int)i);
                ++row;
            }
        }
        SendMessageW(c->hList, WM_SETREDRAW, TRUE, 0);
        --c->filling;
        ShowWindow(c->hList, c->haveInfo ? SW_SHOWNA : SW_HIDE);   // hint text is painted underneath
        EnableWindow(c->hAll, c->haveInfo);
        EnableWindow(c->hNone, c->haveInfo);
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
            const NdmDlgRes& r = c->res;
            Fill(dc, cl, kBg);

            // header
            int ts = c->S(36);
            RECT tile = { c->rcHead.left, c->rcHead.top + c->S(1), c->rcHead.left + ts, c->rcHead.top + c->S(1) + ts };
            GlyphTile(dc, tile, GLY_DOWNARROW, r.dpi);
            int tx = tile.right + c->S(10);
            RECT nr = { tx, c->rcHead.top, c->rcHead.right, c->rcHead.top + c->S(20) };
            std::wstring name = c->haveInfo ? c->info.name : (!c->name.empty() ? c->name : L"Torrent");
            Text(dc, name, nr, NDM_CLR_FG, r.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT sr = { tx, c->rcHead.top + c->S(20), c->rcHead.right, c->rcHead.bottom - c->S(2) };
            if (!c->error.empty())
                Text(dc, c->error, sr, RGB(236, 110, 110), r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            else
            {
                std::wstring sub;
                if (c->haveInfo)
                {
                    int n = (int)c->rowToFile.size();
                    sub = NdmFormatSize(c->info.totalSize) + L"  ·  " + std::to_wstring(n) +
                          (n == 1 ? L" file" : L" files");
                    sub += c->magnet ? L"  ·  Magnet" : L"  ·  .torrent";
                }
                else sub = c->status;
                Text(dc, sub, sr, c->fetching ? NDM_CLR_GOLD : NDM_CLR_FG_DIM, r.ui,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            GoldRule(dc, c->rcHead.left, c->rcHead.right, c->S(56));

            // list caption + frame
            Caption(dc, L"FILES", c->rcCap, r);
            RECT sc = c->rcCap; sc.right -= c->S(22);
            int old = SetTextCharacterExtra(dc, r.S(1));
            Text(dc, L"SIZE", sc, NDM_CLR_FG_DIM, r.caption, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
            SetTextCharacterExtra(dc, old);
            Fill(dc, c->rcList, kListBg);
            RoundFrame(dc, c->rcList, c->S(4), NDM_CLR_BORDER, 1.0f);
            if (!c->haveInfo)
            {
                RECT pr = c->rcList;
                std::wstring t = c->fetching ? L"Fetching metadata from peers…\nYou can start now; all files will be downloaded."
                                             : L"File list unavailable — all files will be downloaded.";
                RECT tr = pr; InflateRect(&tr, -c->S(16), 0);
                RECT meas = tr;
                DrawTextW(dc, t.c_str(), -1, &meas, DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
                int hgt = meas.bottom - meas.top;
                tr.top = (pr.top + pr.bottom - hgt) / 2; tr.bottom = tr.top + hgt + c->S(4);
                Text(dc, t, tr, NDM_CLR_FG_DIM, r.ui, DT_CENTER | DT_WORDBREAK);
            }

            // summary
            std::wstring sum;
            if (c->haveInfo)
            {
                int nSel, nAll; UINT64 b;
                Selection(c, nSel, nAll, b);
                sum = std::to_wstring(nSel) + L" of " + std::to_wstring(nAll) + L" selected  ·  " + NdmFormatSize(b);
            }
            RECT smr = c->rcSum; smr.right -= c->S(140);
            Text(dc, sum, smr, NDM_CLR_FG_DIM, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // save folder
            RECT lr = { c->S(kM), c->rcPath.top, c->rcPath.left - c->S(8), c->rcPath.bottom };
            Text(dc, L"Save to", lr, NDM_CLR_FG_DIM, r.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawField(dc, c->rcPath, GetFocus() == c->hPath, false, r.dpi);

            Fill(dc, c->rcFooter, kFootBg);
            HLine(dc, 0, cl.right, c->rcFooter.top, RGB(40, 44, 53));
        }
        EndPaint(c->hwnd, &ps);
    }

    DWORD WINAPI MetaThread(LPVOID p)
    {
        std::shared_ptr<MetaJob>* sp = (std::shared_ptr<MetaJob>*)p;
        std::shared_ptr<MetaJob> job = *sp;
        delete sp;
        if (job->magnet)
            job->ok = NdmTorrentFetchMetadata(job->src, job->cancel, job->bytes, job->err);
        else
            job->ok = NdmTorrentLoadSource(job->src, job->bytes, job->err, job->cancel);
        HWND h = job->hwnd.load();
        if (h && WaitForSingleObject(job->cancel, 0) != WAIT_OBJECT_0)
            PostMessageW(h, WM_TOR_META, 0, 0);
        return 0;
    }

    void StartFetch(Ctx* c)
    {
        auto job = std::make_shared<MetaJob>();
        job->src = c->src;
        job->magnet = c->magnet;
        job->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        job->hwnd = c->hwnd;
        c->job = job;
        c->fetching = true;
        c->status = c->magnet ? L"Fetching metadata…" : L"Downloading .torrent file…";
        auto* arg = new std::shared_ptr<MetaJob>(job);
        HANDLE th = CreateThread(nullptr, 0, MetaThread, arg, 0, nullptr);
        if (th) CloseHandle(th);
        else { delete arg; c->fetching = false; c->status = L"Metadata not available"; }
    }

    void CancelFetch(Ctx* c)
    {
        if (c->job)
        {
            c->job->hwnd = nullptr;
            SetEvent(c->job->cancel);
            c->job.reset();
        }
        c->fetching = false;
    }

    void SetInfo(Ctx* c, const std::string& bytes)
    {
        NdmTorrentInfo ti;
        if (!NdmTorrentParse(bytes, ti))
        {
            c->error = L"The torrent metadata is invalid.";
            return;
        }
        c->info = ti;
        c->haveInfo = true;
        c->hash = ti.infoHash;
        NdmTorrentCachePut(ti.infoHash, bytes);     // the download starts from the cache
        FillList(c);
    }

    void Commit(Ctx* c, bool startNow)
    {
        std::wstring folder = Trim(GetText(c->hPath));
        if (folder.empty() || !EnsureFolder(folder))
        {
            c->error = L"The folder cannot be created. Choose another location.";
            InvalidateRect(c->hwnd, nullptr, FALSE);
            SetFocus(c->hPath);
            return;
        }
        std::wstring files;
        if (c->haveInfo)
        {
            int nSel, nAll; UINT64 b;
            Selection(c, nSel, nAll, b);
            if (nSel == 0)
            {
                c->error = L"Select at least one file.";
                InvalidateRect(c->hwnd, nullptr, FALSE);
                return;
            }
            if (nSel < nAll)
            {
                int rows = ListView_GetItemCount(c->hList);
                for (int r = 0; r < rows; ++r)
                    if (ListView_GetCheckState(c->hList, r))
                    {
                        if (!files.empty()) files += L",";
                        files += std::to_wstring(c->rowToFile[r] + 1);
                    }
            }
        }
        CancelFetch(c);

        NdmAddParams p;
        p.url          = c->src;
        p.savePath     = folder;
        p.filename     = c->haveInfo ? c->info.name
                       : !c->name.empty() ? c->name
                       : L"Torrent " + std::wstring(c->hash.begin(), c->hash.begin() + (c->hash.size() >= 8 ? 8 : c->hash.size()));
        p.connections  = NdmConfigMgr::GetInstance().GetMaxConnections();
        p.startNow     = startNow;
        p.kind         = NDM_KIND_TORRENT;
        p.torrentFiles = files;
        DWORD id = NdmQueue::GetInstance().AddDownloadEx(p);
        if (!id)
        {
            c->error = L"The download could not be added.";
            InvalidateRect(c->hwnd, nullptr, FALSE);
            return;
        }
        c->added = true;
        c->done = true;
        HWND owner = c->owner;
        if (owner) EnableWindow(owner, TRUE);
        DestroyWindow(c->hwnd);
        if (startNow) NdmProgressDlg::Show(owner, id);
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
            c->hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                       LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                                       0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
            ListView_SetExtendedListViewStyle(c->hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            DarkScrollbars(c->hList);
            ListView_SetBkColor(c->hList, kListBg);
            ListView_SetTextBkColor(c->hList, kListBg);
            ListView_SetTextColor(c->hList, NDM_CLR_FG);
            LVCOLUMNW col = {};
            col.mask = LVCF_WIDTH | LVCF_FMT;
            col.fmt = LVCFMT_LEFT;  col.cx = 300; ListView_InsertColumn(c->hList, 0, &col);
            col.fmt = LVCFMT_RIGHT; col.cx = 80;  ListView_InsertColumn(c->hList, 1, &col);
            BuildChecks(c);
            c->hPath   = MakeEdit(h, IDC_PATH);
            c->hBrowse = MakeBtn(h, IDC_BROWSE, L"", GLY_FOLDER);
            c->hAll    = MakeBtn(h, IDC_ALL, L"Select all");
            c->hNone   = MakeBtn(h, IDC_NONE, L"None");
            c->hStart  = MakeBtn(h, IDC_START, L"Start Download");
            c->hLater  = MakeBtn(h, IDC_LATER, L"Download Later");
            c->hCancel = MakeBtn(h, IDC_CANCEL_BTN, L"Cancel");
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
            case IDC_START:      k = BTN_PRIMARY; break;
            case IDC_LATER:      k = BTN_SECONDARY; break;
            case IDC_CANCEL_BTN: k = BTN_GHOST; break;
            case IDC_BROWSE:     k = BTN_STEPPER; bg = kBg; break;
            case IDC_ALL: case IDC_NONE: k = BTN_GHOST; bg = kBg; break;
            }
            DrawButton(di, k, bg, c->res.bold, c->res.dpi);
            return TRUE;
        }
        case WM_NOTIFY:
        {
            NMHDR* nh = (NMHDR*)l;
            if (nh->idFrom == IDC_LIST)
            {
                if (nh->code == LVN_ITEMCHANGED && !c->filling)
                {
                    NMLISTVIEW* nv = (NMLISTVIEW*)l;
                    if ((nv->uChanged & LVIF_STATE) && ((nv->uNewState ^ nv->uOldState) & LVIS_STATEIMAGEMASK))
                    {
                        c->error.clear();
                        InvalidateRect(h, &c->rcSum, FALSE);
                        InvalidateRect(h, &c->rcHead, FALSE);
                    }
                }
                else if (nh->code == NM_DBLCLK)
                {
                    NMITEMACTIVATE* ia = (NMITEMACTIVATE*)l;
                    if (ia->iItem >= 0)
                        ListView_SetCheckState(c->hList, ia->iItem, !ListView_GetCheckState(c->hList, ia->iItem));
                }
                else if (nh->code == NM_CUSTOMDRAW)
                {
                    NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)l;
                    switch (cd->nmcd.dwDrawStage)
                    {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT:
                        return CDRF_NOTIFYSUBITEMDRAW;
                    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                    {
                        bool sel = ListView_GetItemState(c->hList, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) != 0;
                        bool on  = ListView_GetCheckState(c->hList, (int)cd->nmcd.dwItemSpec) != 0;
                        cd->clrTextBk = sel ? NDM_CLR_SEL_BG : kListBg;
                        cd->clrText = sel ? NDM_CLR_SEL_FG
                                    : !on ? NDM_CLR_FG_DIM
                                    : cd->iSubItem == 1 ? NDM_CLR_FG_DIM : NDM_CLR_FG;
                        // the control paints the highlight itself unless we clear the state
                        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                        return CDRF_NEWFONT;
                    }
                    }
                    return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_TOR_META:
        {
            std::shared_ptr<MetaJob> job = c->job;
            c->fetching = false;
            if (!job) return 0;
            c->job.reset();
            if (job->ok) SetInfo(c, job->bytes);
            else
            {
                c->status = L"Metadata not available";
                c->error = job->err;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id)
            {
            case IDC_ALL:
            case IDC_NONE:
            {
                int rows = ListView_GetItemCount(c->hList);
                for (int r = 0; r < rows; ++r) ListView_SetCheckState(c->hList, r, id == IDC_ALL);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            case IDC_BROWSE:
            {
                std::wstring d = BrowseFolder(h, Trim(GetText(c->hPath)), L"Choose where to save the torrent");
                if (!d.empty()) SetWindowTextW(c->hPath, d.c_str());
                return 0;
            }
            case IDC_PATH:
                if (code == EN_CHANGE) { c->error.clear(); InvalidateRect(h, &c->rcHead, FALSE); }
                break;
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
            SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildChecks(c);
            Layout(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            CancelFetch(c);
            c->done = true;
            if (c->owner && IsWindow(c->owner))
            {
                EnableWindow(c->owner, TRUE);
                if (IsWindowVisible(c->owner)) SetForegroundWindow(c->owner);
            }
            return 0;
        case WM_NCDESTROY:
            if (c)
            {
                if (c->hChecks) { ImageList_Destroy(c->hChecks); c->hChecks = nullptr; }
                c->res.Free();
                c->hwnd = nullptr;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

// ===========================================================================
// Public API
// ===========================================================================
bool NdmTorrentShowAddDialog(HWND owner, const std::wstring& magnetOrTorrent)
{
    std::wstring src = Trim(magnetOrTorrent);
    if (src.empty()) return false;

    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg)
    {
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&ic);
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        reg = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    std::unique_ptr<Ctx> c(new Ctx());
    c->owner  = owner && IsWindow(owner) ? owner : nullptr;
    c->src    = src;
    c->magnet = _wcsnicmp(src.c_str(), L"magnet:", 7) == 0;

    // local .torrent: parse right away; magnet / remote .torrent: worker thread
    std::string bytes;
    std::wstring err;
    bool remote = _wcsnicmp(src.c_str(), L"http://", 7) == 0 || _wcsnicmp(src.c_str(), L"https://", 8) == 0;
    if (c->magnet)
    {
        c->hash = NdmTorrentMagnetHash(src);
        c->name = NdmTorrentMagnetName(src);
        if (c->hash.empty())
        {
            MessageBoxW(c->owner, L"This magnet link has no BitTorrent info hash.", L"Add Torrent", MB_ICONWARNING);
            return false;
        }
    }
    else if (!remote)
    {
        if (!NdmTorrentLoadSource(src, bytes, err))
        {
            MessageBoxW(c->owner, err.c_str(), L"Add Torrent", MB_ICONWARNING);
            return false;
        }
        NdmTorrentInfo ti;
        if (!NdmTorrentParse(bytes, ti))
        {
            MessageBoxW(c->owner, L"This is not a valid .torrent file.", L"Add Torrent", MB_ICONWARNING);
            return false;
        }
    }

    POINT pt = CreationPoint(c->owner);
    HWND h = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, kClass, L"Add Torrent",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             pt.x, pt.y, 100, 100, c->owner, nullptr, hi, c.get());
    if (!h) return false;
    PlaceWindow(h, kW, kH, c->owner);

    std::wstring folder = NdmConfigMgr::GetInstance().GetSavePath();
    if (folder.empty()) folder = NdmConfigMgr::GetDefaultSavePath();
    SetWindowTextW(c->hPath, folder.c_str());

    if (!bytes.empty()) SetInfo(c.get(), bytes);
    else
    {
        // instant fill when the metadata is already cached
        std::string cached;
        if (c->magnet && NdmTorrentCacheGet(c->hash, cached)) SetInfo(c.get(), cached);
        else StartFetch(c.get());
        FillList(c.get());
    }

    if (c->owner) EnableWindow(c->owner, FALSE);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
    SetFocus(c->hStart);

    // modal loop
    MSG msg;
    bool quit = false;
    WPARAM quitCode = 0;
    while (!c->done && IsWindow(h))
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) { quit = r == 0; quitCode = msg.wParam; break; }
        if (IsDialogMessageW(h, &msg)) continue;
        if (PreTranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(h)) DestroyWindow(h);
    if (c->owner) EnableWindow(c->owner, TRUE);
    if (quit) PostQuitMessage((int)quitCode);
    return c->added;
}
