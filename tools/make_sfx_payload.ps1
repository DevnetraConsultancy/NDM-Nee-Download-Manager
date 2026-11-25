# ============================================================
# make_sfx_payload.ps1
# Builds the NDM-SFX v3 payload and appends it to the stub EXE.
# Format: see src\installer\ndm_sfx.h
#
#   - Files are split into blocks (default 8 MB) and every block is
#     compressed independently with the Windows Compression API
#     (cabinet.dll): LZMS for larger files, XPRESS_HUFF for small ones.
#     A block that does not shrink is stored.
#   - CRC-32 of every block and of every whole file.
#   - Streams to disk; memory stays bounded (a few blocks per CPU).
#   - SHA-256 of the whole payload in the tail (verified by the stub before
#     extraction). The tail is padded to end on an 8-byte boundary so the
#     final EXE can be Authenticode-signed (signtool) AFTER packing; the stub
#     finds the tail in front of the certificate table.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File make_sfx_payload.ps1 `
#       -StubExe   "build\bin\ndmsetup643build1.exe" `
#       -OutputExe "dist\NDM-1.0.6-x64-Setup.exe"    `
#       -BinDir    "build\bin"                        `
#       -ExtDir    "src\browser_ext"                  `
#       [-ThirdPartyDir "third_party"] [-ExtraFile <path>[,<path>...]] [-BlockSizeMB 8]
#
# third_party\ (yt-dlp.exe, ffmpeg.exe, ffprobe.exe, deno.exe, licence
# texts) is packed into <install dir>\tools\. If it is missing or empty the
# build still succeeds with a warning.
# -ExtraFile packs additional files into tools\ (testing).
# ============================================================
param(
    [Parameter(Mandatory=$true)][string]$StubExe,
    [Parameter(Mandatory=$true)][string]$OutputExe,
    [Parameter(Mandatory=$true)][string]$BinDir,
    [Parameter(Mandatory=$true)][string]$ExtDir,
    [string]$ThirdPartyDir = "",
    [string[]]$ExtraFile = @(),
    [ValidateRange(1, 32)][int]$BlockSizeMB = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $ThirdPartyDir) { $ThirdPartyDir = Join-Path (Split-Path $PSScriptRoot) "third_party" }

Add-Type @"
using System;
using System.IO;
using System.Threading.Tasks;
using System.Runtime.InteropServices;

public static class SfxPack {
    public const uint MAGIC   = 0x584D444E; // 'NDMX'
    public const uint VERSION = 3;
    public const uint STORED = 0, XPRESS_HUFF = 4, LZMS = 5;
    public const int  ENTRY_HDR_SIZE = 560;

    [DllImport("cabinet.dll", SetLastError = true)]
    static extern bool CreateCompressor(uint algorithm, IntPtr allocationRoutines, out IntPtr handle);
    [DllImport("cabinet.dll", SetLastError = true)]
    static extern bool Compress(IntPtr handle, byte[] uncompressed, UIntPtr uncompressedSize,
                                byte[] compressed, UIntPtr compressedBufferSize, out UIntPtr compressedDataSize);
    [DllImport("cabinet.dll", SetLastError = true)]
    static extern bool CloseCompressor(IntPtr handle);

    static readonly uint[] table = MakeTable();
    static uint[] MakeTable() {
        uint[] t = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }
    public static uint CrcUpdate(uint crc, byte[] data, int off, int len) {
        for (int i = off; i < off + len; i++) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
        return crc;
    }

    class Block {
        public byte[] Orig; public int OrigLen;
        public byte[] Comp; public int CompLen;
        public uint Method; public uint Crc;
    }

