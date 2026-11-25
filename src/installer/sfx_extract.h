#pragma once
// ---------------------------------------------------------------------------
// NDM SFX v2 streaming reader / extractor (header-only, used by ndmsetup.cpp)
//
// - Opens the payload appended to an EXE, validates the tail/header and builds
//   an index of all entries (offsets only; no file data is read).
// - Extracts one entry at a time, block by block: read block -> decompress
//   (cabinet.dll Compression API) -> verify block CRC -> write. Memory use is
//   two block-sized buffers regardless of file size. The whole-file CRC and
//   all sizes are verified as well.
// - The payload SHA-256 stored in the tail is verified in Open(), before any
//   entry is trusted. The tail is located in front of the Authenticode
//   certificate table when the installer is signed (see ndm_sfx.h).
// ---------------------------------------------------------------------------
#include <windows.h>
#include <compressapi.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include "ndm_sfx.h"

#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "bcrypt.lib")

// ---- CRC-32 (IEEE, reflected) -------------------------------------------
static DWORD SfxCrc32Update(DWORD crc, const BYTE* data, size_t len)
{
    static DWORD table[8][256];
    static bool  init = false;
    if (!init) {
        for (DWORD i = 0; i < 256; i++) {
            DWORD c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[0][i] = c;
        }
        for (DWORD i = 0; i < 256; i++)
            for (int t = 1; t < 8; t++)
                table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
        init = true;
    }
    // slicing-by-8
    while (len >= 8) {
        DWORD a = crc ^ (DWORD)(data[0] | (data[1] << 8) | (data[2] << 16) | ((DWORD)data[3] << 24));
        DWORD b = (DWORD)(data[4] | (data[5] << 8) | (data[6] << 16) | ((DWORD)data[7] << 24));
        crc = table[7][a & 0xFF] ^ table[6][(a >> 8) & 0xFF] ^ table[5][(a >> 16) & 0xFF] ^ table[4][a >> 24] ^
              table[3][b & 0xFF] ^ table[2][(b >> 8) & 0xFF] ^ table[1][(b >> 16) & 0xFF] ^ table[0][b >> 24];
        data += 8; len -= 8;
    }
    while (len--) crc = (crc >> 8) ^ table[0][(crc ^ *data++) & 0xFF];
    return crc;
}
static DWORD SfxCrc32(const BYTE* data, size_t len)
{
    return ~SfxCrc32Update(0xFFFFFFFFu, data, len);
}

