#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define NDM_VERSION_MAJOR   1
#define NDM_VERSION_MINOR   0
#define NDM_VERSION_BUILD   1
#define NDM_VERSION_STR     "1.0.5"
#define NDM_VERSION_STR_W   L"1.0.5"

#define NDM_PRODUCT_NAME    L"Nee Download Manager"
#define NDM_EXE_NAME        L"NDMan.exe"
#define NDM_MUTEX_NAME      "NDM_App_Mutex_x9kZ2mQ1"
#define NDM_WINDOW_CLASS    L"NDMMainClass"
#define NDM_TRAY_CLASS      L"NDMTrayClass"

// ---------------------------------------------------------------------------
// Branding  (Invented with Care by Devnetra Consultancy)
// ---------------------------------------------------------------------------
#define NDM_PUBLISHER_NAME  L"Devnetra Consultancy"
#define NDM_TAGLINE         L"Invented with Care By Devnetra Consultancy"
#define NDM_CONTACT_EMAIL   L"devnetraconsultancy@gmail.com"
#define NDM_COPYRIGHT       L"\u00A9 2026 Devnetra Consultancy · MIT License"

// ---------------------------------------------------------------------------
// UI Theme — Premium Dark Gold
// ---------------------------------------------------------------------------
#define NDM_CLR_BG          RGB( 18,  20,  24)   // near-black background
#define NDM_CLR_BG_PANEL    RGB( 26,  29,  35)   // panel / card surface
#define NDM_CLR_BG_ALT      RGB( 32,  36,  44)   // alternating list row
#define NDM_CLR_BG_HEADER   RGB( 22,  25,  31)   // column header / toolbar bg
#define NDM_CLR_BG_HOVER    RGB( 40,  44,  54)   // hovered row / button
#define NDM_CLR_BG_SEL      RGB( 50,  44,  10)   // selected row (dark gold tint)
#define NDM_CLR_FG          RGB(220, 220, 225)   // primary text (off-white)
#define NDM_CLR_FG_DIM      RGB(140, 142, 150)   // secondary / dim text
#define NDM_CLR_GOLD        RGB(212, 175,  55)   // primary gold accent
#define NDM_CLR_GOLD_LIGHT  RGB(240, 210, 100)   // bright gold (hover / active)
#define NDM_CLR_GOLD_DARK   RGB(160, 128,  30)   // deep gold (pressed)
#define NDM_CLR_BORDER      RGB( 55,  60,  72)   // subtle border
#define NDM_CLR_BORDER_GOLD RGB(140, 110,  28)   // gold border accent
#define NDM_CLR_SEL_BG      RGB( 65,  58,  12)   // list selection bg
#define NDM_CLR_SEL_FG      RGB(240, 210, 100)   // list selection text
#define NDM_CLR_DISABLED    RGB( 70,  72,  80)   // disabled / muted
#define NDM_CLR_GREEN       RGB( 70, 200, 120)   // active / downloading indicator
#define NDM_CLR_RED         RGB(220,  70,  70)   // error indicator
#define NDM_CLR_AMBER       RGB(220, 160,  40)   // paused indicator
#define NDM_CLR_EDIT_BG     RGB( 30,  33,  40)   // input field background
#define NDM_CLR_EDIT_FG     RGB(220, 220, 225)   // input field text (alias)
#define NDM_CLR_INPUT_FG    RGB(220, 220, 225)   // input field text
// Header gradient stops
#define NDM_CLR_HDR_TOP     RGB( 26,  28,  34)
#define NDM_CLR_HDR_BOT     RGB( 18,  20,  24)
// Toolbar gradient stops
#define NDM_CLR_TB_TOP      RGB( 32,  35,  42)
#define NDM_CLR_TB_BOT      RGB( 24,  27,  33)

// ---------------------------------------------------------------------------
// Registry keys
// ---------------------------------------------------------------------------
#define NDM_REG_ROOT        L"Software\\Devnetra\\NDM"
#define NDM_REG_UNINSTALL   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Nee Download Manager"

