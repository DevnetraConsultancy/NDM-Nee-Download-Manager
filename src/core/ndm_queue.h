#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include "../common/ndm_common.h"

// Result of NdmQueue::RefreshAddress.
enum NdmRefreshResult : int
{
    NDM_REFRESH_OK            = 0,  // address (and .ndmstate binding) updated
    NDM_REFRESH_NOT_FOUND     = 1,  // no such download
    NDM_REFRESH_NOT_HTTP      = 2,  // new address is not http(s), or the item is media / torrent
    NDM_REFRESH_BUSY          = 3,  // item is downloading / completed: pause it first
    NDM_REFRESH_NETWORK_ERROR = 4,  // could not reach the server (message has the reason)
    NDM_REFRESH_HTTP_ERROR    = 5,  // server answered 4xx/5xx (message has the status)
    NDM_REFRESH_SIZE_MISMATCH = 6,  // the new address serves a file of another size
    NDM_REFRESH_DIFFERENT_FILE= 7,  // same size, but Last-Modified / ETag say another file
    NDM_REFRESH_NO_RANGES     = 8,  // same file, but that server refuses byte ranges, so the
                                    // bytes already downloaded could not be kept: not applied
    NDM_REFRESH_SAVE_FAILED   = 9   // the .ndmstate could not be rewritten
};

// Callback for on-completion actions (sleep / hibernate / shutdown). Called
// on a queue worker thread, outside all queue locks. id = the download that
// completed (per-download action) or 0 ("when all downloads are done").
// The UI shows its cancellable countdown and performs the action itself.
typedef void (*NdmCompletionActionCallback)(int action /*NDM_DONE_*/, DWORD id, void* ctx);

// ---------------------------------------------------------------------------
// NdmQueue — singleton download queue manager
//
// Threading model
//  * All map / status changes happen under m_cs.
//  * Each active download runs NdmHttpEngine::Run on its own thread.
//  * RemoveDownload() of an active download never frees memory a worker still
//    uses: the item leaves the map immediately, but the NdmDownload object is
//    deleted by its own thread once the engine has returned.
//  * Callers never get NdmDownload pointers, only copies (GetSnapshot /
//    GetAllSnapshots). The numeric fields are updated atomically; string
//    fields that change while downloading (filename, errorMsg) are updated in
//    place without reallocation.
// ---------------------------------------------------------------------------
class NdmQueue
{
public:
    // Singleton access
    static NdmQueue& GetInstance();

    // Prevent copy/move
    NdmQueue(const NdmQueue&)            = delete;
    NdmQueue& operator=(const NdmQueue&) = delete;

    // -----------------------------------------------------------------------
    // Queue operations
    // -----------------------------------------------------------------------

    // Add a new download; returns its assigned ID (0 if url is empty).
    // A non-empty filename counts as "user named" (never auto-renamed).
    DWORD AddDownload(const std::wstring& url,
                      const std::wstring& savePath,
                      const std::wstring& filename     = L"",
                      int                 connections  = 8);

    // Add with full parameters (referrer, cookies, UA, start-later).
    DWORD AddDownloadEx(const NdmAddParams& p);

    // Remove (and cancel) a download by ID. Partial temp files are deleted;
    // a completed file is kept.
    void RemoveDownload(DWORD id);

    // Pause a running or queued download.
    void PauseDownload(DWORD id);

    // Resume a paused / failed download (it is queued and started when a
    // slot is free; progress already on disk is kept).
    void ResumeDownload(DWORD id);

    // Consistent copies (hThread/hStopEvent are nulled in the copy).
    bool                      GetSnapshot(DWORD id, NdmDownload& out);
    std::vector<NdmDownload>  GetAllSnapshots();

    // -----------------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------------
    // SaveQueue/LoadQueue also remember the path; the queue then saves
    // itself automatically after changes (add/remove/pause/finish...):
    // coalesced on a background thread, at most about once per second.
    // SaveQueue itself writes synchronously. After Shutdown() it is a no-op
    // (the final state was already written; the items are gone).
    void SaveQueue(const std::wstring& path);
    void LoadQueue(const std::wstring& path);

