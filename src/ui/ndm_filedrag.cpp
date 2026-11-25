// ndm_filedrag.cpp -- drag finished files out of NDM
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
//
// The drag uses the shell's own data object (SHCreateDataObject over the
// files' ID lists) and SHDoDragDrop, so every target sees exactly what an
// Explorer drag gives it: CF_HDROP, shell ID lists, file contents for apps
// that want them, and the standard drag image with "Move to" / "Copy to".
//
// Move vs copy: a plain drag offers MOVE | COPY with "Preferred DropEffect"
// = MOVE (Explorer moves, even to another drive; apps that only read the
// file copy). Shift held when the drag starts offers COPY only.
//
// Following a moved file: a hidden window listens to the shell's change
// notifications from just before the drag until the file is found again (or
// 15 minutes pass). A rename of the old path (same drive) gives the new path
// at once; across drives the new file shows up as a create event with the
// same name and, once the old one is gone, the same size.

#include "ndm_filedrag.h"
#include "ndm_mainwnd.h"
#include "../core/ndm_queue.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace
{
    bool g_active = false;

    const UINT  WM_SHNOTIFY = WM_APP + 0x41;
    const UINT  kPollTimer  = 1;
    const DWORD kWatchMs    = 15 * 60 * 1000;

    struct Watch
    {
        DWORD        id = 0;
        std::wstring path, name;
        UINT64       size = 0;
        bool         dir = false;
        ULONGLONG    until = 0;
        std::vector<std::wstring> created;     // same-name files that appeared
    };

    std::vector<Watch> g_watch;
    HWND  g_wnd = nullptr;
    ULONG g_reg = 0;

    std::wstring Lower(std::wstring s)
    {
        for (auto& ch : s) ch = (wchar_t)towlower(ch);
        return s;
    }

    std::wstring NameOf(const std::wstring& p)
    {
        size_t k = p.find_last_of(L"\\/");
        return k == std::wstring::npos ? p : p.substr(k + 1);
    }

    std::wstring DirOf(const std::wstring& p)
    {
        size_t k = p.find_last_of(L"\\/");
        return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
    }

    bool Exists(const std::wstring& p, bool* isDir = nullptr, UINT64* size = nullptr)
    {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return false;
        if (isDir) *isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (size) *size = ((UINT64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        return true;
    }

    // ---------------------------------------------------------------------
    // Following moved files
    // ---------------------------------------------------------------------
    void StopWatching()
    {
        if (g_reg) { SHChangeNotifyDeregister(g_reg); g_reg = 0; }
        if (g_wnd) KillTimer(g_wnd, kPollTimer);
    }

    void Relocate(const Watch& w, const std::wstring& newPath)
    {
        if (w.id && NdmQueue::GetInstance().RelocateCompleted(w.id, newPath))
            NdmMainWindow::GetInstance().RefreshListView();
    }

    // Settles what can be settled now; drops finished / expired watches.
    void Check()
    {
        ULONGLONG now = GetTickCount64();
        for (size_t i = 0; i < g_watch.size();)
        {
            Watch& w = g_watch[i];
            bool done = now > w.until;
            if (!done && !Exists(w.path))
            {
                for (const std::wstring& c : w.created)
                {
                    bool dir = false; UINT64 sz = 0;
                    if (!Exists(c, &dir, &sz) || dir != w.dir || (!w.dir && sz != w.size)) continue;
                    Relocate(w, c);
                    done = true;
                    break;
                }
            }
            if (done) g_watch.erase(g_watch.begin() + (ptrdiff_t)i);
            else ++i;
        }
        if (g_watch.empty()) StopWatching();
    }

    std::wstring PathOf(PCIDLIST_ABSOLUTE pidl)
    {
        wchar_t buf[MAX_PATH * 2] = {};
        if (!pidl || !SHGetPathFromIDListEx(pidl, buf, (DWORD)_countof(buf), GPFIDL_DEFAULT)) return {};
        return buf;
    }

    void OnShellEvent(LONG ev, PIDLIST_ABSOLUTE* pidls)
    {
        const std::wstring a = PathOf(pidls ? pidls[0] : nullptr);
        const std::wstring b = PathOf(pidls ? pidls[1] : nullptr);
        if (ev & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER))
        {
            for (size_t i = 0; i < g_watch.size(); ++i)
            {
                Watch& w = g_watch[i];
                if (_wcsicmp(a.c_str(), w.path.c_str()) != 0) continue;
                if (NdmFileDrag::AcceptNewPlace(w.path, b, true) && Exists(b))
                    Relocate(w, b);
                g_watch.erase(g_watch.begin() + (ptrdiff_t)i);   // renamed: settled either way
                break;
            }
        }
        if (ev & (SHCNE_CREATE | SHCNE_MKDIR))
            for (Watch& w : g_watch)
                if (NdmFileDrag::AcceptNewPlace(w.path, a, false) &&
                    std::find(w.created.begin(), w.created.end(), a) == w.created.end() &&
                    w.created.size() < 16)
                    w.created.push_back(a);
        Check();
    }

    LRESULT CALLBACK WatchProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        if (m == WM_SHNOTIFY)
        {
            PIDLIST_ABSOLUTE* pidls = nullptr;
            LONG ev = 0;
            if (HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &ev))
            {
                OnShellEvent(ev, pidls);
                SHChangeNotification_Unlock(lock);
            }
            return 0;
        }
        if (m == WM_TIMER && wp == kPollTimer) { Check(); return 0; }
        return DefWindowProcW(h, m, wp, lp);
    }

    bool StartWatching()
    {
        if (!g_wnd)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc   = WatchProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"NDMDragWatch";
            RegisterClassExW(&wc);
            g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            if (!g_wnd) return false;
        }
        if (!g_reg)
        {
            PIDLIST_ABSOLUTE desk = nullptr;
            if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desk))) return false;
            SHChangeNotifyEntry e = { desk, TRUE };
            g_reg = SHChangeNotifyRegister(g_wnd, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
                                           SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_CREATE |
                                           SHCNE_MKDIR | SHCNE_DELETE | SHCNE_RMDIR,
                                           WM_SHNOTIFY, 1, &e);
            CoTaskMemFree(desk);
            if (!g_reg) return false;
        }
        SetTimer(g_wnd, kPollTimer, 2000, nullptr);
        return true;
    }

    // ---------------------------------------------------------------------
    // Drag image: the file's thumbnail / icon, as Explorer shows it
    // ---------------------------------------------------------------------
    void SetDragImage(IDataObject* obj, const std::wstring& path, HWND owner)
    {
        IDragSourceHelper* helper = nullptr;
        if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&helper))))
            return;
        IShellItemImageFactory* f = nullptr;
        HBITMAP bmp = nullptr;
        UINT dpi = GetDpiForWindow(owner);
        int px = MulDiv(96, dpi ? (int)dpi : 96, 96);
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))))
        {
            f->GetImage({ px, px }, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &bmp);
            f->Release();
        }
        if (bmp)
        {
            BITMAP bm = {};
            GetObjectW(bmp, sizeof(bm), &bm);
            SHDRAGIMAGE di = {};
            di.sizeDragImage = { bm.bmWidth, bm.bmHeight };
            di.ptOffset      = { bm.bmWidth / 2, bm.bmHeight - MulDiv(8, px, 96) };
            di.hbmpDragImage = bmp;
            di.crColorKey    = CLR_NONE;
            if (FAILED(helper->InitializeFromBitmap(&di, obj)))
                DeleteObject(bmp);                  // owned by the helper on success
        }
        helper->Release();
    }

    // Drop source. In copy mode (Shift at the start) the drop target must not
    // see Shift: Explorer reads Shift as "move", and with only copy allowed it
    // would refuse the drop. OLE builds the key state it hands the target
    // from this thread's keyboard state, so Shift is cleared there before
    // every step of the drag.
    class DropSource : public IDropSource
    {
    public:
        explicit DropSource(bool copyMode) : m_copy(copyMode) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = this; AddRef(); return S_OK; }
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
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override
        {
            if (esc || (keys & MK_RBUTTON)) return DRAGDROP_S_CANCEL;
            if (m_copy) HideShift();
            if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
        {
            if (m_copy) HideShift();
            return DRAGDROP_S_USEDEFAULTCURSORS;
        }
    private:
        static void HideShift()
        {
            BYTE ks[256];
            if (!GetKeyboardState(ks)) return;
            if (!((ks[VK_SHIFT] | ks[VK_LSHIFT] | ks[VK_RSHIFT]) & 0x80)) return;
            ks[VK_SHIFT] = ks[VK_LSHIFT] = ks[VK_RSHIFT] = 0;
            SetKeyboardState(ks);
        }
        LONG m_ref = 1;
        bool m_copy;
    };

    void SetPreferredEffect(IDataObject* obj, DWORD eff)
    {
        static const CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (!g) return;
        if (DWORD* p = (DWORD*)GlobalLock(g)) { *p = eff; GlobalUnlock(g); }
        FORMATETC fe = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm = {};
        sm.tymed = TYMED_HGLOBAL;
        sm.hGlobal = g;
        if (FAILED(obj->SetData(&fe, &sm, TRUE))) GlobalFree(g);
    }
}

