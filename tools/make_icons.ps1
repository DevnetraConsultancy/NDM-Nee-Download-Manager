# make_icons.ps1 -- renders the NDM logo (resources/icons/ndm.ico, the browser
# extension PNGs and the master SVG) from one vector definition.
# Invented with Care By Devnetra Consultancy
#
#   powershell -ExecutionPolicy Bypass -File tools\make_icons.ps1
#
# The geometry below is the single source of truth for the logo and is
# mirrored 1:1 by NdmTheme::LogoGeom / NdmTheme::Logo (src/ui/ndm_theme.cpp),
# which draws the in-app logo at any DPI. If you change one, change both.
#
# Crispness rules
#   * GDI+ AntiAlias + PixelOffsetMode.Half: coordinates are pixel EDGES, so
#     every horizontal / vertical edge sits exactly on the pixel grid.
#   * 16, 20 and 24 px use hand-tuned integer layouts (simplified: square
#     shaft, no rounded caps) so nothing is a half-covered blur.
#   * 28 px and up use the proportional layout, every coordinate rounded to a
#     whole pixel and the arrow centred with matching parity.

param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Round-Px([double]$v) { return [int][Math]::Floor($v + 0.5) }

# ---------------------------------------------------------------------------
# Logo geometry for a square of $s pixels (all values are pixel edges)
# ---------------------------------------------------------------------------
function Get-LogoGeom([int]$s)
{
    $g = @{}
    switch ($s)
    {
        16 { $g = @{ r=3; bw=1; sx0=6; sx1=10; sy0=3;  hy=6;  hx0=4; hx1=12; ty=10; bx0=3; bx1=13; by0=11; by1=13; cap=0 } }
        20 { $g = @{ r=4; bw=1; sx0=8; sx1=12; sy0=4;  hy=8;  hx0=5; hx1=15; ty=13; bx0=4; bx1=16; by0=14; by1=16; cap=0 } }
        24 { $g = @{ r=5; bw=1; sx0=9; sx1=15; sy0=4;  hy=11; hx0=6; hx1=18; ty=17; bx0=5; bx1=19; by0=18; by1=20; cap=0 } }
        default
        {
            $r  = Round-Px (0.22 * $s)
            $bw = [Math]::Max(1, (Round-Px ($s / 26.0)))
            $w  = Round-Px (0.19 * $s)
            if ((($s - $w) % 2) -ne 0) { $w += 1 }
            $sx0 = ($s - $w) / 2
            $hh = Round-Px (0.27 * $s)
            $T  = Round-Px (0.17 * $s)
            $hy = Round-Px (0.43 * $s)
            $ty = $hy + $hh
            $gap = [Math]::Max(1, (Round-Px (0.05 * $s)))
            $by0 = $ty + $gap
            $bh  = [Math]::Max(2, (Round-Px (0.08 * $s)))
            $bot = $by0 + $bh
            $dy  = Round-Px ((($s - $bot) - $T) / 2.0)
            $bx0 = Round-Px (0.19 * $s)
            $g = @{ r=$r; bw=$bw; sx0=$sx0; sx1=$sx0 + $w; sy0=$T + $dy; hy=$hy + $dy
                    hx0=$s / 2.0 - $hh; hx1=$s / 2.0 + $hh; ty=$ty + $dy
                    bx0=$bx0; bx1=$s - $bx0; by0=$by0 + $dy; by1=$bot + $dy
                    cap=$(if ($s -ge 32) { $bh / 2.0 } else { 0 }) }
        }
    }
    return $g
}

# Palette (identical to ndm_theme.cpp)
$C = @{
    TileTop = [System.Drawing.Color]::FromArgb(255, 46, 50, 60)
    TileBot = [System.Drawing.Color]::FromArgb(255, 17, 19, 23)
    RimTop  = [System.Drawing.Color]::FromArgb(255, 246, 214, 110)
    RimBot  = [System.Drawing.Color]::FromArgb(255, 176, 136, 34)
    ArrTop  = [System.Drawing.Color]::FromArgb(255, 255, 228, 130)
    ArrBot  = [System.Drawing.Color]::FromArgb(255, 222, 176, 52)
}