    // -----------------------------------------------------------------------
    // Runtime settings (populated from NdmConfigMgr at startup)
    // -----------------------------------------------------------------------
    // Max simultaneous downloads. Persisted by NdmConfigMgr ([General]
    // MaxDownloads); raising it starts queued items at once.
    void SetMaxActiveDownloads(int n);
    int  GetMaxActiveDownloads() const { return m_maxActive; }

    // Starts QUEUED downloads while fewer than max are active, High before
    // Normal before Low, then by queue order. Also applies the configured
    // global speed limit and retry policy. Called automatically on every change.
    void ScheduleNext();

    // Stop all downloads (state saved for resume), wait for their threads,
    // save the queue. Called automatically from the destructor; may be
    // called explicitly before the app exits.
    void Shutdown(DWORD timeoutMs = 8000);

    // -----------------------------------------------------------------------
    // Per-download speed limit / priority (persisted; live: applied to a
    // running HTTP download or torrent within ~1 s; a running yt-dlp media
    // download keeps its rate until it is started again).
    // -----------------------------------------------------------------------
    bool SetDownloadSpeedLimit(DWORD id, DWORD bytesPerSec /*0 = none*/);
    bool SetPriority(DWORD id, int priority /*NDM_PRIO_*/);

    // -----------------------------------------------------------------------
    // Queue order (persisted). GetQueueOrder() = all ids, first in queue first.
    // -----------------------------------------------------------------------
    bool MoveUp(DWORD id);
    bool MoveDown(DWORD id);
    bool MoveToTop(DWORD id);
    bool MoveToBottom(DWORD id);
    std::vector<DWORD> GetQueueOrder();

    // -----------------------------------------------------------------------
    // Lookups
    // -----------------------------------------------------------------------
    // Downloads whose address is the same as url (trimmed, #fragment
    // dropped, scheme/host case-insensitive; magnets: same info hash).
    std::vector<DWORD> FindByUrl(const std::wstring& url);
    // A paused / failed HTTP download with the same file name
    // (case-insensitive) and the same known size (> 0): the browser offers
    // "use this link for the existing download?". 0 = none.
    DWORD FindRefreshCandidate(const std::wstring& url, const std::wstring& fileName, UINT64 fileSize);

    // -----------------------------------------------------------------------
    // Rename (UI "Rename..."). A completed download: its file on disk is
    // renamed (MoveFileEx, never replacing another file) and the item
    // follows. An HTTP download that has not written anything yet (no
    // bytes, no .ndmpart / .ndmstate): only the target name changes and
    // counts as user named. Everything else (running, partial data, media
    // not finished, torrents) is refused. msg = reason when false.
    // -----------------------------------------------------------------------
    bool RenameDownload(DWORD id, const std::wstring& newName, std::wstring& msg);

    // -----------------------------------------------------------------------
    // Refresh download address. Probes newUrl (no body download), checks it
    // is the same file as the part on disk (or as the known size), then sets
    // the item's url (and referrer / cookies if non-empty) and rebinds the
    // .ndmstate so Resume continues the existing .ndmpart. The item must be
    // paused / failed / queued-not-running. BLOCKS for up to ~30 s on network
    // I/O: call it from a worker thread, not the UI thread. msg receives a
    // human-readable explanation (empty on success).
    // -----------------------------------------------------------------------
    NdmRefreshResult RefreshAddress(DWORD id, const std::wstring& newUrl,
                                    const std::wstring& newReferrer, const std::wstring& newCookies,
                                    std::wstring& msg);

    // -----------------------------------------------------------------------
    // Auto-recovery. Items that failed with errorKind TRANSIENT are retried
    // by the queue: after 30 s, 1, 2, 4 ... min (max 15 min), and at once
    // when the network comes back / the system resumed (dl->nextRetryTime
    // tells the UI when). The core watches the network itself
    // (NotifyAddrChange); the UI forwards WM_POWERBROADCAST /
    // PBT_APMRESUMEAUTOMATIC as OnSystemResumed().
    // -----------------------------------------------------------------------
    void OnNetworkRestored();
    void OnSystemResumed();
    // Tests / tuning: first and longest auto-retry delay (ms).
    void SetAutoRetryDelays(DWORD firstMs, DWORD maxMs);