namespace NdmFileDrag
{
    bool Active() { return g_active; }

    DWORD AllowedEffects(bool shiftAtStart)
    {
        return shiftAtStart ? DROPEFFECT_COPY : (DROPEFFECT_MOVE | DROPEFFECT_COPY);
    }

    DWORD PreferredEffect(bool shiftAtStart)
    {
        return shiftAtStart ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    }

    bool AcceptNewPlace(const std::wstring& oldPath, const std::wstring& newPath, bool renameOfOld)
    {
        if (newPath.empty() || _wcsicmp(oldPath.c_str(), newPath.c_str()) == 0) return false;
        if (Lower(newPath).find(L"\\$recycle.bin\\") != std::wstring::npos) return false;
        if (renameOfOld) return true;
        return _wcsicmp(NameOf(oldPath).c_str(), NameOf(newPath).c_str()) == 0;
    }

    bool Drag(HWND owner, const std::vector<Item>& itemsIn)
    {
        if (g_active || itemsIn.empty()) return false;

        // the files that exist and share the first one's folder
        std::vector<Item> items;
        const std::wstring folder = DirOf(itemsIn[0].path);
        for (const Item& it : itemsIn)
            if (!it.path.empty() && _wcsicmp(DirOf(it.path).c_str(), folder.c_str()) == 0 && Exists(it.path))
                items.push_back(it);
        if (items.empty() || folder.empty()) return false;

        PIDLIST_ABSOLUTE parent = nullptr;
        if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &parent, 0, nullptr))) return false;
        std::vector<PIDLIST_ABSOLUTE> full;
        std::vector<PCUITEMID_CHILD> kids;
        for (const Item& it : items)
        {
            PIDLIST_ABSOLUTE p = nullptr;
            if (FAILED(SHParseDisplayName(it.path.c_str(), nullptr, &p, 0, nullptr))) continue;
            full.push_back(p);
            kids.push_back(ILFindLastID(p));
        }
        IDataObject* obj = nullptr;
        HRESULT hr = kids.empty() ? E_FAIL
                   : SHCreateDataObject(parent, (UINT)kids.size(), kids.data(), nullptr, IID_PPV_ARGS(&obj));
        for (PIDLIST_ABSOLUTE p : full) CoTaskMemFree(p);
        CoTaskMemFree(parent);
        if (FAILED(hr) || !obj) return false;

        const bool copyOnly = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        SetPreferredEffect(obj, PreferredEffect(copyOnly));
        SetDragImage(obj, items[0].path, owner);

        // follow moves: listen from before the drop
        bool watching = false;
        if (!copyOnly && StartWatching())
        {
            watching = true;
            for (const Item& it : items)
            {
                if (!it.id) continue;
                g_watch.erase(std::remove_if(g_watch.begin(), g_watch.end(),
                              [&](const Watch& w) { return w.id == it.id; }), g_watch.end());
                Watch w;
                w.id = it.id;
                w.path = it.path;
                w.name = NameOf(it.path);
                Exists(it.path, &w.dir, &w.size);
                w.until = GetTickCount64() + kWatchMs;
                g_watch.push_back(w);
            }
        }

        g_active = true;
        DWORD eff = DROPEFFECT_NONE;
        DropSource* src = new DropSource(copyOnly);
        hr = SHDoDragDrop(owner, obj, src, AllowedEffects(copyOnly), &eff);
        src->Release();
        g_active = false;
        obj->Release();

        const bool dropped = hr == DRAGDROP_S_DROP;
        if (watching)
        {
            if (!dropped)
            {
                for (const Item& it : items)
                    g_watch.erase(std::remove_if(g_watch.begin(), g_watch.end(),
                                  [&](const Watch& w) { return w.id == it.id; }), g_watch.end());
                if (g_watch.empty()) StopWatching();
            }
            else
                Check();
        }
        return dropped;
    }
}
