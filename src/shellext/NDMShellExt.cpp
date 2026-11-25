#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <olectl.h>
#include <strsafe.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")

// Linker-provided symbol for this module's base address (HINSTANCE of the DLL)
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

// ---------------------------------------------------------------------------
// NDM window class (must match ndm_common.h)
// ---------------------------------------------------------------------------
#define NDM_WINDOW_CLASS    L"NDMMainClass"
#define WM_NDM_ADD_DOWNLOAD (WM_APP + 1)
// WM_COPYDATA dwData for command-line-origin input (must match
// NDM_COPYDATA_CMDLINE in src/ui/ndm_mainwnd.h): the app accepts a local
// .torrent path only from this origin.
#define NDM_COPYDATA_CMDLINE 0x4E444D43     // 'NDMC'

// ---------------------------------------------------------------------------
// Class GUID: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
// ---------------------------------------------------------------------------
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const GUID CLSID_NDMShellExt =
{ 0xA1B2C3D4, 0xE5F6, 0x7890,
  { 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90 } };

// ---------------------------------------------------------------------------
// URL validation: only "scheme:..." strings without quotes or control
// characters are passed on (the URL becomes one quoted command-line argument
// of NDMan.exe, or WM_COPYDATA text). A leading '-' or '/' could otherwise be
// taken for a switch; a '"' could break out of the quoted argument.
// ---------------------------------------------------------------------------
static const size_t kMaxUrlLen = 8192;

static bool IsAcceptableUrl(const std::wstring& u)
{
    if (u.empty() || u.size() > kMaxUrlLen) return false;
    for (wchar_t c : u)
        if (c < 0x20 || c == 0x7F || c == L'"' || (c >= 0x80 && c < 0xA0) ||
            c == 0x2028 || c == 0x2029)
            return false;
    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    if (!iswascii(u[0]) || !iswalpha(u[0])) return false;
    size_t i = 1;
    while (i < u.size() && iswascii(u[i]) && (iswalnum(u[i]) || u[i] == L'+' || u[i] == L'-' || u[i] == L'.')) i++;
    return i < u.size() && i >= 2 && u[i] == L':';
}

// One command-line argument for CommandLineToArgvW / the CRT parser. The URL
// contains no '"' (IsAcceptableUrl); backslashes in front of the closing
// quote must be doubled so they don't escape it.
static std::wstring QuoteArg(const std::wstring& s)
{
    std::wstring out = L"\"" + s;
    size_t bs = 0;
    for (size_t i = s.size(); i > 0 && s[i - 1] == L'\\'; i--) bs++;
    out.append(bs, L'\\');
    out += L'"';
    return out;
}

// A local .torrent file: drive-absolute "X:\..." path (what the app accepts as
// command-line input), no quotes / control characters, not a switch.
static bool IsAcceptableTorrentPath(const std::wstring& p)
{
    if (p.size() < 12 || p.size() >= 32768) return false;
    if (!iswascii(p[0]) || !iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\') return false;
    if (p.find(L"\\\\") != std::wstring::npos) return false;
    for (wchar_t c : p)
        if (c < 0x20 || c == 0x7F || c == L'"' || (c >= 0x80 && c < 0xA0) || c == 0x2028 || c == 0x2029)
            return false;
    return _wcsicmp(p.c_str() + p.size() - 8, L".torrent") == 0;
}

// HKLM\Software\Devnetra\NDM\InstallPath (any length, REG_SZ only).
static std::wstring ReadInstallPath()
{
    DWORD cb = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Devnetra\\NDM", L"InstallPath",
                     RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS ||
        cb < sizeof(wchar_t) || cb > 64 * 1024)
        return {};
    std::wstring s(cb / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Devnetra\\NDM", L"InstallPath",
                     RRF_RT_REG_SZ, nullptr, &s[0], &cb) != ERROR_SUCCESS)
        return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Module reference count
// ---------------------------------------------------------------------------
static LONG g_cDllRef = 0;

STDAPI_(ULONG) DllAddRef()   { return InterlockedIncrement(&g_cDllRef); }
STDAPI_(ULONG) DllRelease()  { return InterlockedDecrement(&g_cDllRef); }

