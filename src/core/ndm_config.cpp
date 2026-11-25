#include "ndm_config.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool EnsureDirRecursive(const std::wstring& dirIn)
{
    std::wstring d = dirIn;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (d.empty()) return false;
    DWORD a = GetFileAttributesW(d.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t p = d.find_last_of(L"\\/");
    if (p != std::wstring::npos && p > 0)
    {
        std::wstring parent = d.substr(0, p);
        if (!(parent.size() == 2 && parent[1] == L':')) EnsureDirRecursive(parent);
    }
    return CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
NdmConfigMgr& NdmConfigMgr::GetInstance()
{
    static NdmConfigMgr instance;
    return instance;
}

NdmConfigMgr::NdmConfigMgr()
{
    // Set sensible defaults
    m_cfg.defaultSavePath  = NdmConfigMgr::GetDefaultSavePath();
    m_cfg.maxConnections   = 8;
    m_cfg.maxDownloads     = 3;
    m_cfg.browserMonitor   = true;
    m_cfg.startWithWindows = false;
    m_cfg.closeToTray      = true;
    m_cfg.speedLimit       = 0;
}

// ---------------------------------------------------------------------------
// GetAppDataPath — %APPDATA%\Devnetra\NDM\  (falls back to the exe folder)
// ---------------------------------------------------------------------------
std::wstring NdmConfigMgr::GetAppDataPath()
{
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, buf)) && buf[0])
    {
        std::wstring path = buf;
        path += L"\\Devnetra\\NDM\\";
        return path;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L".\\" : p.substr(0, s + 1);
}

// ---------------------------------------------------------------------------
// GetDefaultSavePath — the user's Downloads folder (FOLDERID_Downloads)
// ---------------------------------------------------------------------------
std::wstring NdmConfigMgr::GetDefaultSavePath()
{
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT,
                                       nullptr, &known)) && known && known[0])
    {
        std::wstring path = known;
        CoTaskMemFree(known);
        if (path.back() != L'\\') path += L'\\';
        return path;
    }
    if (known) CoTaskMemFree(known);

    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr,
                                   SHGFP_TYPE_CURRENT, buf)) && buf[0])
    {
        std::wstring path = buf;
        path += L"\\Downloads\\";
        return path;
    }
    return L"C:\\Downloads\\";
}

// ---------------------------------------------------------------------------
// Load — read settings from ndm.ini
// ---------------------------------------------------------------------------
void NdmConfigMgr::Load()
{
    std::wstring appDir  = GetAppDataPath();
    std::wstring iniPath = appDir + L"ndm.ini";

    // Ensure %APPDATA%\Devnetra\NDM exists (CreateDirectory is not recursive)
    EnsureDirRecursive(appDir);

    auto ReadStr = [&](const wchar_t* section,
                       const wchar_t* key,
                       const wchar_t* def) -> std::wstring
    {
        wchar_t buf[2048] = {};
        GetPrivateProfileStringW(section, key, def, buf, 2048, iniPath.c_str());
        return std::wstring(buf);
    };

    auto ReadInt = [&](const wchar_t* section,
                       const wchar_t* key,
                       int            def) -> int
    {
        return (int)GetPrivateProfileIntW(section, key, def, iniPath.c_str());
    };

    // [General]
    std::wstring sp = ReadStr(L"General", L"SavePath", m_cfg.defaultSavePath.c_str());
    while (!sp.empty() && (sp.back() == L' ')) sp.pop_back();
    m_cfg.defaultSavePath = sp.empty() ? GetDefaultSavePath() : sp;

    m_cfg.maxDownloads     = Clamp(ReadInt(L"General", L"MaxDownloads", m_cfg.maxDownloads), 1, 32);
    m_cfg.startWithWindows = ReadInt(L"General", L"StartWithWindows", 0) != 0;
    m_cfg.closeToTray      = ReadInt(L"General", L"CloseToTray",      1) != 0;

    // [Connection]
    m_cfg.maxConnections = Clamp(ReadInt(L"Connection", L"MaxConnections", m_cfg.maxConnections), 1, 32);
    std::wstring sl = ReadStr(L"Connection", L"SpeedLimit", L"0");
    m_cfg.speedLimit = (DWORD)wcstoul(sl.c_str(), nullptr, 10);   // bytes/sec, 0 = unlimited
    m_cfg.retryCount    = Clamp(ReadInt(L"Connection", L"RetryCount", m_cfg.retryCount), 0, 1000);
    m_cfg.retryMaxDelay = Clamp(ReadInt(L"Connection", L"RetryMaxDelay", m_cfg.retryMaxDelay), 1, 3600);

    // [Browser]
    m_cfg.browserMonitor = ReadInt(L"Browser", L"Monitor", 1) != 0;

    // [Scheduler]
    NdmSchedule& s = m_cfg.schedule;
    s.startEnabled = ReadInt(L"Scheduler", L"StartEnabled", 0) != 0;
    s.stopEnabled  = ReadInt(L"Scheduler", L"StopEnabled",  0) != 0;
    s.startMinute  = Clamp(ReadInt(L"Scheduler", L"StartMinute", 0), 0, 1439);
    s.stopMinute   = Clamp(ReadInt(L"Scheduler", L"StopMinute",  0), 0, 1439);
    s.once         = ReadInt(L"Scheduler", L"Once", 0) != 0;
    s.dayMask      = ReadInt(L"Scheduler", L"Days", 0x7F) & 0x7F;
    s.onceDate     = ReadInt(L"Scheduler", L"OnceDate", 0);

    // [UI]
    m_ui.Load(iniPath);
}