function New-RoundPath([double]$x, [double]$y, [double]$w, [double]$h, [double]$rad)
{
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2.0 * $rad
    if ($d -gt $w) { $d = $w }
    if ($d -gt $h) { $d = $h }
    if ($d -lt 1.0)
    {
        $p.AddRectangle((New-Object System.Drawing.RectangleF([single]$x, [single]$y, [single]$w, [single]$h)))
        return $p
    }
    $p.AddArc([single]$x, [single]$y, [single]$d, [single]$d, 180, 90)
    $p.AddArc([single]($x + $w - $d), [single]$y, [single]$d, [single]$d, 270, 90)
    $p.AddArc([single]($x + $w - $d), [single]($y + $h - $d), [single]$d, [single]$d, 0, 90)
    $p.AddArc([single]$x, [single]($y + $h - $d), [single]$d, [single]$d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-VBrush([double]$y0, [double]$y1, $top, $bot)
{
    # one pixel of slack each side avoids the GDI+ wrap seam at the edges
    return New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, [single]($y0 - 1))),
        (New-Object System.Drawing.PointF(0, [single]($y1 + 1))), $top, $bot)
}

function Render-Logo([int]$s)
{
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gr = [System.Drawing.Graphics]::FromImage($bmp)
    $gr.Clear([System.Drawing.Color]::Transparent)
    $gr.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $gr.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $gr.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g = Get-LogoGeom $s

    # gold rim = outer rounded square; dark tile = inner rounded square
    $outer = New-RoundPath 0 0 $s $s $g.r
    $rimB  = New-VBrush 0 $s $C.RimTop $C.RimBot
    $gr.FillPath($rimB, $outer)
    $bw = $g.bw
    $inner = New-RoundPath $bw $bw ($s - 2 * $bw) ($s - 2 * $bw) ([Math]::Max(0, $g.r - $bw))
    $tileB = New-VBrush 0 $s $C.TileTop $C.TileBot
    $gr.FillPath($tileB, $inner)

    # arrow + base bar share one vertical gold gradient
    $arrB = New-VBrush $g.sy0 $g.by1 $C.ArrTop $C.ArrBot
    $shaft = New-RoundPath $g.sx0 $g.sy0 ($g.sx1 - $g.sx0) ($g.hy + 1 - $g.sy0) 0
    $gr.FillPath($arrB, $shaft)
    $head = New-Object System.Drawing.Drawing2D.GraphicsPath
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF([single]$g.hx0, [single]$g.hy)),
        (New-Object System.Drawing.PointF([single]$g.hx1, [single]$g.hy)),
        (New-Object System.Drawing.PointF([single]($s / 2.0), [single]$g.ty)))
    $head.AddPolygon($pts)
    $gr.FillPath($arrB, $head)
    $bar = New-RoundPath $g.bx0 $g.by0 ($g.bx1 - $g.bx0) ($g.by1 - $g.by0) $g.cap
    $gr.FillPath($arrB, $bar)

    foreach ($o in @($outer, $rimB, $inner, $tileB, $arrB, $shaft, $head, $bar)) { $o.Dispose() }
    $gr.Dispose()
    return $bmp
}

function Get-PngBytes($bmp)
{
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $b = $ms.ToArray(); $ms.Dispose()
    return ,$b
}

