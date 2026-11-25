#pragma once
// ---------------------------------------------------------------------------
// NDM Self-Extracting Archive (SFX) format — version 3
//
// Appended after the stub PE image by tools/make_sfx_payload.ps1:
//
//   [SfxHeader]
//   [SfxEntryHdr] [SfxBlockHdr][block data] [SfxBlockHdr][block data] ...  -- entry 0
//   [SfxEntryHdr] [SfxBlockHdr][block data] ...                             -- entry 1
//   ...
//   [0..7 zero bytes]                              (pads the tail to 8-byte alignment)
//   [SfxTail]                                      (48 bytes)
//   [WIN_CERTIFICATE table]                        (optional: Authenticode signature)
//
// The installer is signed AFTER the payload is appended. signtool appends the
// certificate table at the end of the file and records its file offset in
// the PE IMAGE_DIRECTORY_ENTRY_SECURITY data directory, so readers find the
// tail at (certificate table offset - sizeof(SfxTail)), or at the end of the
// file when the image is unsigned. The packer pads so that the tail ends on
// an 8-byte boundary, as the certificate table must start 8-byte aligned.
//
// Every file is split into blocks of at most SfxEntryHdr::blockSize original
// bytes. Each block is compressed independently with the Windows Compression
// API (cabinet.dll, buffer mode) or stored, so the extractor needs only two
// block-sized buffers no matter how large the file is.
//
// Integrity: SHA-256 of the whole payload (SfxHeader .. padding, i.e. every
// byte from SfxTail::payloadOff up to the tail) is stored in the tail and
// verified before anything is extracted; CRC-32 (IEEE, reflected 0xEDB88320)
// of every block's ORIGINAL bytes and of every whole file. Sizes are 64-bit.
// All multi-byte fields are little-endian.
// ---------------------------------------------------------------------------
#include <windows.h>

#define NDM_SFX_MAGIC        0x584D444E   // 'NDMX' LE
#define NDM_SFX_VERSION      3

// Block / entry compression methods (values match COMPRESS_ALGORITHM_*)
#define SFX_METHOD_STORED      0
#define SFX_METHOD_XPRESS_HUFF 4          // COMPRESS_ALGORITHM_XPRESS_HUFF
#define SFX_METHOD_LZMS        5          // COMPRESS_ALGORITHM_LZMS

#define SFX_MAX_BLOCK_SIZE   (32u * 1024u * 1024u)   // sanity limit for readers
#define SFX_MAX_ENTRIES      100000u

#pragma pack(push, 1)

struct SfxHeader
{
    DWORD magic;        // NDM_SFX_MAGIC
    DWORD version;      // NDM_SFX_VERSION
    DWORD numEntries;   // number of file entries
    DWORD reserved;
};

struct SfxEntryHdr
{
    UINT64 origSize;    // original file size
    UINT64 compSize;    // bytes that follow this header (all block headers + data)
    DWORD  crc32;       // CRC-32 of the whole ORIGINAL file
    DWORD  flags;       // SFX_FLAG_*
    DWORD  method;      // preferred SFX_METHOD_* (blocks may still be stored)
    DWORD  blockSize;   // original bytes per block (last block may be shorter)
    DWORD  numBlocks;   // number of blocks (0 for an empty file)
    DWORD  reserved;
    WCHAR  relPath[260];// relative path inside install dir (null-terminated)
};

struct SfxBlockHdr
{
    DWORD origLen;      // original bytes in this block
    DWORD compLen;      // bytes of data that follow
    DWORD crc32;        // CRC-32 of this block's ORIGINAL bytes
    DWORD method;       // SFX_METHOD_* actually used for this block
};

struct SfxTail
{
    DWORD  magic;       // NDM_SFX_MAGIC
    DWORD  version;     // NDM_SFX_VERSION
    UINT64 payloadOff;  // file offset where SfxHeader begins
    BYTE   sha256[32];  // SHA-256 of [payloadOff, offset of this tail)
};

#pragma pack(pop)

static_assert(sizeof(SfxEntryHdr) == 560, "SfxEntryHdr layout");
static_assert(sizeof(SfxBlockHdr) == 16,  "SfxBlockHdr layout");
static_assert(sizeof(SfxTail)     == 48,  "SfxTail layout");

// flags
#define SFX_FLAG_NONE       0x00
#define SFX_FLAG_EXEC       0x01    // executable
#define SFX_FLAG_RUNAFTER   0x02    // run this file after full extraction
#define SFX_FLAG_TOOL       0x04    // third-party tool (install dir\tools\)