    static void CompressBlock(Block b, uint method) {
        b.Crc = ~CrcUpdate(0xFFFFFFFFu, b.Orig, 0, b.OrigLen);
        b.Method = STORED; b.Comp = b.Orig; b.CompLen = b.OrigLen;
        if (method == STORED || b.OrigLen < 64) return;
        IntPtr h;
        if (!CreateCompressor(method, IntPtr.Zero, out h))
            throw new Exception("CreateCompressor failed: " + Marshal.GetLastWin32Error());
        try {
            byte[] input = b.Orig;
            if (input.Length != b.OrigLen) { input = new byte[b.OrigLen]; Buffer.BlockCopy(b.Orig, 0, input, 0, b.OrigLen); }
            byte[] outBuf = new byte[b.OrigLen + b.OrigLen / 2 + 65536];
            UIntPtr got;
            if (Compress(h, input, new UIntPtr((uint)b.OrigLen), outBuf, new UIntPtr((uint)outBuf.Length), out got)) {
                int n = (int)got.ToUInt32();
                if (n > 0 && n < b.OrigLen) { b.Comp = outBuf; b.CompLen = n; b.Method = method; }
            }
        } finally { CloseCompressor(h); }
    }

    static void W32(Stream s, uint v) { s.Write(BitConverter.GetBytes(v), 0, 4); }
    static void W64(Stream s, ulong v) { s.Write(BitConverter.GetBytes(v), 0, 8); }

    static void WriteEntryHeader(Stream s, ulong origSize, ulong compSize, uint crc, uint flags,
                                 uint method, uint blockSize, uint numBlocks, string rel) {
        W64(s, origSize); W64(s, compSize); W32(s, crc); W32(s, flags);
        W32(s, method); W32(s, blockSize); W32(s, numBlocks); W32(s, 0);
        byte[] buf = new byte[520];
        byte[] src = System.Text.Encoding.Unicode.GetBytes(rel);
        if (src.Length > 518) throw new Exception("Path too long: " + rel);
        Array.Copy(src, buf, src.Length);
        s.Write(buf, 0, 520);
    }

    public static void WritePayloadHeader(Stream s, uint numEntries) {
        W32(s, MAGIC); W32(s, VERSION); W32(s, numEntries); W32(s, 0);
    }
    // Zero padding so that the 48-byte tail ends on an 8-byte boundary, then
    // the tail with the SHA-256 of [payloadOff, tail).
    public static byte[] FinishPayload(FileStream s, ulong payloadOff) {
        long pad = (8 - ((s.Position + 48) % 8)) % 8;
        if (pad > 0) s.Write(new byte[pad], 0, (int)pad);
        long tailPos = s.Position;
        s.Flush();
        byte[] hash;
        s.Position = (long)payloadOff;
        using (var sha = System.Security.Cryptography.SHA256.Create()) {
            byte[] buf = new byte[1 << 20];
            long left = tailPos - (long)payloadOff;
            while (left > 0) {
                int n = s.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (n <= 0) throw new Exception("Short read while hashing the payload");
                sha.TransformBlock(buf, 0, n, null, 0);
                left -= n;
            }
            sha.TransformFinalBlock(buf, 0, 0);
            hash = sha.Hash;
        }
        s.Position = tailPos;
        W32(s, MAGIC); W32(s, VERSION); W64(s, payloadOff);
        s.Write(hash, 0, 32);
        return hash;
    }

    // True if the PE image already carries an Authenticode certificate table.
    public static bool IsSigned(byte[] pe) {
        int lfanew = BitConverter.ToInt32(pe, 0x3C);
        ushort magic = BitConverter.ToUInt16(pe, lfanew + 24);
        int dirOff = lfanew + 24 + (magic == 0x20b ? 112 : 96) + 4 * 8;   // DataDirectory[SECURITY]
        return BitConverter.ToUInt32(pe, dirOff) != 0 || BitConverter.ToUInt32(pe, dirOff + 4) != 0;
    }