# 32-bit DIB (XOR image + AND mask) as stored inside an .ico
function Get-DibBytes($bmp)
{
    $s = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $bwr = New-Object System.IO.BinaryWriter($ms)
    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $bwr.Write([int]40); $bwr.Write([int]$s); $bwr.Write([int]($s * 2))
    $bwr.Write([int16]1); $bwr.Write([int16]32); $bwr.Write([int]0)
    $bwr.Write([int]($s * $s * 4 + $maskStride * $s))
    $bwr.Write([int]0); $bwr.Write([int]0); $bwr.Write([int]0); $bwr.Write([int]0)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $px = New-Object byte[] ($data.Stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
    $stride = $data.Stride
    $bmp.UnlockBits($data)
    for ($y = $s - 1; $y -ge 0; $y--) { $bwr.Write($px, $y * $stride, $s * 4) }
    for ($y = $s - 1; $y -ge 0; $y--)
    {
        $row = New-Object byte[] $maskStride
        for ($x = 0; $x -lt $s; $x++)
        {
            if ($px[$y * $stride + $x * 4 + 3] -eq 0) { $row[[int][Math]::Floor($x / 8)] = $row[[int][Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) }
        }
        $bwr.Write($row)
    }
    $bwr.Flush()
    $b = $ms.ToArray(); $bwr.Dispose()
    return ,$b
}

# ---------------------------------------------------------------------------
# Render
# ---------------------------------------------------------------------------
$iconDir = Join-Path $Root 'resources\icons'
$pngDir  = Join-Path $iconDir 'png'
New-Item -ItemType Directory -Force $pngDir | Out-Null

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = @()
foreach ($s in $sizes)
{
    $bmp = Render-Logo $s
    $png = Get-PngBytes $bmp
    [System.IO.File]::WriteAllBytes((Join-Path $pngDir "ndm_$s.png"), $png)
    if ($s -ge 256) { $images += ,@{ s = $s; bytes = $png } }
    else            { $images += ,@{ s = $s; bytes = (Get-DibBytes $bmp) } }
    $bmp.Dispose()
}

# .ico container
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($im in $images)
{
    $dim = if ($im.s -ge 256) { 0 } else { $im.s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]$im.bytes.Length); $w.Write([int]$offset)
    $offset += $im.bytes.Length
}
foreach ($im in $images) { $w.Write($im.bytes) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $iconDir 'ndm.ico'), $ms.ToArray())
$w.Dispose()

# Browser extension icons: the very same renders
foreach ($b in 'chrome', 'firefox')
{
    $d = Join-Path $Root "src\browser_ext\$b\images"
    if (Test-Path $d)
    {
        foreach ($s in 16, 32, 48, 128)
        {
            Copy-Item (Join-Path $pngDir "ndm_$s.png") (Join-Path $d "ndm$s.png") -Force
        }
    }
}

# Master SVG (proportional layout on a 256 grid)
$g = Get-LogoGeom 256
$svg = @"
<?xml version="1.0" encoding="UTF-8"?>
<!-- NDM -- Nee Download Manager logo (master). Generated by tools/make_icons.ps1;
     same geometry as NdmTheme::Logo. Small sizes (16/20/24) use the hand-tuned
     pixel layouts in the script. Invented with Care By Devnetra Consultancy -->
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#F6D66E"/><stop offset="1" stop-color="#B08822"/>
    </linearGradient>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#2E323C"/><stop offset="1" stop-color="#111317"/>
    </linearGradient>
    <linearGradient id="gold" gradientUnits="userSpaceOnUse" x1="0" y1="$($g.sy0)" x2="0" y2="$($g.by1)">
      <stop offset="0" stop-color="#FFE482"/><stop offset="1" stop-color="#DEB034"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="256" height="256" rx="$($g.r)" fill="url(#rim)"/>
  <rect x="$($g.bw)" y="$($g.bw)" width="$(256 - 2 * $g.bw)" height="$(256 - 2 * $g.bw)" rx="$($g.r - $g.bw)" fill="url(#tile)"/>
  <g fill="url(#gold)">
    <rect x="$($g.sx0)" y="$($g.sy0)" width="$($g.sx1 - $g.sx0)" height="$($g.hy + 1 - $g.sy0)"/>
    <polygon points="$($g.hx0),$($g.hy) $($g.hx1),$($g.hy) 128,$($g.ty)"/>
    <rect x="$($g.bx0)" y="$($g.by0)" width="$($g.bx1 - $g.bx0)" height="$($g.by1 - $g.by0)" rx="$($g.cap)"/>
  </g>
</svg>
"@
[System.IO.File]::WriteAllText((Join-Path $iconDir 'ndm_logo.svg'), $svg, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "NDM icons written:" (Join-Path $iconDir 'ndm.ico') "($($sizes -join ', ') px)"
