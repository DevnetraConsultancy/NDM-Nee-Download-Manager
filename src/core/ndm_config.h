#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "../common/ndm_common.h"

// Main-window state kept in ndm.ini [UI]: the user's column widths (96-dpi
// px, "" = built-in defaults), the sort column (-1 = queue order) and
// direction, and one-time hints. Load / Save take the INI path (tests use a
// temporary file).
struct NdmUiState
{
    std::wstring columnWidths;
    int          sortColumn    = -1;
    bool         sortAscending = true;
    bool         trayHintShown = false;     // "NDM is still running" shown once ever

    void Load(const std::wstring& iniPath);
    void Save(const std::wstring& iniPath) const;
};

// NdmConfigMgr -- singleton that persists settings to an INI file
// File location: %APPDATA%\Devnetra\NDM\ndm.ini
class NdmConfigMgr
{
public:
    static NdmConfigMgr& GetInstance();

    NdmConfigMgr(const NdmConfigMgr&)            = delete;
    NdmConfigMgr& operator=(const NdmConfigMgr&) = delete;

    void Load();
    void Save();

    // Path helpers
    std::wstring         GetAppDataPath();
    static std::wstring  GetDefaultSavePath();

    // Getters
    const std::wstring& GetSavePath()       const { return m_cfg.defaultSavePath; }
    int                 GetMaxConnections() const { return m_cfg.maxConnections; }
    int                 GetMaxDownloads()   const { return m_cfg.maxDownloads; }
    bool                GetBrowserMonitor() const { return m_cfg.browserMonitor; }
    bool                GetStartWithWin()   const { return m_cfg.startWithWindows; }
    bool                GetCloseToTray()    const { return m_cfg.closeToTray; }
    DWORD               GetSpeedLimit()     const { return m_cfg.speedLimit; }
    // HTTP engine retry policy: retries per run (0 = unlimited) and the
    // longest backoff between them, seconds ([Connection] RetryCount /
    // RetryMaxDelay). The queue applies them to the engine on every start.
    int                 GetRetryCount()     const { return m_cfg.retryCount; }
    int                 GetRetryMaxDelay()  const { return m_cfg.retryMaxDelay; }
    // Scheduler ([Scheduler] section). After changing it call
    // NdmQueue::SetSchedule(GetSchedule()) so the running timer uses it.
    const NdmSchedule&  GetSchedule()       const { return m_cfg.schedule; }

    // Setters
    void SetSavePath(const std::wstring& p)  { m_cfg.defaultSavePath  = p; }
    void SetMaxConnections(int n)            { m_cfg.maxConnections    = n; }
    void SetMaxDownloads(int n)              { m_cfg.maxDownloads      = n; }
    void SetBrowserMonitor(bool b)           { m_cfg.browserMonitor    = b; }
    void SetStartWithWin(bool b)             { m_cfg.startWithWindows  = b; }
    void SetCloseToTray(bool b)              { m_cfg.closeToTray       = b; }
    void SetSpeedLimit(DWORD bps)            { m_cfg.speedLimit        = bps; }
    void SetRetryCount(int n)                { m_cfg.retryCount        = n < 0 ? 0 : (n > 1000 ? 1000 : n); }
    void SetRetryMaxDelay(int sec)           { m_cfg.retryMaxDelay     = sec < 1 ? 1 : (sec > 3600 ? 3600 : sec); }
    void SetSchedule(const NdmSchedule& s)   { m_cfg.schedule          = s; }

    NdmConfig& Config() { return m_cfg; }

    // Main-window state ([UI] section). Load / Save include it; SaveUi
    // writes only that section (no Run-key update).
    NdmUiState&  Ui() { return m_ui; }
    std::wstring GetIniPath() { return GetAppDataPath() + L"ndm.ini"; }
    void         SaveUi();

private:
    NdmConfigMgr();
    NdmConfig  m_cfg;
    NdmUiState m_ui;
};
