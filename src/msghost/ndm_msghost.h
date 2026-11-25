#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// NdmMsgHost — IPC bridge between the NDM browser extension and NDMan.exe
//
//   browser ext  <-- native messaging (stdio) -->  NDMMsgHost.exe
//   NDMMsgHost.exe  <-- named pipe -->  NDMan.exe
//
// The pipe is per user and per logon session:
//   \\.\pipe\NDMMsgHost-<user SID>-<session id>
// DACL: SYSTEM + owner only (no mandatory label: a medium-integrity relay
// talks to the always-unelevated NDMan.exe). Both ends verify each other:
// the server checks the client process runs as the same user; the relay
// checks the server process is <its own folder>\NDMan.exe of the same user.
//
// Wire format on BOTH legs: 4-byte native-endian (little-endian) length
// followed by that many bytes of UTF-8 JSON (max 16 MiB). Each request gets
// exactly one reply, in order.
//
// Two modes:
//   1. Embedded in NDMan.exe: StartListening() runs a multi-client pipe
//      server. "addDownload" / "addDownloads" become NdmBrowserRequest
//      objects queued with NdmPostBrowserRequest (the main window gets a
//      WM_NDM_BROWSER_MSG carrying only a token).
//
//   2. Standalone NDMMsgHost.exe (NDM_MSGHOST_STANDALONE): speaks the
//      Chrome/Firefox native messaging protocol on stdin/stdout and relays
//      every message to NDMan.exe over the pipe, starting NDMan.exe
//      ("NDMan.exe -tray") when it is not running and the request needs it.
// ---------------------------------------------------------------------------

#define NDM_PIPE_PREFIX      L"\\\\.\\pipe\\NDMMsgHost-"
#define NDM_NATIVE_HOST_NAME "com.devnetra.ndm"
#define NDM_LAUNCH_ARG       L"-tray"   // arg the relay passes to NDMan.exe

class NdmMsgHost
{
public:
    // -----------------------------------------------------------------------
    // Embedded-mode API (called from NDMan.exe)
    // -----------------------------------------------------------------------
    static void StartListening();
    static void StopListening();
    static void SetMainWindow(HWND hWnd);

    // Process one JSON request and return the JSON reply (UTF-8).
    // Public so it can be unit-tested; thread-safe.
    static std::string HandleMessage(const std::string& json);

#ifndef NDM_MSGHOST_STANDALONE
    // The getMedia "ok" reply built from a yt-dlp -J document (no yt-dlp
    // run); "" when nothing is usable. For unit tests.
    static std::string MediaReplyFromJson(const std::string& ytdlpJson);
#endif

    // "\\.\pipe\NDMMsgHost-<SID of this process' user>-<session id>";
    // empty if the token cannot be read.
    static std::wstring PipeName();

#ifdef NDM_MSGHOST_STANDALONE
    // Run the stdin/stdout <-> named-pipe relay loop.
    static int RunStandalone();
#endif

private:
    static DWORD WINAPI PipeServerThread(LPVOID param);
    static DWORD WINAPI PipeClientThread(LPVOID param);
};