// ---------------------------------------------------------------------------
// CNDMShellExt — implements IShellExtInit + IContextMenu
// ---------------------------------------------------------------------------
class CNDMShellExt : public IShellExtInit, public IContextMenu
{
public:
    CNDMShellExt() : m_cRef(1) { DllAddRef(); }
    ~CNDMShellExt()            { DllRelease(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IShellExtInit))
        {
            *ppv = static_cast<IShellExtInit*>(this);
        }
        else if (IsEqualIID(riid, IID_IContextMenu))
        {
            *ppv = static_cast<IContextMenu*>(this);
        }
        else return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) delete this;
        return cRef;
    }

    // -----------------------------------------------------------------------
    // IShellExtInit
    // -----------------------------------------------------------------------
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE /*pidlFolder*/,
                             IDataObject*        pDataObj,
                             HKEY                /*hRegKey*/) override
    {
        if (!pDataObj) return E_INVALIDARG;

        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stm = {};

        if (FAILED(pDataObj->GetData(&fmt, &stm)))
            return E_FAIL;

        HDROP hDrop = reinterpret_cast<HDROP>(GlobalLock(stm.hGlobal));
        if (!hDrop)
        {
            ReleaseStgMedium(&stm);
            return E_FAIL;
        }

        // We only act on the first selected file (any path length)
        UINT len = DragQueryFileW(hDrop, 0, nullptr, 0);
        if (len > 0 && len < 32768)
        {
            std::wstring buf(len + 1, L'\0');
            UINT got = DragQueryFileW(hDrop, 0, &buf[0], len + 1);
            buf.resize(got <= len ? got : len);
            m_selectedFile = buf;
        }

        GlobalUnlock(stm.hGlobal);
        ReleaseStgMedium(&stm);

        // For .url files, extract the URL= value
        if (!m_selectedFile.empty())
        {
            const std::wstring ext = PathFindExtensionW(m_selectedFile.c_str());
            if (_wcsicmp(ext.c_str(), L".url") == 0)
            {
                wchar_t urlBuf[2048] = {};
                GetPrivateProfileStringW(L"InternetShortcut", L"URL",
                                         L"", urlBuf, 2048,
                                         m_selectedFile.c_str());
                if (wcslen(urlBuf) > 0)
                    m_url = urlBuf;
            }
            else if (_wcsicmp(ext.c_str(), L".torrent") == 0)
            {
                // The plain local path, sent as command-line input (the app
                // only opens local .torrent files from that origin).
                if (IsAcceptableTorrentPath(m_selectedFile))
                    m_torrentPath = m_selectedFile;
            }
            // Anything with quotes / control characters / no scheme is dropped.
            if (!m_url.empty() && !IsAcceptableUrl(m_url)) m_url.clear();
        }

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // IContextMenu
    // -----------------------------------------------------------------------
    STDMETHODIMP QueryContextMenu(HMENU   hmenu,
                                   UINT    indexMenu,
                                   UINT    idCmdFirst,
                                   UINT    /*idCmdLast*/,
                                   UINT    uFlags) override
    {
        if (uFlags & CMF_DEFAULTONLY)
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        if (m_url.empty() && m_torrentPath.empty())   // nothing NDM can download
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

        InsertMenuW(hmenu, indexMenu, MF_BYPOSITION | MF_STRING,
                    idCmdFirst, L"Download with NDM");

        m_idCmdFirst = idCmdFirst;
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
    }

    STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* pci) override
    {
        if (!pci) return E_INVALIDARG;

        UINT idCmd = (IS_INTRESOURCE(pci->lpVerb))
                     ? (UINT)(UINT_PTR)pci->lpVerb
                     : UINT_MAX;

        if (idCmd != 0) return E_INVALIDARG; // only one item

        // Find the main NDM window
        HWND hNDM = FindWindowW(NDM_WINDOW_CLASS, nullptr);

        // .url -> the URL (external origin); .torrent -> the plain local path
        // (command-line origin).
        const bool isTorrent = m_url.empty() && !m_torrentPath.empty() && IsAcceptableTorrentPath(m_torrentPath);
        const std::wstring& item = isTorrent ? m_torrentPath : m_url;

        if (isTorrent || (!m_url.empty() && IsAcceptableUrl(m_url)))
        {
            if (hNDM)
            {
                // Send via COPYDATASTRUCT so we can pass the string
                COPYDATASTRUCT cds = {};
                cds.dwData = isTorrent ? NDM_COPYDATA_CMDLINE : WM_NDM_ADD_DOWNLOAD;
                cds.cbData = (DWORD)((item.size() + 1) * sizeof(wchar_t));
                cds.lpData = const_cast<wchar_t*>(item.c_str());

                SendMessageW(hNDM, WM_COPYDATA,
                             reinterpret_cast<WPARAM>(pci->hwnd),
                             reinterpret_cast<LPARAM>(&cds));
            }
            else
            {
                // NDM not running — launch it with the URL as argument
                std::wstring dir = ReadInstallPath();
                if (!dir.empty() && !PathIsRelativeW(dir.c_str()))
                {
                    std::wstring exe  = dir + L"\\NDMan.exe";
                    std::wstring args = QuoteArg(item);

                    SHELLEXECUTEINFOW sei = {};
                    sei.cbSize       = sizeof(sei);
                    sei.lpVerb       = L"open";
                    sei.lpFile       = exe.c_str();
                    sei.lpParameters = args.c_str();
                    sei.nShow        = SW_SHOWNORMAL;
                    ShellExecuteExW(&sei);
                }
            }
        }

        return S_OK;
    }

    STDMETHODIMP GetCommandString(UINT_PTR idCmd,
                                   UINT     uFlags,
                                   UINT*    /*reserved*/,
                                   PSTR     pszName,
                                   UINT     cchMax) override
    {
        if (idCmd != 0) return E_INVALIDARG;
        // cchMax is the buffer size in characters (WCHARs for the W forms).
        // StringCch* never write past it and always NUL-terminate; a short
        // buffer gives STRSAFE_E_INSUFFICIENT_BUFFER instead of the CRT
        // invalid-parameter handler (which would terminate Explorer).
        static const wchar_t kHelpW[] = L"Download with Nee Download Manager";
        static const char    kHelpA[] = "Download with Nee Download Manager";
        static const wchar_t kVerbW[] = L"NDMDownload";
        static const char    kVerbA[] = "NDMDownload";
        if (!pszName || cchMax == 0) return E_INVALIDARG;
        switch (uFlags)
        {
        case GCS_HELPTEXTW: return StringCchCopyW(reinterpret_cast<wchar_t*>(pszName), cchMax, kHelpW);
        case GCS_HELPTEXTA: return StringCchCopyA(pszName, cchMax, kHelpA);
        case GCS_VERBW:     return StringCchCopyW(reinterpret_cast<wchar_t*>(pszName), cchMax, kVerbW);
        case GCS_VERBA:     return StringCchCopyA(pszName, cchMax, kVerbA);
        case GCS_VALIDATEW:
        case GCS_VALIDATEA: return S_OK;
        default:            return E_NOTIMPL;
        }
    }

