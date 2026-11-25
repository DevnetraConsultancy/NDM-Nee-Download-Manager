#pragma once
// ---------------------------------------------------------------------------
// Internal API shared by the torrent engine (ndm_torrent.cpp, ndm_core) and
// the torrent add dialog (ndm_torrent_dlg.cpp, NDMan). No WinHTTP types here.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

struct NdmTorrentFile
{
    std::wstring path;          // relative path inside the torrent ("dir\file")
    UINT64       size = 0;
    bool         padding = false;   // BEP 47 padding file (hidden in the UI)
};

struct NdmTorrentInfo
{
    std::wstring                name;       // info.name (utf-8 aware)
    UINT64                      totalSize = 0;
    std::vector<NdmTorrentFile> files;      // aria2 --select-file index = position + 1
    std::string                 infoHash;   // 40 lower-case hex chars
    bool                        multiFile = false;
};

// Parse .torrent bytes (bencode). Computes the v1 info hash.
bool NdmTorrentParse(const std::string& bytes, NdmTorrentInfo& out);

// "magnet:?xt=urn:btih:..." -> 40 lower-case hex chars (hex or base32 form).
std::string  NdmTorrentMagnetHash(const std::wstring& magnet);
// Display name (dn=) of a magnet link, or empty.
std::wstring NdmTorrentMagnetName(const std::wstring& magnet);

// Read a local .torrent file or fetch an http(s) .torrent URL (max 32 MB).
bool NdmTorrentLoadSource(const std::wstring& src, std::string& bytes,
                          std::wstring& err, HANDLE hCancel = nullptr);

// Metadata cache: %APPDATA%\Devnetra\NDM\torrents\<hash>.torrent
bool NdmTorrentCacheGet(const std::string& hash, std::string& bytes);
void NdmTorrentCachePut(const std::string& hash, const std::string& bytes);

// Fetch the metadata of a magnet through aria2 (bt-metadata-only). Blocks
// until done, failed, or hCancel is signalled. Stores the result in the cache.
bool NdmTorrentFetchMetadata(const std::wstring& magnet, HANDLE hCancel,
                             std::string& bytes, std::wstring& err);

// Kill the shared aria2c process (normally it simply dies with NDM).
void NdmTorrentShutdown();
