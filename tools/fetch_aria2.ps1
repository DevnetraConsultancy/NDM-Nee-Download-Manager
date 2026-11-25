# ============================================================
# fetch_aria2.ps1
# Downloads aria2c.exe (BitTorrent / magnet engine NDM ships in
# <install dir>\tools\) from the official aria2 GitHub release and extracts
# ONLY aria2c.exe and its licence (COPYING -> aria2-COPYING.txt) into
# third_party\ (or -OutDir).
#
# Default mode is PINNED and fails closed: a fixed release tag/asset, the
# SHA-256 of the zip AND of both extracted files are recorded below; any
# mismatch aborts before anything is written. Files already in -OutDir are
# checked against the same pins (mismatch = error unless -Force).
# Nothing from the archive is executed.
#
# -Update (maintainer only): fetches the LATEST release (aria2 publishes no
# checksums: verified by the GitHub API size/digest), writes it to -OutDir and
# prints new pins to paste here. Review before committing.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\fetch_aria2.ps1 [-OutDir <dir>] [-Force]
#   powershell -ExecutionPolicy Bypass -File tools\fetch_aria2.ps1 -Update [-OutDir <dir>]
# ============================================================
param(
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "third_party"),
    [switch]$Force,
    [switch]$Update
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
# PINS (recorded 2026-11-24 from the files in third_party\)
# ---------------------------------------------------------------------------
$Tag     = "release-1.37.0"
$Asset   = "aria2-1.37.0-win-64bit-build1.zip"
$Url     = "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip"
$ZipSha  = "67d015301eef0b612191212d564c5bb0a14b5b9c4796b76454276a4d28d9b288"
$ZipSize = 2475379
$FileSha = [ordered]@{
    "aria2c.exe" = "be2099c214f63a3cb4954b09a0becd6e2e34660b886d4c898d260febfe9d70c2"
    "COPYING"    = "8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643"   # -> aria2-COPYING.txt
}
$OutName = @{ "aria2c.exe" = "aria2c.exe"; "COPYING" = "aria2-COPYING.txt" }

function Get-Sha([string]$File) { (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant() }

function Assert-Hash([string]$File, [string]$Expected, [string]$Label) {
    if (-not $Expected -or $Expected -notmatch '^[0-9a-f]{64}$') { throw "No SHA-256 pinned for $Label - refusing to use it." }
    $actual = Get-Sha $File
    if ($actual -ne $Expected) { throw "SHA-256 mismatch for $Label`n  expected $Expected`n  actual   $actual" }
    Write-Host "  OK  sha256 $Label = $actual"
}

# Extract aria2c.exe + COPYING (leaf names only; archive paths are never used).
function Expand-Aria2([string]$Zip, [string]$Dir) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $got = @{}
    $za = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $za.Entries) {
            $leaf = [IO.Path]::GetFileName($e.FullName)
            if ($OutName.ContainsKey($leaf) -and -not $got.ContainsKey($leaf)) {
                if ($e.Length -le 0 -or $e.Length -gt 64MB) { throw "Suspicious entry size for $leaf" }
                $tmp = Join-Path $Dir ("x_" + $OutName[$leaf])
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $tmp, $true)
                $got[$leaf] = $tmp
            }
        }
    } finally { $za.Dispose() }
    if (-not $got.ContainsKey("aria2c.exe")) { throw "aria2c.exe not found in archive" }
    $hdr = [IO.File]::ReadAllBytes($got["aria2c.exe"])[0..1]
    if ($hdr[0] -ne 0x4D -or $hdr[1] -ne 0x5A) { throw "aria2c.exe is not a PE file" }
    return $got
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("ndm_aria2_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    if ($Update) {
        Write-Host "Querying latest aria2 release..."
        $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/aria2/aria2/releases/latest" -UserAgent "NDM-fetch-aria2"
        $a = $rel.assets | Where-Object { $_.name -match '^aria2-[0-9.]+-win-64bit-build[0-9]+\.zip$' } | Select-Object -First 1
        if (-not $a) { throw "No win-64bit zip in release $($rel.tag_name)" }
        $u = [string]$a.browser_download_url
        if ($u -notmatch '^https://github\.com/aria2/aria2/releases/download/') { throw "Unexpected download URL: $u" }
        $zip = Join-Path $work $a.name
        Write-Host "  GET $u"
        Invoke-WebRequest -Uri $u -OutFile $zip -UseBasicParsing -UserAgent "NDM-fetch-aria2"
        if ((Get-Item -LiteralPath $zip).Length -ne [int64]$a.size) { throw "Size mismatch for $($a.name)" }
        $sha = Get-Sha $zip
        $digest = if ($a.PSObject.Properties['digest'] -and $a.digest) { [string]$a.digest } else { "" }
        if ($digest -match '^sha256:([0-9a-f]{64})$' -and $Matches[1] -ne $sha) { throw "GitHub digest mismatch for $($a.name)" }
        $got = Expand-Aria2 $zip $work
        foreach ($k in $got.Keys) { Copy-Item -LiteralPath $got[$k] -Destination (Join-Path $OutDir $OutName[$k]) -Force }
        Write-Host ""
        Write-Host "NEW PINS for $($rel.tag_name):"
        Write-Host "  `$Tag     = `"$($rel.tag_name)`""
        Write-Host "  `$Asset   = `"$($a.name)`""
        Write-Host "  `$Url     = `"$u`""
        Write-Host "  `$ZipSha  = `"$sha`""
        Write-Host "  `$ZipSize = $($a.size)"
        foreach ($k in $got.Keys) { Write-Host ("  `"{0}`" = `"{1}`"" -f $k, (Get-Sha $got[$k])) }
        return
    }

    # ---------------- pinned (default) ----------------
    $present = $true
    foreach ($k in $FileSha.Keys) {
        $f = Join-Path $OutDir $OutName[$k]
        if (-not (Test-Path -LiteralPath $f)) { $present = $false; continue }
        if (-not $Force) { Assert-Hash $f $FileSha[$k] "$f (present)" }
    }
    if ($present -and -not $Force) {
        Write-Host "aria2 $Tag already present and verified (use -Force to re-fetch)."
        return
    }

    $zip = Join-Path $work $Asset
    Write-Host "  GET $Url"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing -UserAgent "NDM-fetch-aria2"
    if ((Get-Item -LiteralPath $zip).Length -ne $ZipSize) { throw "Size mismatch for $Asset" }
    Assert-Hash $zip $ZipSha $Asset
    $got = Expand-Aria2 $zip $work
    foreach ($k in $FileSha.Keys) {
        if (-not $got.ContainsKey($k)) { throw "$k not found in $Asset" }
        Assert-Hash $got[$k] $FileSha[$k] $k
    }
    foreach ($k in $FileSha.Keys) {
        $dest = Join-Path $OutDir $OutName[$k]
        Copy-Item -LiteralPath $got[$k] -Destination $dest -Force
        Write-Host "  -> $dest"
    }
    Write-Host "aria2 $Tag ready (all SHA-256 pins verified)."
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
