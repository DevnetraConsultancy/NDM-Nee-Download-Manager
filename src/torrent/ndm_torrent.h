#pragma once
// ---------------------------------------------------------------------------
// NdmTorrent — BitTorrent / magnet downloads via the bundled aria2c.exe
// (<install dir>\tools\aria2c.exe), controlled over its local JSON-RPC.
// ---------------------------------------------------------------------------
#include "../common/ndm_common.h"
#include "../network/ndm_http.h"

// Runs one NDM_KIND_TORRENT download on the queue's worker thread.
// dl->url = "magnet:?..." or a local/remote .torrent path/URL.
// Same contract as NdmHttpEngine::Run (hStopEvent, progress fields, result).
NdmRunResult NdmTorrentRun(NdmDownload* dl, const NdmRunOptions& opt);

// UI entry: the main window calls this for a magnet: URI or a .torrent
// path/URL (command line, file association, browser capture, Add URL).
// Shows the compact torrent dialog (metadata, file list with checkboxes,
// save folder) and adds an NDM_KIND_TORRENT item via AddDownloadEx.
// Must be called on the UI thread. Returns false if the user cancelled.
bool NdmTorrentShowAddDialog(HWND owner, const std::wstring& magnetOrTorrent);

// True for "magnet:?..." or something ending in ".torrent".
bool NdmTorrentIsTorrentLink(const std::wstring& s);

// App exit: saves aria2's session, shuts aria2c down gracefully (it flushes
// its .aria2 control files) and refuses any restart. Call BEFORE
// NdmQueue::Shutdown(); running torrent items then end as STOPPED.
// Idempotent.
void NdmTorrentShutdown();
