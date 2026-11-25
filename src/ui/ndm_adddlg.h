// ndm_adddlg.h -- "Download File Info" / "Add URL" dialog (modeless, premium)
// Invented with Care By Devnetra Consultancy
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "../common/ndm_common.h"

class NdmAddDlg
{
public:
    // Manual "Add URL" (Ctrl+N). Prefills from the clipboard when it holds a
    // URL. Only one Add URL window exists at a time.
    static void ShowAddUrl(HWND owner, const std::wstring& prefillUrl = L"");

    // Download captured from the browser / shell extension. Always opens a
    // new window and brings it to the foreground.
    static void ShowBrowserRequest(HWND owner, const NdmBrowserRequest& req);

    // Returns a URL from the clipboard, or empty.
    static std::wstring GetClipboardUrl();
};
