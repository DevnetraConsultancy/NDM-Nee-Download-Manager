// ndm_optdlg.h -- NDM Options (modal, tabbed, premium)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../common/ndm_common.h"

class NdmOptDlg
{
public:
    // Modal. Returns true when the user saved changes (already applied).
    static bool Show(HWND parent, int initialTab = 0);

    // Applies the current NdmConfigMgr settings to the running app
    // (queue limits, Run key, browser-monitor flag). Called at startup too.
    static void ApplySettings();

    // Writes / removes the HKCU Run entry ("<exe>" -tray) per the config.
    static void ApplyRunKey();
};
