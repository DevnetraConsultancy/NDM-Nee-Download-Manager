#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "../common/ndm_common.h"

// ---------------------------------------------------------------------------
// Result of one engine run.
// ---------------------------------------------------------------------------
enum NdmRunResult : int
{
    NDM_RUN_COMPLETED = 0,   // file is complete and in place
    NDM_RUN_STOPPED   = 1,   // stop event was signalled (pause / remove / exit)
    NDM_RUN_ERROR     = 2    // failed; dl->errorMsg holds the reason, dl->errorKind
                             // (HTTP engine) whether it was transient or fatal
};

// Called by the engine (from the download thread) when it changes a
// persistent field of the download (currently: the resolved filename).
typedef void (*NdmChangeCallback)(NdmDownload* dl, void* ctx);

struct NdmRunOptions
{
    bool              userNamed  = false;   // true = keep the user's name (ignore
                                            //        server Content-Disposition)
    bool              overwriteConfirmed = false; // true = user agreed to replace
                                            //        an existing file of that name
    bool              setStatus  = true;    // engine writes dl->status itself
    NdmChangeCallback onChange   = nullptr;
    void*             onChangeCtx = nullptr;
};

// ---------------------------------------------------------------------------
// NdmHttpEngine
//
// Segmented HTTP/HTTPS downloader on top of WinHTTP (synchronous API on
// worker threads, HTTP/1.1, one TCP connection per segment).
//
//  * Probe with an open ranged GET (Range: bytes=0-) -> size, range support,
//    Content-Disposition, ETag, final URL after redirects. The probe
//    response is kept and becomes the first connection's data stream.
//  * Ranged servers: N connections write directly into one preallocated
//    (sparse) "<file>.ndmpart" through per-connection 2 MB write buffers
//    flushed sequentially; free connections
//    steal half of the largest remaining segment (dynamic segmentation).
//  * Progress is persisted in "<file>.ndmstate" so a pause / app restart
//    resumes from the bytes already on disk.
//  * Servers without ranges / unknown size: single connection stream.
//  * On completion .ndmpart is renamed to the final name, state deleted.
// ---------------------------------------------------------------------------
class NdmHttpEngine
{
public:
    NdmHttpEngine()  = default;
    ~NdmHttpEngine() = default;

    // -----------------------------------------------------------------------
    // API used by NdmQueue (the queue owns the download threads)
    // -----------------------------------------------------------------------

    // Run a download synchronously on the calling thread.
    // dl->hStopEvent must be a manual-reset event (created if null).
    static NdmRunResult Run(NdmDownload* dl, const NdmRunOptions& opt);

    // Global speed limit for all downloads, bytes/sec (0 = unlimited).
    // It is shared out by the bandwidth allocator below; HTTP, media and
    // torrent downloads together stay within it.
    static void  SetSpeedLimit(DWORD bytesPerSec);
    static DWORD GetSpeedLimit();

    // Retry policy of one Run() after transient (network / 5xx) failures:
    // maxRetries 0 = unlimited; waits 3, 6, 12 ... s capped at maxDelaySec.
    // When the retries are used up the run ends with NDM_RUN_ERROR and
    // dl->errorKind = NDM_ERRKIND_TRANSIENT (the queue retries those later).
    static void SetRetryPolicy(int maxRetries, int maxDelaySec);

    // -----------------------------------------------------------------------
    // Bandwidth allocator (shared by all engines of the process)
    //
    // Every running download registers a "flow". About twice a second the
    // allocator recomputes each flow's rate from dl->speedLimit, dl->priority
    // (weights High 4 / Normal 2 / Low 1), the global limit and the measured
    // speeds (dl->speed):
    //  * Global limit set: weighted max-min fair share of the limit. A flow
    //    that has been using clearly less than its share for ~1.5 s (slow
    //    server) keeps a little headroom above what it uses and the rest
    //    goes to the others; as soon as it uses >= 80 % of its rate it gets
    //    its full share again. Per-download limits cap a flow's share.
    //  * No global limit: High and Normal are not limited (except by their
    //    own per-download limit). Low flows yield: while a High/Normal flow
    //    is transferring (> 1 KB/s), all Low flows together get at most a
    //    quarter of what the High/Normal flows achieve (at least 32 KB/s),
    //    i.e. Low ends up with <= ~20 % of a saturated line.
    //  * Media (yt-dlp) cannot change its rate while running: its flow is
    //    pinned to the rate it was started with (SetFlowFixedRate) and the
    //    other flows share what is left. A limit / priority change reaches a
    //    running yt-dlp only on its next start.
    // FlowRate() returns the current rate for a flow, 0 = unlimited.
    // -----------------------------------------------------------------------
    struct Flow;
    static Flow* RegisterFlow(NdmDownload* dl, int kind /*NDM_KIND_*/);
    static void  UnregisterFlow(Flow* f);
    static DWORD FlowRate(Flow* f);
    static void  SetFlowFixedRate(Flow* f, DWORD bps);

