// ndm_power.h -- on-completion actions: cancellable countdown window and
// the sleep / hibernate / shut down itself.
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace NdmPower
{
    // NdmCompletionActionCallback for NdmQueue (any thread): posts
    // WM_NDM_DONE_ACTION (wParam = action, lParam = id) to the HWND in ctx.
    void OnQueueAction(int action, DWORD id, void* ctx);

    // UI thread: topmost "Shutting down in 30 s  [Cancel]" window; performs
    // the action when the countdown ends. id = finished download, 0 = all.
    // A second request while one is counting down is ignored.
    void StartCountdown(HWND owner, int action, DWORD id);

    // Can this PC do it (sleep / hibernate enabled)? Shut down: always.
    bool Available(int action);
    // "Hibernate is not available on this PC." ("" when available)
    std::wstring UnavailableText(int action);
}