// ---- error text ----------------------------------------------------------
static std::wstring SfxErrorText(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = L"Error " + std::to_wstring(code);
    if (buf) { s += L": "; s += buf; LocalFree(buf); }
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

struct SfxIndexEntry
{
    std::wstring relPath;
    UINT64 origSize  = 0;
    UINT64 compSize  = 0;
    UINT64 dataOff   = 0;   // file offset of the first SfxBlockHdr
    DWORD  crc32     = 0;
    DWORD  flags     = 0;
    DWORD  method    = 0;
    DWORD  blockSize = 0;
    DWORD  numBlocks = 0;
};

// Progress callback: bytes of the current entry written so far. Return false
// to cancel.
typedef bool (*SfxProgressFn)(void* ctx, const SfxIndexEntry& e, UINT64 doneBytes);

class SfxReader
{
public:
    std::vector<SfxIndexEntry> entries;
    UINT64 totalOrig = 0;
    UINT64 totalComp = 0;
    bool   cancelled = false;   // set when a progress callback returned false

    SfxReader() = default;
    SfxReader(const SfxReader&) = delete;
    SfxReader& operator=(const SfxReader&) = delete;
    ~SfxReader()
    {
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        for (auto& d : m_dec) if (d.h) CloseDecompressor(d.h);
    }

    // Open the payload appended to exePath and index it.
    bool Open(const std::wstring& exePath, std::wstring& err)
    {
        m_file = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            err = L"Cannot open the installer file.\n\n" + SfxErrorText(GetLastError());
            return false;
        }
        LARGE_INTEGER fs = {};
        if (!GetFileSizeEx(m_file, &fs) || fs.QuadPart < (LONGLONG)(sizeof(SfxTail) + sizeof(SfxHeader)))
            return Corrupt(err, L"no setup data found");
        const UINT64 fileSize = (UINT64)fs.QuadPart;

        // End of the appended data: the Authenticode certificate table (if
        // the installer is signed) starts right after the SFX tail.
        UINT64 dataEnd = 0, imageEnd = 0;
        if (!OverlayEnd(fileSize, dataEnd, imageEnd, err)) return false;

        // The tail ends on an 8-byte boundary; tolerate up to 7 zero bytes of
        // alignment padding a signing tool may have added after it.
        SfxTail tail = {};
        UINT64 tailOff = 0;
        bool found = false;
        for (UINT64 pad = 0; pad < 8 && !found; pad++) {
            if (dataEnd < imageEnd + sizeof(SfxTail) + pad) break;
            UINT64 off = dataEnd - pad - sizeof(SfxTail);
            if (pad) {
                BYTE z[8] = {};
                if (!ReadAt(dataEnd - pad, z, (DWORD)pad, err)) return false;
                bool zero = true;
                for (UINT64 i = 0; i < pad; i++) if (z[i]) zero = false;
                if (!zero) break;
            }
            if (!ReadAt(off, &tail, sizeof(tail), err)) return false;
            if (tail.magic == NDM_SFX_MAGIC) { tailOff = off; found = true; }
        }
        if (!found)
            return Corrupt(err, L"no setup data found");
        if (tail.version != NDM_SFX_VERSION)
            return Corrupt(err, L"unsupported setup data version " + std::to_wstring(tail.version));
        const UINT64 payloadEnd = tailOff;
        if (tail.payloadOff < imageEnd || tail.payloadOff + sizeof(SfxHeader) > payloadEnd)
            return Corrupt(err, L"bad payload offset");

        // Verify the SHA-256 of the whole payload before trusting any of it.
        // The file is open without FILE_SHARE_WRITE, so it cannot change
        // between this check and extraction.
        if (!VerifySha256(tail.payloadOff, payloadEnd - tail.payloadOff, tail.sha256, err)) return false;

        SfxHeader hdr = {};
        if (!ReadAt(tail.payloadOff, &hdr, sizeof(hdr), err)) return false;
        if (hdr.magic != NDM_SFX_MAGIC || hdr.version != NDM_SFX_VERSION)
            return Corrupt(err, L"bad payload header");
        if (hdr.numEntries > SFX_MAX_ENTRIES)
            return Corrupt(err, L"too many entries");

        UINT64 off = tail.payloadOff + sizeof(SfxHeader);
        for (DWORD i = 0; i < hdr.numEntries; i++)
        {
            SfxEntryHdr eh = {};
            if (off + sizeof(eh) > payloadEnd) return Corrupt(err, L"truncated entry header");
            if (!ReadAt(off, &eh, sizeof(eh), err)) return false;
            off += sizeof(eh);

            eh.relPath[259] = 0;
            SfxIndexEntry e;
            e.relPath   = eh.relPath;
            e.origSize  = eh.origSize;
            e.compSize  = eh.compSize;
            e.dataOff   = off;
            e.crc32     = eh.crc32;
            e.flags     = eh.flags;
            e.method    = eh.method;
            e.blockSize = eh.blockSize;
            e.numBlocks = eh.numBlocks;

            if (!IsSafeRelPath(e.relPath))
                return Corrupt(err, L"unsafe path in entry " + std::to_wstring(i));
            if (e.compSize > payloadEnd - off)
                return Corrupt(err, L"entry exceeds payload: " + e.relPath);
            if (e.origSize > 0) {
                if (e.blockSize == 0 || e.blockSize > SFX_MAX_BLOCK_SIZE)
                    return Corrupt(err, L"bad block size: " + e.relPath);
                UINT64 expect = (e.origSize + e.blockSize - 1) / e.blockSize;
                if (expect != e.numBlocks)
                    return Corrupt(err, L"bad block count: " + e.relPath);
            } else if (e.numBlocks != 0) {
                return Corrupt(err, L"bad block count: " + e.relPath);
            }
            if (e.compSize < (UINT64)e.numBlocks * sizeof(SfxBlockHdr))
                return Corrupt(err, L"bad compressed size: " + e.relPath);

            off += e.compSize;
            totalOrig += e.origSize;
            totalComp += e.compSize;
            entries.push_back(std::move(e));
        }
        // Only the (hashed) alignment padding may follow the last entry.
        if (off > payloadEnd || payloadEnd - off >= 8)
            return Corrupt(err, L"payload size mismatch");
        return true;
    }

    // Extract one entry into an open, writable file handle.
    bool Extract(const SfxIndexEntry& e, HANDLE hOut, SfxProgressFn cb, void* ctx, std::wstring& err)
    {
        cancelled = false;
        if (e.numBlocks == 0) {
            if (e.crc32 != SfxCrc32(nullptr, 0)) return Corrupt(err, L"CRC mismatch: " + e.relPath);
            return true;
        }
        if (!EnsureBuffers(e.blockSize, err)) return false;
        if (!Seek(e.dataOff, err)) return false;

        UINT64 done = 0, consumed = 0;
        DWORD  fileCrc = 0xFFFFFFFFu;
        for (DWORD b = 0; b < e.numBlocks; b++)
        {
            SfxBlockHdr bh = {};
            if (!ReadSeq(&bh, sizeof(bh), err)) return false;
            consumed += sizeof(bh);

            UINT64 remaining = e.origSize - done;
            DWORD  expectLen = (DWORD)(remaining < e.blockSize ? remaining : e.blockSize);
            if (bh.origLen != expectLen)
                return Corrupt(err, L"bad block length in " + e.relPath);
            if (bh.compLen == 0 || bh.compLen > m_inCap || consumed + bh.compLen > e.compSize)
                return Corrupt(err, L"bad compressed block in " + e.relPath);
            if (!ReadSeq(m_in.data(), bh.compLen, err)) return false;
            consumed += bh.compLen;

            const BYTE* plain = nullptr;
            if (bh.method == SFX_METHOD_STORED) {
                if (bh.compLen != bh.origLen)
                    return Corrupt(err, L"bad stored block in " + e.relPath);
                plain = m_in.data();
            } else if (bh.method == SFX_METHOD_XPRESS_HUFF || bh.method == SFX_METHOD_LZMS) {
                DECOMPRESSOR_HANDLE h = GetDecompressor(bh.method, err);
                if (!h) return false;
                SIZE_T got = 0;
                if (!Decompress(h, m_in.data(), bh.compLen, m_out.data(), bh.origLen, &got) ||
                    got != bh.origLen)
                {
                    DWORD le = GetLastError();
                    err = L"Setup data is corrupted (decompression failed in " + e.relPath +
                          L").\n\n" + SfxErrorText(le) + L"\n\nPlease re-download the installer.";
                    return false;
                }
                plain = m_out.data();
            } else {
                return Corrupt(err, L"unknown compression method in " + e.relPath);
            }

            if (SfxCrc32(plain, bh.origLen) != bh.crc32)
                return Corrupt(err, L"CRC mismatch in " + e.relPath + L" (block " +
                                    std::to_wstring(b + 1) + L")");
            fileCrc = SfxCrc32Update(fileCrc, plain, bh.origLen);

            DWORD off = 0;
            while (off < bh.origLen) {
                DWORD wr = 0;
                if (!WriteFile(hOut, plain + off, bh.origLen - off, &wr, nullptr) || wr == 0) {
                    DWORD le = GetLastError();
                    err = L"Failed to write " + e.relPath + L"\n\n" + SfxErrorText(le ? le : ERROR_WRITE_FAULT);
                    return false;
                }
                off += wr;
            }
            done += bh.origLen;
            if (cb && !cb(ctx, e, done)) { cancelled = true; err = L"Cancelled."; return false; }
        }
        if (done != e.origSize || consumed != e.compSize)
            return Corrupt(err, L"size mismatch in " + e.relPath);
        if (~fileCrc != e.crc32)
            return Corrupt(err, L"CRC mismatch: " + e.relPath);
        return true;
    }

    static bool IsSafeRelPath(const std::wstring& p)
    {
        if (p.empty() || p.size() >= 260) return false;
        if (p[0] == L'\\' || p[0] == L'/' || p.find(L':') != std::wstring::npos) return false;
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find_first_of(L"\\/", start);
            if (end == std::wstring::npos) end = p.size();
            std::wstring seg = p.substr(start, end - start);
            if (seg.empty() || seg == L"." || seg == L"..") return false;
            start = end + 1;
        }
        for (wchar_t c : p) if (c < 32 || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return false;
        return true;
    }

private:
    struct Dec { DWORD method; DECOMPRESSOR_HANDLE h; };
    HANDLE            m_file = INVALID_HANDLE_VALUE;
    std::vector<BYTE> m_in, m_out;
    DWORD             m_inCap = 0;
    std::vector<Dec>  m_dec;

    bool Corrupt(std::wstring& err, const std::wstring& what)
    {
        err = L"Setup data is corrupted or incomplete (" + what +
              L").\n\nPlease re-download the installer.";
        return false;
    }
    bool Seek(UINT64 off, std::wstring& err)
    {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(m_file, li, nullptr, FILE_BEGIN)) {
            err = L"Cannot read setup data.\n\n" + SfxErrorText(GetLastError());
            return false;
        }
        return true;
    }
    bool ReadSeq(void* buf, DWORD len, std::wstring& err)
    {
        BYTE* p = static_cast<BYTE*>(buf);
        while (len) {
            DWORD rd = 0;
            if (!ReadFile(m_file, p, len, &rd, nullptr)) {
                err = L"Cannot read setup data.\n\n" + SfxErrorText(GetLastError());
                return false;
            }
            if (rd == 0) return Corrupt(err, L"unexpected end of file");
            p += rd; len -= rd;
        }
        return true;
    }
    bool ReadAt(UINT64 off, void* buf, DWORD len, std::wstring& err)
    {
        return Seek(off, err) && ReadSeq(buf, len, err);
    }

    // Parse the PE headers of this file. dataEnd = file offset of the
    // Authenticode certificate table (IMAGE_DIRECTORY_ENTRY_SECURITY; its
    // "VirtualAddress" is a file offset), or the file size when unsigned.
    // imageEnd = end of the last section's raw data (start of the overlay).
    bool OverlayEnd(UINT64 fileSize, UINT64& dataEnd, UINT64& imageEnd, std::wstring& err)
    {
        IMAGE_DOS_HEADER dos = {};
        if (!ReadAt(0, &dos, sizeof(dos), err)) return false;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            (UINT64)dos.e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fileSize)
            return Corrupt(err, L"bad executable header");
        BYTE nt[sizeof(IMAGE_NT_HEADERS64)] = {};
        if (!ReadAt((UINT64)dos.e_lfanew, nt, sizeof(nt), err)) return false;
        const IMAGE_NT_HEADERS64* h64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt);
        const IMAGE_NT_HEADERS32* h32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt);
        if (h64->Signature != IMAGE_NT_SIGNATURE) return Corrupt(err, L"bad executable header");

        IMAGE_DATA_DIRECTORY sec = {};
        DWORD numDirs = 0;
        if (h64->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            numDirs = h64->OptionalHeader.NumberOfRvaAndSizes;
            if (numDirs > IMAGE_DIRECTORY_ENTRY_SECURITY)
                sec = h64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
        } else if (h64->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            numDirs = h32->OptionalHeader.NumberOfRvaAndSizes;
            if (numDirs > IMAGE_DIRECTORY_ENTRY_SECURITY)
                sec = h32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
        } else {
            return Corrupt(err, L"bad executable header");
        }

        // End of the PE image proper (section headers follow the optional header).
        const IMAGE_FILE_HEADER& fh = h64->FileHeader;
        UINT64 shOff = (UINT64)dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + fh.SizeOfOptionalHeader;
        if (fh.NumberOfSections == 0 || fh.NumberOfSections > 96 ||
            shOff + (UINT64)fh.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > fileSize)
            return Corrupt(err, L"bad executable header");
        std::vector<IMAGE_SECTION_HEADER> sh(fh.NumberOfSections);
        if (!ReadAt(shOff, sh.data(), (DWORD)(sh.size() * sizeof(IMAGE_SECTION_HEADER)), err)) return false;
        imageEnd = shOff + (UINT64)fh.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
        for (const auto& s : sh) {
            UINT64 e = (UINT64)s.PointerToRawData + s.SizeOfRawData;
            if (s.SizeOfRawData && e > imageEnd) imageEnd = e;
        }
        if (imageEnd > fileSize) return Corrupt(err, L"bad executable header");

        if (sec.VirtualAddress == 0 && sec.Size == 0) { dataEnd = fileSize; return true; }
        // A signed image: the certificate table must be the last thing in the
        // file and start after the image itself.
        if (sec.VirtualAddress < imageEnd || sec.Size < 8 ||
            (UINT64)sec.VirtualAddress + sec.Size != fileSize)
            return Corrupt(err, L"bad signature table");
        dataEnd = sec.VirtualAddress;
        return true;
    }

    bool VerifySha256(UINT64 off, UINT64 len, const BYTE expect[32], std::wstring& err)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = false;
        BYTE digest[32] = {};
        std::vector<BYTE> buf;
        try { buf.resize(1u << 20); } catch (...) { err = L"Not enough memory to verify setup data."; return false; }
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)))
        {
            err = L"Cannot verify setup data (SHA-256 unavailable).";
            goto done;
        }
        if (!Seek(off, err)) goto done;
        while (len) {
            DWORD n = (DWORD)(len < buf.size() ? len : buf.size());
            if (!ReadSeq(buf.data(), n, err)) goto done;
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), n, 0))) {
                err = L"Cannot verify setup data (SHA-256 failed).";
                goto done;
            }
            len -= n;
        }
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            err = L"Cannot verify setup data (SHA-256 failed).";
            goto done;
        }
        {
            BYTE diff = 0;
            for (int i = 0; i < 32; i++) diff |= (BYTE)(digest[i] ^ expect[i]);
            if (diff) { Corrupt(err, L"SHA-256 mismatch"); goto done; }
        }
        ok = true;
    done:
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }
    bool EnsureBuffers(DWORD blockSize, std::wstring& err)
    {
        // Compressed blocks are only kept when smaller than the original, so
        // blockSize bytes always suffice for the input buffer.
        if (m_inCap >= blockSize) return true;
        try {
            m_in.assign(blockSize, 0);
            m_out.assign(blockSize, 0);
        } catch (...) {
            err = L"Not enough memory to extract setup data.";
            return false;
        }
        m_inCap = blockSize;
        return true;
    }
    DECOMPRESSOR_HANDLE GetDecompressor(DWORD method, std::wstring& err)
    {
        for (auto& d : m_dec) if (d.method == method) return d.h;
        DECOMPRESSOR_HANDLE h = nullptr;
        if (!CreateDecompressor(method, nullptr, &h)) {
            err = L"Cannot initialise decompression (Windows 8 or later is required).\n\n" +
                  SfxErrorText(GetLastError());
            return nullptr;
        }
        m_dec.push_back({ method, h });
        return h;
    }
};