private:
    LONG         m_cRef       = 1;
    std::wstring m_selectedFile;
    std::wstring m_url;          // .url: the shortcut's URL
    std::wstring m_torrentPath;  // .torrent: the local path
    UINT         m_idCmdFirst = 0;
};

// ---------------------------------------------------------------------------
// Class factory
// ---------------------------------------------------------------------------
class CNDMClassFactory : public IClassFactory
{
public:
    CNDMClassFactory()  { DllAddRef(); }
    ~CNDMClassFactory() { DllRelease(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG c = InterlockedDecrement(&m_cRef);
        if (!c) delete this;
        return c;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter,
                                 REFIID     riid,
                                 void**     ppv) override
    {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CNDMShellExt* pExt = new (std::nothrow) CNDMShellExt();
        if (!pExt) return E_OUTOFMEMORY;
        HRESULT hr = pExt->QueryInterface(riid, ppv);
        pExt->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) DllAddRef();
        else       DllRelease();
        return S_OK;
    }

private:
    LONG m_cRef = 1;
};

// ---------------------------------------------------------------------------
// DLL entry points
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hInst);
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_NDMShellExt))
        return CLASS_E_CLASSNOTAVAILABLE;

    CNDMClassFactory* pFact = new (std::nothrow) CNDMClassFactory();
    if (!pFact) return E_OUTOFMEMORY;

    HRESULT hr = pFact->QueryInterface(riid, ppv);
    pFact->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