// ---------------------------------------------------------------------------
// [UI] section -- main-window state
// ---------------------------------------------------------------------------
void NdmUiState::Load(const std::wstring& iniPath)
{
    auto ReadStr = [&](const wchar_t* key, const wchar_t* def) -> std::wstring
    {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(L"UI", key, def, buf, 512, iniPath.c_str());
        return std::wstring(buf);
    };
    columnWidths = ReadStr(L"ColumnWidths", L"");
    // GetPrivateProfileInt cannot return negative values: -1 = queue order.
    std::wstring sc = ReadStr(L"SortColumn", L"-1");
    wchar_t* end = nullptr;
    long v = wcstol(sc.c_str(), &end, 10);
    sortColumn    = (end && end != sc.c_str() && v >= -1 && v < 64) ? (int)v : -1;
    sortAscending = GetPrivateProfileIntW(L"UI", L"SortAscending", 1, iniPath.c_str()) != 0;
    trayHintShown = GetPrivateProfileIntW(L"UI", L"TrayHintShown", 0, iniPath.c_str()) != 0;
}

void NdmUiState::Save(const std::wstring& iniPath) const
{
    const wchar_t* p = iniPath.c_str();
    WritePrivateProfileStringW(L"UI", L"ColumnWidths",  columnWidths.c_str(), p);
    WritePrivateProfileStringW(L"UI", L"SortColumn",    std::to_wstring(sortColumn).c_str(), p);
    WritePrivateProfileStringW(L"UI", L"SortAscending", sortAscending ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"UI", L"TrayHintShown", trayHintShown ? L"1" : L"0", p);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, p);     // flush
}

void NdmConfigMgr::SaveUi()
{
    EnsureDirRecursive(GetAppDataPath());
    m_ui.Save(GetIniPath());
}

// ---------------------------------------------------------------------------
// Save — write settings to ndm.ini
// ---------------------------------------------------------------------------
void NdmConfigMgr::Save()
{
    std::wstring appDir  = GetAppDataPath();
    std::wstring iniPath = appDir + L"ndm.ini";

    EnsureDirRecursive(appDir);

    auto WriteStr = [&](const wchar_t* section,
                        const wchar_t* key,
                        const std::wstring& val)
    {
        WritePrivateProfileStringW(section, key, val.c_str(), iniPath.c_str());
    };

    auto WriteUInt = [&](const wchar_t* section,
                         const wchar_t* key,
                         unsigned long val)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%lu", val);
        WritePrivateProfileStringW(section, key, buf, iniPath.c_str());
    };

    // [General]
    WriteStr (L"General", L"SavePath",          m_cfg.defaultSavePath);
    WriteUInt(L"General", L"MaxDownloads",      (unsigned long)Clamp(m_cfg.maxDownloads, 1, 32));
    WriteUInt(L"General", L"StartWithWindows",  m_cfg.startWithWindows ? 1 : 0);
    WriteUInt(L"General", L"CloseToTray",       m_cfg.closeToTray      ? 1 : 0);

    // [Connection]
    WriteUInt(L"Connection", L"MaxConnections", (unsigned long)Clamp(m_cfg.maxConnections, 1, 32));
    WriteUInt(L"Connection", L"SpeedLimit",     (unsigned long)m_cfg.speedLimit);
    WriteUInt(L"Connection", L"RetryCount",     (unsigned long)Clamp(m_cfg.retryCount, 0, 1000));
    WriteUInt(L"Connection", L"RetryMaxDelay",  (unsigned long)Clamp(m_cfg.retryMaxDelay, 1, 3600));

    // [Browser]
    WriteUInt(L"Browser", L"Monitor", m_cfg.browserMonitor ? 1 : 0);

    // [Scheduler]
    const NdmSchedule& s = m_cfg.schedule;
    WriteUInt(L"Scheduler", L"StartEnabled", s.startEnabled ? 1 : 0);
    WriteUInt(L"Scheduler", L"StopEnabled",  s.stopEnabled ? 1 : 0);
    WriteUInt(L"Scheduler", L"StartMinute",  (unsigned long)Clamp(s.startMinute, 0, 1439));
    WriteUInt(L"Scheduler", L"StopMinute",   (unsigned long)Clamp(s.stopMinute, 0, 1439));
    WriteUInt(L"Scheduler", L"Once",         s.once ? 1 : 0);
    WriteUInt(L"Scheduler", L"Days",         (unsigned long)(s.dayMask & 0x7F));
    WriteUInt(L"Scheduler", L"OnceDate",     (unsigned long)(s.onceDate < 0 ? 0 : s.onceDate));

    // [UI]
    m_ui.Save(iniPath);

    // Flush the INI cache to disk
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());

    // Start-with-Windows: manage the Run registry key
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS)
    {
        if (m_cfg.startWithWindows)
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            // Quote the path: an unquoted path with spaces is ambiguous.
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(hRun, NDM_PRODUCT_NAME, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(hRun, NDM_PRODUCT_NAME);
        }
        RegCloseKey(hRun);
    }
}
