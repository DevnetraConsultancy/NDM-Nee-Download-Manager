// ndm_dialogs.h -- small themed dialogs: question with buttons, one-line
// input, duplicate / refresh prompts and the "Refresh download address"
// window.
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "../common/ndm_common.h"
#include "ndm_theme.h"

namespace NdmDialogs
{
    // Modal question. buttons[0] is the default (gold, focused, rightmost).
    // Returns the index of the button pressed, -1 for Esc / close box.
    int  Ask(HWND owner, const wchar_t* title, const std::wstring& text,
             const std::vector<std::wstring>& buttons, NdmGlyph glyph = GLY_INFO);

    // Modal question with buttons in screen order (left to right).
    // defaultButton: gold, focused, Enter. cancelButton: Esc / close box
    // (-1: they return -1). dangerButton: red outline (-1: none).
    // Returns the index of the button pressed.
    struct AskSpec
    {
        const wchar_t*            title = L"NDM";
        std::wstring              text;
        std::vector<std::wstring> buttons;
        int                       defaultButton = 0;
        int                       cancelButton  = -1;
        int                       dangerButton  = -1;
        NdmGlyph                  glyph = GLY_INFO;
    };
    int  AskEx(HWND owner, const AskSpec& spec);
    // One-button notice (OK).
    void Notice(HWND owner, const wchar_t* title, const std::wstring& text, NdmGlyph glyph = GLY_INFO);

    // Modal one-line input. validate(text) returns "" to accept or an error
    // shown under the field (the window stays open). suffix: unit drawn
    // right of the field (may be null). selectStem: select the file name
    // without its extension at first.
    bool Input(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring& text,
               const std::function<std::wstring(const std::wstring&)>& validate,
               bool numeric = false, const wchar_t* suffix = nullptr, bool selectStem = false);

    // "This link is already in your list ..." for the item `existing`.
    // 0 = open existing, 1 = download again, -1 = cancel.
    int  AskDuplicate(HWND owner, DWORD existing);

    // "file.zip is already in your list (paused at 62%). Use this new link
    // to continue it?"  0 = continue, 1 = add as new, -1 = cancel.
    int  AskContinueWithNewLink(HWND owner, DWORD candidate);

    // Refresh download address (modeless, one per download). With req:
    // automatic mode for a browser download that matches `id`: checks at
    // once with the browser's link / referrer / cookies; on success the
    // download resumes, on failure the window explains and offers
    // "Add as new" (the normal File Info dialog for req).
    void ShowRefresh(HWND owner, DWORD id, const NdmBrowserRequest* req = nullptr);

    // App exit, before NdmQueue::Shutdown: wait (at most ms) for address
    // checks still running on worker threads.
    void WaitForWorkers(DWORD ms);
}