    // Streams one file into the payload. Returns the compressed size
    // (block headers + data) written after the entry header.
    public static long WriteEntry(FileStream outS, string path, string rel, uint flags,
                                  uint method, int blockSize) {
        long hdrPos = outS.Position;
        using (FileStream inS = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
            long size = inS.Length;
            uint numBlocks = (uint)((size + blockSize - 1) / blockSize);
            WriteEntryHeader(outS, (ulong)size, 0, 0, flags, method, (uint)blockSize, numBlocks, rel); // patched below
            uint fileCrc = 0xFFFFFFFFu;
            long comp = 0, done = 0;
            int par = Math.Max(1, Environment.ProcessorCount);
            while (done < size) {
                // read up to `par` blocks, compress them in parallel, write in order
                var batch = new System.Collections.Generic.List<Block>();
                while (batch.Count < par && done < size) {
                    int len = (int)Math.Min(blockSize, size - done);
                    Block b = new Block(); b.Orig = new byte[len]; b.OrigLen = len;
                    int off = 0;
                    while (off < len) {
                        int r = inS.Read(b.Orig, off, len - off);
                        if (r <= 0) throw new Exception("Unexpected end of file: " + path);
                        off += r;
                    }
                    done += len;
                    batch.Add(b);
                }
                Parallel.ForEach(batch, b => CompressBlock(b, method));
                foreach (Block b in batch) {
                    fileCrc = CrcUpdate(fileCrc, b.Orig, 0, b.OrigLen);
                    W32(outS, (uint)b.OrigLen); W32(outS, (uint)b.CompLen); W32(outS, b.Crc); W32(outS, b.Method);
                    outS.Write(b.Comp, 0, b.CompLen);
                    comp += 16 + b.CompLen;
                }
            }
            if (inS.Length != size) throw new Exception("File changed while packing: " + path);
            long end = outS.Position;
            outS.Position = hdrPos;
            WriteEntryHeader(outS, (ulong)size, (ulong)comp, ~fileCrc, flags, method, (uint)blockSize, numBlocks, rel);
            outS.Position = end;
            return comp;
        }
    }
}
"@

# ---- Collect files to pack ------------------------------------------------
$files = [System.Collections.Generic.List[pscustomobject]]::new()

function Add-File([string]$path, [string]$rel, [uint32]$flags=0) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing file: $path" }
    $files.Add([pscustomobject]@{Path=(Resolve-Path -LiteralPath $path).Path; Rel=$rel; Flags=$flags})
}

# Core binaries (go into install root)
Add-File "$BinDir\NDMan.exe"       "NDMan.exe"       1   # SFX_FLAG_EXEC
Add-File "$BinDir\NDMMsgHost.exe"  "NDMMsgHost.exe"  1
Add-File "$BinDir\NDMShellExt.dll" "NDMShellExt.dll" 1
Add-File "$BinDir\Uninstall.exe"   "Uninstall.exe"   1

