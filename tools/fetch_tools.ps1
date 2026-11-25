# ============================================================
# fetch_tools.ps1
# Downloads the media helpers NDM ships in <install dir>\tools\:
#   yt-dlp.exe              (official yt-dlp release)
#   ffmpeg.exe, ffprobe.exe + their DLLs (BtbN FFmpeg-Builds, win64 LGPL shared build)
#   deno.exe                (official Deno release; JS runtime yt-dlp needs for YouTube)
# into third_party\ (or -OutDir).
#
# Default mode is PINNED and fails closed: exact release assets (fixed tags,
# never "latest"), the SHA-256 of every downloaded archive AND of every file
# taken out of it are recorded below. Any mismatch aborts; nothing is written
# to -OutDir unless every hash matches. Files already in -OutDir are checked
# against the same pins (a mismatch is an error unless -Force re-fetches).
# Nothing from the downloads is executed by this script.
#
# -Update (maintainer only): fetches the LATEST releases, verifies them against
# the checksums published with each release, writes them to -OutDir and
# prints a new pin block to paste into this script. Review before committing.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\fetch_tools.ps1 [-OutDir <dir>] [-Force]
#   powershell -ExecutionPolicy Bypass -File tools\fetch_tools.ps1 -Update [-OutDir <dir>]
# ============================================================
param(
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "third_party"),
    [switch]$Force,
    [switch]$Update
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"   # Invoke-WebRequest is 10x slower with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
# PINS (recorded 2026-11-24 from the files in third_party\)
# ---------------------------------------------------------------------------
$Pins = @(
    @{
        Name    = "yt-dlp"
        Tag     = "2026.11.23"                     # github.com/yt-dlp/yt-dlp release tag
        Url     = "https://github.com/yt-dlp/yt-dlp/releases/download/2026.11.23/yt-dlp.exe"
        Archive = $null                            # plain exe, no archive
        Sha256  = "622ed48023cac1daa52a3bf3a6ac38f03514de214b5f0fa069389fd0f6d17e5c"
        Files   = [ordered]@{
            "yt-dlp.exe" = "622ed48023cac1daa52a3bf3a6ac38f03514de214b5f0fa069389fd0f6d17e5c"
        }
    },
    @{
        Name    = "ffmpeg"
        Tag     = "autobuild-2026-11-23-12-43"     # github.com/BtbN/FFmpeg-Builds (FFmpeg N-129241-g05c674c913)
        Url     = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-11-23-12-43/ffmpeg-N-129241-g05c674c913-win64-lgpl-shared.zip"
        Archive = "zip"
        Sha256  = "824507d0254cbc1b52ab7ee4ef402c62e61807ed21d99bea7cdeaeb01eabc3c2"
        Files   = [ordered]@{                      # zip entry bin/<name> -> SHA-256
            "ffmpeg.exe"       = "b13119e47eb081abda3d96e5bd9628c469dcd42196805b4ab17690b3bf1adaf7"
            "ffprobe.exe"      = "58d3d9316e0b220c582ec8005ec0d626c1ac104e7689b18bbfaaa62c75770c75"
            "avcodec-63.dll"   = "8f2c985a539df2d0604adf840b251d38bc640eb12a032a04a3468f277a729e96"
            "avdevice-63.dll"  = "1e6eb04595f15c4bedc73795561e9ba7b6e257c0365bb8935025ac4360418103"
            "avfilter-12.dll"  = "3753dc4df23d64c924369d4bda3a6a72e58ccb64fdd966bf03be1362e6d92cff"
            "avformat-63.dll"  = "040a7182027ec3a3b82e3ddae12d1610c557d6f8a5f5c28c6466187e68db331f"
            "avutil-61.dll"    = "9fe0fc2288b5fc5b30589f0132d084df9022506707c374d2530a777b65074766"
            "swresample-7.dll" = "31c3370f47bb4f6ed271042175c539d6d47c2695e7a419160575ef51bedb9e15"
            "swscale-10.dll"   = "6eb7c6831fe306cefcaaf12b48a0eee5c04e7c6ee9a2e4361568994123cca265"
        }
    },
    @{
        Name    = "deno"
        Tag     = "v2.9.8"                         # github.com/denoland/deno release tag
        Url     = "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip"
        Archive = "zip"
        Sha256  = "cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf"
        Files   = [ordered]@{
            "deno.exe" = "924397800533d3eb0ed0c889b20ad70e18b970d93acd8135bc043f425e528266"
        }
    }
)

# Latest-release sources (only for -Update)
$Latest = @{
    "yt-dlp" = @{ Url  = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
                  Sums = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS"
                  SumName = "yt-dlp.exe" }
    "ffmpeg" = @{ Url  = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
                  Sums = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/checksums.sha256"
                  SumName = "ffmpeg-master-latest-win64-lgpl-shared.zip" }
    "deno"   = @{ Url  = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"
                  Sums = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum"
                  SumName = $null }
}

function Get-File([string]$Url, [string]$Dest) {
    if ($Url -notmatch '^https://github\.com/') { throw "Refusing non-GitHub URL: $Url" }
    Write-Host "  GET $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -UserAgent "NDM-fetch-tools"
}