    // -----------------------------------------------------------------------
    // Address refresh support (NdmQueue::RefreshAddress)
    // -----------------------------------------------------------------------
    struct ProbeResult
    {
        DWORD        netError  = 0;      // WinHTTP / Win32 error, 0 = got an HTTP answer
        DWORD        status    = 0;      // HTTP status of the final response
        bool         rangesOk  = false;  // 206 with a valid Content-Range
        bool         sizeKnown = false;
        UINT64       total     = 0;
        std::wstring etag, lastMod, finalUrl, message;
    };
    // One ranged request ("Range: bytes=0-0", redirects followed like a
    // download): size, range support, ETag, Last-Modified. The body is never
    // downloaded (at most one byte; a 200 answer is closed unread).
    static bool ProbeUrl(const std::wstring& url, const std::wstring& referrer,
                         const std::wstring& cookies, const std::wstring& userAgent,
                         ProbeResult& out);
    // Facts stored in the download's .ndmstate (only if it belongs to dl:
    // same id and url). bytesOnDisk = bytes already in the .ndmpart.
    static bool ReadStateInfo(const NdmDownload* dl, UINT64& total, std::wstring& etag,
                              std::wstring& lastMod, UINT64& bytesOnDisk);
    // Same file? (size equal; Last-Modified / ETag equal where both sides
    // have them; an ETag-only mismatch is ignored when both Last-Modified
    // values exist and match). The rule the engine applies on resume.
    static bool SameRemoteFile(UINT64 totalA, const std::wstring& etagA, const std::wstring& lmA,
                               UINT64 totalB, const std::wstring& etagB, const std::wstring& lmB);
    // Rebind the .ndmstate of dl to newUrl (dl->url must still be the old
    // address) so the next run resumes the existing .ndmpart. true if there
    // is no state file at all (nothing to rebind) or it was rewritten.
    static bool RebindState(const NdmDownload* dl, const std::wstring& newUrl);
    // URL as the engine uses it (trimmed, #fragment dropped, spaces escaped).
    static std::wstring NormalizeUrl(const std::wstring& url);

    // Delete the .ndmpart/.ndmstate temp files of a download (only if they
    // belong to this download).
    static void DeleteTempFiles(const NdmDownload* dl);

    // Thread-safe updates of std::wstring fields the UI reads unlocked.
    // PrepareSharedStrings() must be called once when the NdmDownload is
    // created: it gives filename/errorMsg a fixed, zero-filled capacity so
    // later SetSharedString() calls never reallocate the buffer the UI
    // thread may be reading.
    static void PrepareSharedStrings(NdmDownload* dl);
    static void SetSharedString(std::wstring& field, const std::wstring& value);
    static std::wstring GetSharedString(const std::wstring& field);

    // Lock guarding string fields of all NdmDownload objects.
    static void LockFields(bool exclusive);
    static void UnlockFields(bool exclusive);

    // Atomic 64-bit read (32-bit builds would otherwise tear).
    static UINT64 AtomicRead64(const volatile UINT64* p);

    // Sanitize a file name for Windows (no path, no reserved chars/names).
    static std::wstring SanitizeFilename(const std::wstring& name);

    // Human readable text for a WinHTTP / Win32 error code.
    static std::wstring ErrorText(DWORD err);
};

// RAII registration of a bandwidth flow for the lifetime of one engine run.
class NdmFlowScope
{
public:
    NdmFlowScope(NdmDownload* dl, int kind) : m_f(NdmHttpEngine::RegisterFlow(dl, kind)) {}
    ~NdmFlowScope() { NdmHttpEngine::UnregisterFlow(m_f); }
    NdmFlowScope(const NdmFlowScope&) = delete;
    NdmFlowScope& operator=(const NdmFlowScope&) = delete;
    NdmHttpEngine::Flow* get() const { return m_f; }
    DWORD Rate() const { return NdmHttpEngine::FlowRate(m_f); }
private:
    NdmHttpEngine::Flow* m_f;
};