// ---------------------------------------------------------------------------
// Custom window messages
// ---------------------------------------------------------------------------
#define WM_NDM_ADD_DOWNLOAD     (WM_APP + 1)
#define WM_NDM_TRAY_ICON        (WM_APP + 2)
#define WM_NDM_BROWSER_MSG      (WM_APP + 3)
#define WM_NDM_SPEED_UPDATE     (WM_APP + 4)

// Tray icon ID
#define NDM_TRAY_ICON_ID        1

// ---------------------------------------------------------------------------
// Download status enum
// ---------------------------------------------------------------------------
enum NdmStatus : int
{
    NDM_STATUS_QUEUED      = 0,
    NDM_STATUS_DOWNLOADING = 1,
    NDM_STATUS_PAUSED      = 2,
    NDM_STATUS_COMPLETED   = 3,
    NDM_STATUS_ERROR       = 4
};

// ---------------------------------------------------------------------------
// Download descriptor
// ---------------------------------------------------------------------------
struct NdmDownload
{
    DWORD           id             = 0;
    std::wstring    url;
    std::wstring    filename;
    std::wstring    savePath;
    UINT64          totalSize      = 0;
    UINT64          downloadedSize = 0;
    int             numConnections = 8;
    DWORD           speed          = 0;    // bytes/sec, updated by engine
    NdmStatus       status         = NDM_STATUS_QUEUED;
    HANDLE          hThread        = nullptr;
    HANDLE          hStopEvent     = nullptr;
    std::wstring    referrer;              // optional HTTP Referer
    std::wstring    errorMsg;             // last error description
    std::wstring    cookies;               // optional Cookie header value (from browser)
    std::wstring    userAgent;             // optional User-Agent (from browser)
    int             kind           = 0;    // NDM_KIND_*
    std::wstring    pageUrl;               // media: page the video was on (yt-dlp input)
    std::wstring    mediaFormat;           // media: yt-dlp format selector, e.g. "bv*[height<=1080]+ba/b"
    std::wstring    torrentFiles;          // torrent: selected file indexes "1,3,5" (aria2 --select-file); empty = all
    std::wstring    cookieJar;             // media: Netscape cookies.txt lines for yt-dlp (exact browser cookies)
    bool            overwriteConfirmed = false; // user chose "Replace" for an existing file
    std::wstring    mediaTemps;            // media: "|"-separated partial / intermediate file names yt-dlp
                                           //        reported (only these are ever deleted as temp files)
    std::wstring    cookiesEnc;            // DPAPI blob of cookies (cached for queue saves; "" = none / stale)
    std::wstring    cookieJarEnc;          // DPAPI blob of cookieJar (cached)
    // Live engine facts for the Properties window (written by the engine
    // thread with Interlocked/aligned stores, read by the UI):
    LONG            resumable   = -1;      // 1 = server accepts byte ranges, 0 = no, -1 = not known yet
    LONG            activeConns = 0;       // connections transferring right now
    LONG            fileMapOk   = 0;       // 1 = fileMap is valid (size known)
    WORD            fileMap[32] = {};      // per 1/32 of the file: permille already on disk
                                           // (torrents: activeConns = peers connected right now)

    // ---- phase 2 ------------------------------------------------------------
    // Per-download speed limit, bytes/s (0 = none) and priority (NDM_PRIO_*).
    // Change them only through NdmQueue::SetDownloadSpeedLimit / SetPriority
    // (atomic stores; the engines pick a change up within about one second).
    DWORD           speedLimit  = 0;
    int             priority    = 1;       // NDM_PRIO_NORMAL
    // Dates as UTC FILETIME values (100 ns units since 1601-01-01, the value
    // of GetSystemTimeAsFileTime). 0 = unknown: show blank, never a guess.
    UINT64          addedTime     = 0;
    UINT64          completedTime = 0;
    // Why the last run failed (status ERROR): NDM_ERRKIND_*. Only TRANSIENT
    // errors are retried automatically by the queue.
    int             errorKind   = 0;
    UINT64          nextRetryTime = 0;     // UTC FILETIME of the queued auto-retry, 0 = none
    int             autoRetries = 0;       // auto-retries since the last progress
    INT64           queueOrder  = 0;       // position in the queue (smaller = earlier)
    bool            scheduled   = false;   // member of the scheduler's queue (started at the start time)
    int             completionAction = 0;  // NDM_DONE_*: fired when THIS download completes (not persisted)
    std::wstring    torrentHash;           // torrent: v1 info hash (40 hex), "" = not known yet
};

