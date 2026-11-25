// ndm_progdlg.h -- per-download progress / properties window (modeless)
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class NdmProgressDlg
{
public:
    // Opens (or focuses) the progress window for a download id.
    static void Show(HWND owner, DWORD downloadId);
    // Closes the window for an id if one is open (e.g. after Delete).
    static void CloseFor(DWORD downloadId);
};