function Get-Sha([string]$File) { (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant() }

function Assert-Hash([string]$File, [string]$Expected, [string]$Label) {
    if (-not $Expected -or $Expected -notmatch '^[0-9a-f]{64}$') { throw "No SHA-256 pinned for $Label - refusing to use it." }
    $actual = Get-Sha $File
    if ($actual -ne $Expected) { throw "SHA-256 mismatch for $Label`n  expected $Expected`n  actual   $actual" }
    Write-Host "  OK  sha256 $Label = $actual"
}

# "<hex>  <name>" / "<hex> *<name>" lines -> hash for $Name; $Name = $null: first 64-hex token
function Find-Sum([string]$SumsFile, [string]$Name) {
    if (-not $Name) {
        $m = [regex]::Match((Get-Content -LiteralPath $SumsFile -Raw), '\b[0-9a-fA-F]{64}\b')
        if ($m.Success) { return $m.Value.ToLowerInvariant() } else { return $null }
    }
    foreach ($line in Get-Content -LiteralPath $SumsFile) {
        if ($line -match '^\s*([0-9a-fA-F]{64})\s+\*?(.+?)\s*$' -and $Matches[2] -eq $Name) {
            return $Matches[1].ToLowerInvariant()
        }
    }
    return $null
}

# Extract the wanted leaf names (flat) from a zip into $Dir. Returns name -> path.
function Expand-Wanted([string]$Zip, [string[]]$Names, [string]$Dir, [string]$Pattern) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $out = [ordered]@{}
    $za = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $za.Entries) {
            $leaf = $e.Name
            $want = if ($Names) { $Names -contains $leaf } else { $e.FullName -match $Pattern }
            if (-not $want -or $out.Contains($leaf)) { continue }
            if ($leaf -notmatch '^[A-Za-z0-9_.-]+$') { throw "Unexpected entry name $($e.FullName)" }
            if ($e.Length -le 0 -or $e.Length -gt 512MB) { throw "Suspicious entry size for $leaf" }
            $dest = Join-Path $Dir $leaf
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
            $out[$leaf] = $dest
        }
    } finally { $za.Dispose() }
    return $out
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("ndm_tools_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    if ($Update) {
        # ---------------- maintainer: latest releases -> new pins ----------------
        $report = @()
        foreach ($p in $Pins) {
            $src = $Latest[$p.Name]
            Write-Host "$($p.Name) (latest):"
            $dir = Join-Path $work $p.Name; New-Item -ItemType Directory -Path $dir | Out-Null
            $sums = Join-Path $dir "sums.txt"
            Get-File $src.Sums $sums
            $leafName = [IO.Path]::GetFileName(([uri]$src.Url).AbsolutePath)
            $dl = Join-Path $dir $leafName
            Get-File $src.Url $dl
            $archSha = Get-Sha $dl
            Assert-Hash $dl (Find-Sum $sums $(if ($src.SumName) { $src.SumName } else { $null })) $leafName
            $files = [ordered]@{}
            if ($p.Archive -eq "zip") {
                $x = Join-Path $dir "x"; New-Item -ItemType Directory -Path $x | Out-Null
                $pat = if ($p.Name -eq "ffmpeg") { '(^|/)bin/((ffmpeg|ffprobe)\.exe|[A-Za-z0-9_.-]+\.dll)$' } else { '(^|/)deno\.exe$' }
                $got = Expand-Wanted $dl $null $x $pat
                foreach ($k in $got.Keys) { $files[$k] = $got[$k] }
            } else {
                $files[$leafName] = $dl
            }
            if ($p.Name -eq "ffmpeg") {
                Get-ChildItem -LiteralPath $OutDir -Filter "*.dll" | Where-Object { $_.Name -match '^(av|sw|postproc)' } | Remove-Item -Force
            }
            $pinFiles = @()
            foreach ($k in $files.Keys) {
                Copy-Item -LiteralPath $files[$k] -Destination (Join-Path $OutDir $k) -Force
                $pinFiles += ('            "{0}" = "{1}"' -f $k, (Get-Sha $files[$k]))
            }
            $report += "  $($p.Name): archive sha256 $archSha"
            $report += $pinFiles
        }
        Write-Host ""
        Write-Host "NEW PINS (update Tag/Url/Sha256/Files above; find the fixed release tag of each asset on GitHub):"
        $report | ForEach-Object { Write-Host $_ }
        return
    }

    # ---------------- pinned (default) ----------------
    foreach ($p in $Pins) {
        $present = $true
        foreach ($k in $p.Files.Keys) {
            $f = Join-Path $OutDir $k
            if (-not (Test-Path -LiteralPath $f)) { $present = $false; continue }
            if (-not $Force) { Assert-Hash $f $p.Files[$k] "$OutDir\$k (present)" }
        }
        if ($present -and -not $Force) { Write-Host "$($p.Name) $($p.Tag) already present and verified (use -Force to re-fetch)"; continue }

        Write-Host "$($p.Name) $($p.Tag):"
        $dir = Join-Path $work $p.Name; New-Item -ItemType Directory -Path $dir | Out-Null
        $leafName = [IO.Path]::GetFileName(([uri]$p.Url).AbsolutePath)
        $dl = Join-Path $dir $leafName
        Get-File $p.Url $dl
        Assert-Hash $dl $p.Sha256 $leafName

        $staged = [ordered]@{}
        if ($p.Archive -eq "zip") {
            $x = Join-Path $dir "x"; New-Item -ItemType Directory -Path $x | Out-Null
            $staged = Expand-Wanted $dl ([string[]]@($p.Files.Keys)) $x $null
        } else {
            $staged[$leafName] = $dl
        }
        # Every pinned file must be there and match before anything is copied.
        foreach ($k in $p.Files.Keys) {
            if (-not $staged.Contains($k)) { throw "$k not found in $leafName" }
            Assert-Hash $staged[$k] $p.Files[$k] $k
        }
        foreach ($k in $p.Files.Keys) {
            Copy-Item -LiteralPath $staged[$k] -Destination (Join-Path $OutDir $k) -Force
            Write-Host "  -> $(Join-Path $OutDir $k)"
        }
    }
    Write-Host "Done. Tools in $OutDir (all SHA-256 pins verified)"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