// Priority values (NdmDownload::priority). Weights for the bandwidth split:
// High 4, Normal 2, Low 1.
#define NDM_PRIO_LOW     0
#define NDM_PRIO_NORMAL  1
#define NDM_PRIO_HIGH    2

// NdmDownload::errorKind
#define NDM_ERRKIND_NONE      0
#define NDM_ERRKIND_TRANSIENT 1   // network / server temporarily failing: auto-retried
#define NDM_ERRKIND_FATAL     2   // 404, disk full, server refuses resume, ...: needs the user

// On-completion actions (performed by the UI after its countdown).
#define NDM_DONE_NONE       0
#define NDM_DONE_SLEEP      1
#define NDM_DONE_HIBERNATE  2
#define NDM_DONE_SHUTDOWN   3

#define NDM_KIND_HTTP   0   // plain file, NdmHttpEngine
#define NDM_KIND_MEDIA  1   // video/audio via yt-dlp (+ffmpeg merge), NdmMediaRun
#define NDM_KIND_TORRENT 2  // magnet link or .torrent file via aria2c, NdmTorrentRun

// ---------------------------------------------------------------------------
// Parameters for adding a download (UI, browser capture, shell ext).
// ---------------------------------------------------------------------------
struct NdmAddParams
{
    std::wstring    url;
    std::wstring    savePath;              // folder; empty = config default
    std::wstring    filename;              // empty = derive from URL / server
    int             connections = 8;
    std::wstring    referrer;
    std::wstring    cookies;
    std::wstring    userAgent;
    bool            startNow    = true;    // false = add paused ("Download Later")
    int             kind        = NDM_KIND_HTTP;
    std::wstring    pageUrl;               // media only
    std::wstring    mediaFormat;           // media only
    std::wstring    torrentFiles;          // torrent only
    std::wstring    cookieJar;             // media only: Netscape cookies.txt lines
    // true ONLY when the user explicitly confirmed "Replace" for an existing
    // file in the File Info dialog. Browser / pipe supplied names never
    // overwrite anything.
    bool            overwriteConfirmed = false;
    int             priority    = 1;       // NDM_PRIO_*
    DWORD           speedLimit  = 0;       // bytes/s, 0 = none
    bool            scheduled   = false;   // add to the scheduler's queue
};

// Where an NdmBrowserRequest came from (decides what it may contain).
#define NDM_ORIGIN_EXTERNAL  0   // browser pipe, shell extension, WM_COPYDATA: URLs only
#define NDM_ORIGIN_CMDLINE   1   // our own command line / second instance: may be a local .torrent

// ---------------------------------------------------------------------------
// A download captured by the browser extension.
// Producers (pipe thread, command line, WM_COPYDATA) hand one to
// NdmPostBrowserRequest(); the main window receives only an opaque token in
// LPARAM of WM_NDM_BROWSER_MSG and redeems it with NdmTakeBrowserRequest().
// No heap pointer ever travels in a window message.
// ---------------------------------------------------------------------------
struct NdmBrowserRequest
{
    std::wstring    url;
    std::wstring    filename;              // browser-suggested name (may be empty)
    std::wstring    referrer;
    std::wstring    cookies;
    std::wstring    userAgent;
    std::wstring    mime;
    UINT64          fileSize    = 0;       // 0 = unknown
    bool            silent      = false;   // true = queue without showing dialog
    int             kind        = NDM_KIND_HTTP;
    std::wstring    pageUrl;               // media only (url may equal pageUrl)
    std::wstring    mediaFormat;           // media only
    std::wstring    qualityLabel;          // media only, for display, e.g. "1080p MP4"
    std::wstring    cookieJar;             // media only: Netscape cookies.txt lines (from structured cookies)
    int             origin      = NDM_ORIGIN_EXTERNAL;
    bool            autoCapture = false;   // a browser download the extension intercepted (not a user's
                                           // explicit "Download with NDM"): obeys Options -> Browser capture
};

