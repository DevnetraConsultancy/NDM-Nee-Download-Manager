#pragma once
// ---------------------------------------------------------------------------
// NdmMedia — video/audio/photo downloads from web pages (YouTube, Facebook,
// Instagram, any site yt-dlp supports) using the bundled yt-dlp.exe and
// ffmpeg.exe in <install dir>\tools\.
// ---------------------------------------------------------------------------
#include "../common/ndm_common.h"
#include "../network/ndm_http.h"

// Runs one NDM_KIND_MEDIA download on the queue's worker thread.
// Same contract as NdmHttpEngine::Run: honours dl->hStopEvent (pause/remove),
// keeps dl->totalSize / downloadedSize / speed / filename / errorMsg updated,
// returns COMPLETED / STOPPED / ERROR. Never sets dl->status when
// opt.setStatus == false (the queue does).
NdmRunResult NdmMediaRun(NdmDownload* dl, const NdmRunOptions& opt);

// Runs "yt-dlp -J" for a page and returns its raw UTF-8 JSON on stdout.
// Blocks the calling thread up to timeoutMs (or until hAbort is signalled).
// On failure returns false and a short human readable error.
struct NdmMediaProbeArgs
{
    std::wstring pageUrl;
    std::wstring cookies;     // "a=b; c=d" for pageUrl (may be empty; fallback)
    std::wstring cookieJar;   // exact Netscape cookies.txt lines (preferred when set)
    std::wstring userAgent;
    std::wstring referrer;
    DWORD        timeoutMs = 45000;
    HANDLE       hAbort    = nullptr;
};
bool NdmMediaProbe(const NdmMediaProbeArgs& a, std::string& jsonOut, std::wstring& errorOut);

// Full path of a helper in <install dir>\tools\ (NDM_DEV_BUILD only:
// also <repo>\third_party\), empty if not found. name e.g. L"yt-dlp.exe".
std::wstring NdmMediaToolPath(const wchar_t* name);

// "<stem>." + rest is one of yt-dlp's own partial / intermediate files:
// "fNNN.<media ext>[.part|.ytdl|.part-FragN]", "temp.<media ext>",
// "<media ext>.part|.ytdl|.part-FragN". Case-insensitive. Shared with the
// queue's temp-file cleanup.
bool NdmMediaIsPartialRest(const std::wstring& rest);

// True when `name` (a bare file name) splits into "<stem>.<rest>" with a
// non-empty stem and NdmMediaIsPartialRest(rest).
bool NdmMediaIsPartialName(const std::wstring& name);

// Deletes the yt-dlp partial / intermediate files of a media download: only
// names recorded in dl->mediaTemps by NdmMediaRun (plus their ".part",
// ".ytdl" and ".part-FragN" companions), each re-checked with
// NdmMediaIsPartialName, never dl->filename itself, never a directory or
// reparse point, never outside dl->savePath.
void NdmMediaDeleteTempFiles(const NdmDownload* dl);
