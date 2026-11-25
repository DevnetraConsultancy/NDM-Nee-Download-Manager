// ndm_firstrun.h -- first-run questions, notification preferences and the
// "Add NDM to your browser" helper.
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// Preferences that NdmConfigMgr has no field for. Stored as REG_DWORD under
// HKCU\Software\Devnetra\NDM.
namespace NdmPrefs
{
    extern const wchar_t* const kNotifyDialog;   // show "Download complete" window
    extern const wchar_t* const kNotifyTray;     // tray notification on finish / failure
    extern const wchar_t* const kNotifySound;    // play a sound on finish
    extern const wchar_t* const kFileInfo;       // show "Download File Info" for browser captures
    extern const wchar_t* const kFirstRunDone;
    extern const wchar_t* const kWatchClipboard;  // offer copied download links (Add URL)

    bool Get(const wchar_t* name, bool def);
    void Set(const wchar_t* name, bool on);

    inline bool NotifyDialog() { return Get(kNotifyDialog, true); }
    inline bool NotifyTray()   { return Get(kNotifyTray, true); }
    inline bool NotifySound()  { return Get(kNotifySound, true); }
    inline bool FileInfo()     { return Get(kFileInfo, true); }
    inline bool WatchClipboard() { return Get(kWatchClipboard, true); }
}

class NdmFirstRun
{
public:
    // True if the first-run questions have never been answered.
    static bool IsNeeded();
    // Small modeless window: Start with Windows, browser capture and the
    // browser extension guide (notification choices live in Options).
    static void Show(HWND owner);
    // Opens the extension guide page in the browser and Explorer at
    // <install dir>\ext\chrome (or \firefox) with the folder selected.
    static void OpenBrowserSetup(HWND owner, bool firefox);
    // (The instruction lines next to the browser buttons: NdmUi::BrowserHint / BrowserNote.)
    // <install dir> (folder of NDMan.exe), no trailing backslash.
    static std::wstring InstallDir();
};
