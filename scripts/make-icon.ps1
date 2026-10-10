# Draws res\qoqdir.ico: a cartoon folder, flat yellow with a dark outline, a small face on its front from 32 pixels
# up, and Qoqdir's mark beside it: a pink ring with a leaf lying on its lower right. Every shape is drawn here from
# numbers on a 256 grid, nothing is taken from another icon. Each size is drawn afresh; at 64 and below the folder is
# snapped to whole pixels so its edges stay sharp, and the mark is drawn from 24 pixels up, a little larger at 24 and
# 32 where it has to be read. 16, 24 and 32 are stored as plain bitmaps (what the title bar and Explorer use most),
# 48, 64 and 256 PNG-compressed, which Windows reads: about 20 KB in all.
Add-Type -AssemblyName System.Drawing
function Color($html) { [System.Drawing.ColorTranslator]::FromHtml($html) }
function Brush($html) { New-Object System.Drawing.SolidBrush (Color $html) }
function ABrush($alpha, $html) { $c = Color $html; New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int]$alpha, $c.R, $c.G, $c.B)) }
function Pen($html, $width) { New-Object System.Drawing.Pen (Color $html), ([float]$width) }
function RoundRect($x, $y, $w, $h, $r) {
    if ($r -lt 1) { $r = 1 }   # an arc of size 0 is refused
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc([float]$x, [float]$y, [float](2 * $r), [float](2 * $r), 180, 90); $p.AddArc([float]($x + $w - 2 * $r), [float]$y, [float](2 * $r), [float](2 * $r), 270, 90)
    $p.AddArc([float]($x + $w - 2 * $r), [float]($y + $h - 2 * $r), [float](2 * $r), [float](2 * $r), 0, 90); $p.AddArc([float]$x, [float]($y + $h - 2 * $r), [float](2 * $r), [float](2 * $r), 90, 90)
    $p.CloseFigure()
    return $p
}
# The folder's back with its tab: a rounded box from ($x0,$top) to ($x1,$bottom) rising to $tabTop over the first
# $tabW of its width, the tab's right side sloping down over $slope.
function Back($x0, $tabTop, $top, $x1, $bottom, $tabW, $slope, $r) {
    if ($r -lt 1) { $r = 1 }
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc([float]$x0, [float]$tabTop, [float](2 * $r), [float](2 * $r), 180, 90)
    $p.AddLine([float]($x0 + $r), [float]$tabTop, [float]($x0 + $tabW), [float]$tabTop)
    $p.AddLine([float]($x0 + $tabW), [float]$tabTop, [float]($x0 + $tabW + $slope), [float]$top)
    $p.AddLine([float]($x0 + $tabW + $slope), [float]$top, [float]($x1 - $r), [float]$top)
    $p.AddArc([float]($x1 - 2 * $r), [float]$top, [float](2 * $r), [float](2 * $r), 270, 90)
    $p.AddArc([float]($x1 - 2 * $r), [float]($bottom - 2 * $r), [float](2 * $r), [float](2 * $r), 0, 90)
    $p.AddArc([float]$x0, [float]($bottom - 2 * $r), [float](2 * $r), [float](2 * $r), 90, 90)
    $p.CloseFigure()
    return $p
}
# A teardrop leaf, round at ($bx,$by), narrowing to its stem at ($tx,$ty), in two greens split along its spine when $two.
function Leaf($g, $bx, $by, $tx, $ty, $bulge, $two) {
    $dx = [double]$tx - $bx; $dy = [double]$ty - $by; $len = [Math]::Sqrt($dx * $dx + $dy * $dy)
    $nx = -$dy / $len * $bulge; $ny = $dx / $len * $bulge
    function Side($sign) {   # one edge of the leaf, from the round end to the stem, closed along the spine
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath
        $p.AddBezier([float]$bx, [float]$by, [float]($bx + $dx * 0.02 + $nx * $sign), [float]($by + $dy * 0.02 + $ny * $sign), [float]($bx + $dx * 0.6 + $nx * 0.9 * $sign), [float]($by + $dy * 0.6 + $ny * 0.9 * $sign), [float]$tx, [float]$ty)
        $p.AddLine([float]$tx, [float]$ty, [float]$bx, [float]$by)
        $p.CloseFigure()
        return $p
    }
    $g.FillPath((Brush '#4CAF50'), (Side 1)); $g.FillPath((Brush $(if ($two) { '#7CCB80' } else { '#4CAF50' })), (Side -1))
}