    // -----------------------------------------------------------------------
    // Scheduler (one core timer thread, wakes at the due times, at most
    // every 15 s otherwise). Start time: StartQueue(); stop time: StopQueue().
    // -----------------------------------------------------------------------
    void SetSchedule(const NdmSchedule& s);
    NdmSchedule GetSchedule();
    // Membership of the scheduler queue (persisted).
    bool SetScheduled(DWORD id, bool on);
    // Start every scheduled item that is paused / failed (they are queued
    // and started up to the max-active limit).
    void StartQueue();
    // Pause everything downloading or queued; these items join the scheduler
    // queue so the next start time resumes them.
    void StopQueue();

    // -----------------------------------------------------------------------
    // On-completion action. Per download: fires when that download completes.
    // All: fires once when nothing is downloading, queued or waiting for an
    // auto-retry any more and at least one download completed since it was
    // set. Both are one-shot and not persisted (a restart never powers off).
    // -----------------------------------------------------------------------
    void SetCompletionActionCallback(NdmCompletionActionCallback cb, void* ctx);
    bool SetDownloadCompletionAction(DWORD id, int action);
    void SetAllDoneAction(int action);
    int  GetAllDoneAction();

private:
    NdmQueue();
    ~NdmQueue();

    static DWORD WINAPI ThreadProc(LPVOID param);
    static void         OnEngineChange(NdmDownload* dl, void* ctx);
    void                OnJobFinished(NdmDownload* dl, int result);
    void                AutoSave();                 // mark dirty, wake the saver
    void                WriteQueue(const std::wstring& path);
    static DWORD WINAPI SaveThreadProc(LPVOID param);
    void                StopSaver();
    void                DestroyDownload(NdmDownload* dl, bool deleteTemp);
    // core timer thread: auto-retries, scheduler, network watcher
    void                EnsureTimer();
    void                StopTimer();
    void                WakeTimer();
    static DWORD WINAPI TimerThreadProc(LPVOID param);
    void                TimerLoop();
    UINT64              RunDueRetries(UINT64 nowFt);   // returns the next due time (0 = none)
    void                RetryTransientNow();
    bool                Move(DWORD id, int how);
    void                CheckAllDone(std::vector<std::pair<int, DWORD>>& fire);
    void                FireActions(const std::vector<std::pair<int, DWORD>>& fire);

    std::map<DWORD, NdmDownload*> m_downloads;
    std::set<NdmDownload*>        m_running;     // thread currently active
    std::set<NdmDownload*>        m_zombies;     // removed while running
    std::set<DWORD>               m_userNamed;   // ids with explicit filename
    DWORD            m_nextId      = 1;
    volatile int     m_maxActive   = 3;
    bool             m_shuttingDown = false;
    bool             m_shutdownDone = false;
    std::wstring     m_autoSavePath;
    CRITICAL_SECTION m_cs;
    CRITICAL_SECTION m_saveCs;
    // background saver (debounced AutoSave)
    HANDLE           m_saveThread = nullptr;
    HANDLE           m_saveEvent  = nullptr;   // auto-reset: "something changed"
    HANDLE           m_saveQuit   = nullptr;   // manual-reset: stop the saver
    volatile LONG    m_dirty      = 0;
    bool             m_saverStopped = false;
    bool             m_keepUnread   = false;   // queue.xml was unreadable at load (guarded by m_saveCs)
    INT64            m_nextOrder    = 1;       // next queueOrder for a new item
    std::map<DWORD, UINT64> m_failBytes;       // bytes on disk at the last transient failure
    // auto-retry delays
    DWORD            m_retryFirstMs = 30000;
    DWORD            m_retryMaxMs   = 15 * 60 * 1000;
    // core timer
    HANDLE           m_timerThread  = nullptr;
    HANDLE           m_timerWake    = nullptr;   // auto-reset
    HANDLE           m_timerQuit    = nullptr;   // manual-reset
    bool             m_timerStopped = false;
    NdmSchedule      m_schedule;
    // completion actions
    NdmCompletionActionCallback m_doneCb  = nullptr;
    void*            m_doneCtx      = nullptr;
    int              m_allDoneAction = NDM_DONE_NONE;
    bool             m_allDoneSeenCompletion = false;
};
