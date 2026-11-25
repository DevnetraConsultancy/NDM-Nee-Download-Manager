// ndm_scheddlg.h -- Scheduler window (modeless, one at a time)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class NdmSchedDlg
{
public:
    // Start / stop times, max simultaneous downloads, the "when all
    // downloads finish" action, Start queue now / Stop queue.
    static void Show(HWND owner);
};