function Draw($size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $k = $size / 256.0
    function S($v) { if ($size -le 64) { [Math]::Round($v * $k) } else { $v * $k } }   # a length on the 256 grid, at this size
    $back = Back (S 20) (S 40) (S 66) (S 236) (S 214) (S 86) (S 14) (S 14)
    $front = RoundRect (S 20) (S 88) (S 216) (S 126) (S 14)
    $g.FillPath((Brush '#F5A623'), $back); $g.FillPath((Brush '#FFD93D'), $front)
    $ink = Pen '#2B2B2B' ([Math]::Max(1.5, 6 * $k))
    $g.DrawPath($ink, $back); $g.DrawPath($ink, $front)
    $face = $size -ge 32
    $g.ScaleTransform($k, $k)   # from here on, the 256 grid
    if ($face) {   # on the left of the front: blushing cheeks, two dot eyes, a small smile
        $g.FillEllipse((ABrush 120 '#FF7A9A'), 38, 142, 18, 9); $g.FillEllipse((ABrush 120 '#FF7A9A'), 92, 142, 18, 9)
        $g.FillEllipse((Brush '#2B2B2B'), 54, 126, 9, 9); $g.FillEllipse((Brush '#2B2B2B'), 84, 126, 9, 9)
        $g.DrawArc((Pen '#2B2B2B' 3.5), 60, 132, 28, 18, 20, 140)
    }
    if ($size -ge 24) {   # the mark, to the right of the face when there is one
        $dx = if ($face) { 50 } else { 0 }
        if ($size -le 32) { $g.TranslateTransform(132 + $dx, 150); $g.ScaleTransform(1.12, 1.12); $g.TranslateTransform(-132, -150) } else { $g.TranslateTransform($dx, 0) }
        $g.DrawEllipse((Pen '#EF4F9E' ([Math]::Max(13, 2.2 / $k))), 79, 110, 66, 66)   # the ring: 13 wide, never under 2.2 px
        Leaf $g 172 198 134 165 16 ($size -ge 48)
    }
    $g.Dispose()
    return $bmp
}
function Dib($bmp) {   # a 32-bit bitmap as an icon file holds it: header, rows bottom-up, then the 1-bit mask
    $w = $bmp.Width; $h = $bmp.Height; $maskRow = [int]([Math]::Ceiling($w / 32.0) * 4)
    $ms = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter $ms
    foreach ($v in @([uint32]40, [int32]$w, [int32]($h * 2), [uint16]1, [uint16]32, [uint32]0, [uint32]($w * $h * 4 + $maskRow * $h), [int32]0, [int32]0, [uint32]0, [uint32]0)) { $bw.Write($v) }
    for ($y = $h - 1; $y -ge 0; $y--) { for ($x = 0; $x -lt $w; $x++) { $c = $bmp.GetPixel($x, $y); $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A) } }
    for ($y = $h - 1; $y -ge 0; $y--) {
        $row = New-Object byte[] $maskRow
        for ($x = 0; $x -lt $w; $x++) { if ($bmp.GetPixel($x, $y).A -eq 0) { $i = [int][Math]::Floor($x / 8); $row[$i] = $row[$i] -bor (0x80 -shr ($x % 8)) } }
        $bw.Write($row)
    }
    $bw.Flush()
    return ,$ms.ToArray()
}
function Png($bmp) { $ms = New-Object IO.MemoryStream; $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); return ,$ms.ToArray() }

$images = New-Object System.Collections.ArrayList
foreach ($size in 16, 24, 32) { [void]$images.Add(@{ size = $size; bytes = [byte[]](Dib (Draw $size)) }) }
foreach ($size in 48, 64, 256) { [void]$images.Add(@{ size = $size; bytes = [byte[]](Png (Draw $size)) }) }
$ms = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)   # an icon file with this many pictures
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = [byte]$(if ($img.size -eq 256) { 0 } else { $img.size })   # 0 means 256
    $bw.Write($dim); $bw.Write($dim); $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$img.bytes.Length); $bw.Write([uint32]$offset)
    $offset += $img.bytes.Length
}
foreach ($img in $images) { $bw.Write([byte[]]$img.bytes) }
$bw.Flush()
$path = Join-Path (Split-Path $PSScriptRoot) 'res\qoqdir.ico'
[IO.File]::WriteAllBytes($path, $ms.ToArray())
"wrote $path, $((Get-Item $path).Length) bytes"