# Browser extensions (unpacked; Chrome/Edge "Load unpacked" from ext\chrome,
# Firefox from ext\firefox). Every file in the folder tree is packed, so new
# pages/scripts are picked up automatically. Key material is never packed.
$secretPattern = '\.(pem|key|crx|der|p12|pfx)$'
foreach ($browser in @("chrome", "firefox")) {
    $root = Join-Path $ExtDir $browser
    if (-not (Test-Path $root)) { throw "Missing extension folder: $root" }
    $rootFull = (Resolve-Path $root).Path.TrimEnd('\')
    foreach ($f in Get-ChildItem $root -File -Recurse) {
        if ($f.Name -match $secretPattern) { Write-Host "  Skipping secret: $($f.FullName)"; continue }
        $rel = $f.FullName.Substring($rootFull.Length + 1)
        Add-File $f.FullName "ext\$browser\$rel" 0
    }
}

# Native messaging host manifests (templates; the installer regenerates both
# with the real install path and registers them for Chrome/Edge/Chromium/Firefox)
$msgHostDir = Join-Path (Split-Path $ExtDir) "msghost"
foreach ($name in @("NDMMsgHost_chrome.json", "NDMMsgHost_firefox.json")) {
    $j = Join-Path $msgHostDir $name
    if (-not (Test-Path $j)) { throw "Missing native host manifest: $j" }
    Add-File $j $name 0
}

# Third-party video tools -> <install dir>\tools\
$toolCount = 0
if (Test-Path -LiteralPath $ThirdPartyDir -PathType Container) {
    $tpFull = (Resolve-Path -LiteralPath $ThirdPartyDir).Path.TrimEnd('\')
    foreach ($f in Get-ChildItem -LiteralPath $tpFull -File -Recurse) {
        $isTool    = ($f.Extension -ieq ".exe") -or ($f.Extension -ieq ".dll")   # shared FFmpeg DLLs
        $isLicence = ($f.Extension -ieq ".txt") -or ($f.Name -match '^(LICEN[CS]E|COPYING|NOTICE)')
        if (-not ($isTool -or $isLicence)) { continue }
        if ($f.Length -eq 0) { Write-Warning "Skipping empty file: $($f.FullName)"; continue }
        # A file still being written (download in progress) cannot be opened
        # with read-only sharing: skip it rather than ship a truncated tool.
        try { $s = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'Read'); $s.Dispose() }
        catch { Write-Warning "Skipping file in use (still downloading?): $($f.FullName)"; continue }
        $rel = $f.FullName.Substring($tpFull.Length + 1)
        Add-File $f.FullName "tools\$rel" ([uint32]($(if ($isTool) { 1 } else { 0 }) -bor 4))  # EXEC | TOOL
        $toolCount++
    }
}
if ($toolCount -eq 0) {
    Write-Warning "No third-party tools found in '$ThirdPartyDir' - building the installer WITHOUT yt-dlp/ffmpeg (tools\ will be empty)."
}
# (powershell -File passes "a,b" as one string: split it)
foreach ($x in @($ExtraFile | ForEach-Object { $_ -split '[,;]' })) {
    if ($x) { Add-File $x ("tools\" + (Split-Path $x -Leaf)) 4 }
}

# Licences: NDM is MIT (LICENSE -> LICENSE.txt). THIRD_PARTY_NOTICES.txt is the
# repo template plus a generated list of the exact bundled tool binaries.
$repoRoot = Split-Path $PSScriptRoot
Add-File (Join-Path $repoRoot "LICENSE") "LICENSE.txt" 0
$noticeSrc = Join-Path $repoRoot "THIRD_PARTY_NOTICES.txt"
if (-not (Test-Path -LiteralPath $noticeSrc)) { throw "Missing $noticeSrc" }
$noticeTmp = Join-Path ([System.IO.Path]::GetTempPath()) ("ndm_notices_{0}.txt" -f [guid]::NewGuid().ToString("N"))
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append([System.IO.File]::ReadAllText($noticeSrc).TrimEnd()).Append("`r`n`r`n")
[void]$sb.Append("-------------------------------------------------------------------------------`r`n")
[void]$sb.Append("Bundled tool files (generated by make_sfx_payload.ps1, $((Get-Date).ToString('yyyy-MM-dd')))`r`n")
[void]$sb.Append("-------------------------------------------------------------------------------`r`n")
$toolFiles = @($files | Where-Object { $_.Rel -like 'tools\*' })
if ($toolFiles.Count -eq 0) { [void]$sb.Append("(none - this installer was built without third-party tools)`r`n") }
foreach ($t in $toolFiles) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $t.Path).Hash.ToLowerInvariant()
    [void]$sb.Append(("{0,-28} {1,12:N0} bytes  SHA-256 {2}`r`n" -f $t.Rel, (Get-Item -LiteralPath $t.Path).Length, $h))
}
[System.IO.File]::WriteAllText($noticeTmp, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Add-File $noticeTmp "THIRD_PARTY_NOTICES.txt" 0

# Safety net: never ship private keys; no duplicate destinations.
foreach ($f in $files) {
    if ($f.Path -match $secretPattern -or $f.Path -match '[\/]keys[\/]') {
        throw "Refusing to pack key material: $($f.Path)"
    }
}
$dups = $files | Group-Object { $_.Rel.ToLowerInvariant() } | Where-Object Count -gt 1
if ($dups) { throw "Duplicate payload paths: $(($dups | ForEach-Object Name) -join ', ')" }

# ---- Write output EXE: stub + payload + tail ------------------------------
$blockSize = $BlockSizeMB * 1MB
$outFull = [System.IO.Path]::GetFullPath($OutputExe)
$outDir = Split-Path $outFull
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$tmpOut = "$outFull.partial"
if (Test-Path -LiteralPath $tmpOut) { Remove-Item -LiteralPath $tmpOut -Force }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$stubBytes = [System.IO.File]::ReadAllBytes((Resolve-Path $StubExe).Path)
# Sign the FINAL exe (after packing), never the stub: a certificate table in
# the middle of the file would no longer be the last thing in the image.
if ([SfxPack]::IsSigned($stubBytes)) { throw "Stub is already Authenticode-signed: sign the packed output instead." }
$out = New-Object System.IO.FileStream($tmpOut, 'CreateNew', 'ReadWrite', 'None', 1MB)
$totalOrig = [long]0; $totalComp = [long]0
try {
    $out.Write($stubBytes, 0, $stubBytes.Length)
    $payloadOffset = [uint64]$out.Position
    [SfxPack]::WritePayloadHeader($out, [uint32]$files.Count)

    Write-Host "Packing $($files.Count) files into SFX v3 payload (block size $BlockSizeMB MB)..."
    foreach ($f in $files) {
        $len = (Get-Item -LiteralPath $f.Path).Length
        # LZMS for anything sizeable (best ratio on executables), XPRESS_HUFF for small files
        $method = if ($len -ge 512KB) { [SfxPack]::LZMS } else { [SfxPack]::XPRESS_HUFF }
        $t0 = $sw.Elapsed.TotalSeconds
        $comp = [SfxPack]::WriteEntry($out, $f.Path, $f.Rel, $f.Flags, $method, $blockSize)
        $totalOrig += $len; $totalComp += $comp
        $ratio = if ($len) { 100.0 * $comp / $len } else { 100 }
        $mname = if ($method -eq 5) { "LZMS" } else { "XPRESS_HUFF" }
        if ($len -ge 256KB) {
            Write-Host ("  {0,-44} {1,10:N1} KB -> {2,10:N1} KB  ({3,5:N1}%, {4}, {5:N1}s)" -f $f.Rel, ($len/1KB), ($comp/1KB), $ratio, $mname, ($sw.Elapsed.TotalSeconds - $t0))
        }
    }
    $payloadHash = [SfxPack]::FinishPayload($out, $payloadOffset)
    $payloadBytes = $out.Position - $stubBytes.Length
}
finally { $out.Dispose(); Remove-Item -LiteralPath $noticeTmp -Force -ErrorAction SilentlyContinue }

Move-Item -LiteralPath $tmpOut -Destination $outFull -Force
$sw.Stop()

$finalSz = [math]::Round((Get-Item $outFull).Length/1MB, 2)
Write-Host ""
Write-Host "SUCCESS: $outFull"
Write-Host ("  Files:   {0} ({1} third-party)" -f $files.Count, $toolCount)
Write-Host ("  Stub:    {0:N1} KB" -f ($stubBytes.Length/1KB))
Write-Host ("  Data:    {0:N2} MB original -> {1:N2} MB compressed ({2:N1}%)" -f ($totalOrig/1MB), ($totalComp/1MB), $(if ($totalOrig) { 100.0*$totalComp/$totalOrig } else { 0 }))
Write-Host ("  Payload: {0:N2} MB  SHA-256 {1}" -f ($payloadBytes/1MB), (($payloadHash | ForEach-Object { $_.ToString("x2") }) -join ''))
Write-Host ("  Total:   {0} MB  ({1:N1}s)" -f $finalSz, $sw.Elapsed.TotalSeconds)