// True for http://, https:// and magnet:? addresses (case-insensitive,
// no control characters / whitespace). ftp:// is not supported (WinHTTP has
// no FTP).
bool NdmIsNetOrMagnetUrl(const std::wstring& s);
// True for http:// and https:// only.
bool NdmIsHttpUrl(const std::wstring& s);

// Queue a request for the main window (takes ownership; false = not posted,
// request already freed). Posts WM_NDM_BROWSER_MSG with WPARAM = 0 and a
// token in LPARAM.
bool NdmPostBrowserRequest(HWND hWnd, NdmBrowserRequest* req);
// Redeem a token from WM_NDM_BROWSER_MSG. Unknown / reused tokens -> nullptr.
// The caller owns the returned object.
NdmBrowserRequest* NdmTakeBrowserRequest(LPARAM token);
// Free every request that was queued but never redeemed (main window exit).
void NdmDiscardBrowserRequests();

// Referer value to send to `target` for a page URL `referrer`, per the
// strict-origin-when-cross-origin policy: the full referrer (without
// fragment and user:pass@) when same-origin, only "scheme://host[:port]/"
// when cross-origin, nothing for an https page -> http target or when
// either URL is not http(s).
std::wstring NdmReferrerFor(const std::wstring& referrer, const std::wstring& target);

// Bidi controls / zero-width / invisible formatting characters that can
// disguise a file name: U+200B-U+200F, U+202A-U+202E, U+2066-U+2069,
// U+061C, U+FEFF. File-name sanitisers map them to '_'.
bool NdmIsDeceptiveChar(wchar_t c);

// Process runs with an elevated (admin) token.
bool NdmIsElevated();

// Per-user DPAPI (CryptProtectData, user scope) + base64. Empty in -> empty out.
std::wstring NdmProtectString(const std::wstring& plain);
// Reverse of NdmProtectString; returns false when the blob cannot be decrypted.
bool NdmUnprotectString(const std::wstring& b64, std::wstring& plain);

// ---------------------------------------------------------------------------
// Scheduler settings (local wall-clock times). See ndm_scheduler.h.
// ---------------------------------------------------------------------------
struct NdmSchedule
{
    bool    startEnabled = false;  // start the queue at startMinute
    bool    stopEnabled  = false;  // pause running downloads at stopMinute
    int     startMinute  = 0;      // minutes after local midnight (0..1439)
    int     stopMinute   = 0;
    bool    once         = false;  // false = daily on the days in dayMask; true = only on onceDate
    int     dayMask      = 0x7F;   // daily mode: bit 0 = Sunday ... bit 6 = Saturday
    int     onceDate     = 0;      // once mode: yyyymmdd (local date of the start)
};

// ---------------------------------------------------------------------------
// Application configuration
// ---------------------------------------------------------------------------
struct NdmConfig
{
    std::wstring    defaultSavePath;
    int             maxConnections  = 8;
    int             maxDownloads    = 3;
    bool            browserMonitor  = true;
    bool            startWithWindows = false;
    bool            closeToTray     = true;
    DWORD           speedLimit      = 0;   // 0 = unlimited (bytes/sec)
    int             retryCount      = 10;  // HTTP engine retries per run after a network
                                           // error (0 = unlimited)
    int             retryMaxDelay   = 60;  // longest wait between those retries, seconds
    NdmSchedule     schedule;
};

// ---------------------------------------------------------------------------
// Helper function declarations
// ---------------------------------------------------------------------------
std::wstring NdmFormatSize(UINT64 bytes);
std::wstring NdmFormatSpeed(DWORD bytesPerSec);
std::wstring NdmFormatTime(DWORD seconds);
std::wstring NdmSafeFilename(const std::wstring& url);

// Mark a finished download as "from the Internet" (Mark-of-the-Web) so
// SmartScreen / Office Protected View / Attachment Manager check it on open,
// exactly as if the browser had saved it. Call on the final file path after
// every successful download (http engine, media, each torrent file).
// Uses IAttachmentExecute (respects policy); falls back to writing the
// Zone.Identifier stream (ZoneId=3). Never throws; returns true on success.
// COM must not be required by the caller: the function initialises it itself.
bool NdmMarkOfTheWeb(const std::wstring& filePath,
                     const std::wstring& sourceUrl,
                     const std::wstring& referrer);