// ---------------------------------------------------------------------------
// DllRegisterServer / DllUnregisterServer
// ---------------------------------------------------------------------------

static HRESULT SetRegKeyValue(HKEY hRoot, const wchar_t* subKey,
                               const wchar_t* name, const wchar_t* value)
{
    HKEY hKey = nullptr;
    LONG lr = RegCreateKeyExW(hRoot, subKey, 0, nullptr,
                               REG_OPTION_NON_VOLATILE,
                               KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lr);

    lr = RegSetValueExW(hKey, name, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(value),
                         (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(lr);
}

STDAPI DllRegisterServer()
{
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath, MAX_PATH);

    // CLSID InprocServer32
    wchar_t clsidStr[64] = {};
    StringFromGUID2(CLSID_NDMShellExt, clsidStr, 64);

    std::wstring clsidKey = std::wstring(L"CLSID\\") + clsidStr;
    SetRegKeyValue(HKEY_CLASSES_ROOT, clsidKey.c_str(),
                   nullptr, L"NDM Shell Extension");

    std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    SetRegKeyValue(HKEY_CLASSES_ROOT, inprocKey.c_str(), nullptr, dllPath);
    SetRegKeyValue(HKEY_CLASSES_ROOT, inprocKey.c_str(),
                   L"ThreadingModel", L"Apartment");

    // .torrent and .url files only (same keys as NdmRegisterShellExt in
    // src/installer/ndm_integration.h); the old all-files ("*") handler goes.
    RegDeleteKeyW(HKEY_CLASSES_ROOT,
                  L"*\\shellex\\ContextMenuHandlers\\NDMShellExt");
    std::wstring torrentHandlerKey =
        L"SystemFileAssociations\\.torrent\\shellex\\ContextMenuHandlers\\NDMShellExt";
    SetRegKeyValue(HKEY_CLASSES_ROOT, torrentHandlerKey.c_str(), nullptr, clsidStr);

    // .url file handler
    std::wstring urlHandlerKey =
        L"SystemFileAssociations\\.url\\shellex\\ContextMenuHandlers\\NDMShellExt";
    SetRegKeyValue(HKEY_CLASSES_ROOT, urlHandlerKey.c_str(), nullptr, clsidStr);

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    wchar_t clsidStr[64] = {};
    StringFromGUID2(CLSID_NDMShellExt, clsidStr, 64);

    // Remove context menu handlers (incl. the old all-files one)
    RegDeleteKeyW(HKEY_CLASSES_ROOT,
                  L"*\\shellex\\ContextMenuHandlers\\NDMShellExt");
    RegDeleteKeyW(HKEY_CLASSES_ROOT,
                  L"SystemFileAssociations\\.torrent\\shellex\\ContextMenuHandlers\\NDMShellExt");
    RegDeleteKeyW(HKEY_CLASSES_ROOT,
                  L"SystemFileAssociations\\.url\\shellex\\ContextMenuHandlers\\NDMShellExt");

    // Remove CLSID entries
    std::wstring inprocKey =
        std::wstring(L"CLSID\\") + clsidStr + L"\\InprocServer32";
    RegDeleteKeyW(HKEY_CLASSES_ROOT, inprocKey.c_str());

    std::wstring clsidKey = std::wstring(L"CLSID\\") + clsidStr;
    RegDeleteKeyW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    return S_OK;
}

// Needed for DllRegisterServer to get own path
extern "C" IMAGE_DOS_HEADER __ImageBase;
