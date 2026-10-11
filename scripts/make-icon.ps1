# Draws res\qoqdir.ico: a yellow folder filling its square, like Windows' own, shaded a little lighter at the top,
# with Qoqdir's mark in the middle of its front: a white Q, a rounded square with a short tail off its lower right
# corner, over a soft shadow. Every shape is drawn here from numbers on a 256 grid, nothing is taken from another
# icon. The sizes up to 64 are drawn eight times larger and shrunk, which gives them even edges, and carry the mark
# from 24 pixels up, where it can be read. 16, 24 and 32 are stored as plain bitmaps (what the title bar and Explorer
# use most), 48, 64 and 256 PNG-compressed, which Windows reads: about 20 KB in all.
Add-Type -AssemblyName System.Drawing
function Color($html, $alpha = 255) { $c = [System.Drawing.ColorTranslator]::FromHtml($html); [System.Drawing.Color]::FromArgb($alpha, $c) }
function Shade($y, $h, $top, $bottom) {   # a brush running from $top at $y to $bottom at $y + $h
    New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, $y, 256, $h), (Color $top), (Color $bottom), ([float]90)
}
function Pen($brush, $width) { $p = New-Object System.Drawing.Pen $brush, ([float]$width); $p.LineJoin = 'Round'; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p }
function RoundRect($x, $y, $w, $h, $r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc([float]$x, [float]$y, [float](2 * $r), [float](2 * $r), 180, 90); $p.AddArc([float]($x + $w - 2 * $r), [float]$y, [float](2 * $r), [float](2 * $r), 270, 90)
    $p.AddArc([float]($x + $w - 2 * $r), [float]($y + $h - 2 * $r), [float](2 * $r), [float](2 * $r), 0, 90); $p.AddArc([float]$x, [float]($y + $h - 2 * $r), [float](2 * $r), [float](2 * $r), 90, 90)
    $p.CloseFigure()
    return $p
}
# The folder's back with its tab: a rounded box from ($x0,$top) to ($x1,$bottom) rising to $tabTop over the first
# $tabW of its width, the tab's right side sloping down over $slope.
function Back($x0, $tabTop, $top, $x1, $bottom, $tabW, $slope, $r) {
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

function Q($g, $pen, $dy = 0) {   # the mark, $dy lower than its place
    $g.TranslateTransform(0, $dy); $g.DrawPath($pen, (RoundRect 83 115 78 78 11)); $g.DrawLine($pen, 161, 193, 186, 218); $g.TranslateTransform(0, -$dy)
}
function Draw($size) {
    $ss = if ($size -le 64) { 8 } else { 1 }   # the small sizes are drawn this many times larger, then shrunk
    $big = New-Object System.Drawing.Bitmap ($size * $ss), ($size * $ss)
    $g = [System.Drawing.Graphics]::FromImage($big); $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $g.ScaleTransform($size * $ss / 256.0, $size * $ss / 256.0)   # from here on, the 256 grid
    $g.FillPath((Shade 24 212 '#F6B52E' '#E39310'), (Back 8 24 52 248 236 92 16 14))   # the back with its tab
    $g.FillPath((Shade 84 152 '#FFD75A' '#FFBE2E'), (RoundRect 8 84 240 152 14))       # the front
    $g.DrawLine((Pen (Color '#FFFFFF' 110) 5), 22, 86.5, 234, 86.5)                   # the light along its top edge
    if ($size -ge 24) {
        foreach ($s in @(@(40, 18), @(34, 28), @(28, 40))) { Q $g (Pen (Color '#7A4A00' $s[1]) $s[0]) 5 }   # the shadow: three soft layers
        Q $g (Pen (Shade 92 140 '#FFFFFF' '#F2EDE2') 26)
    }
    $g.Dispose()
    if ($ss -eq 1) { return $big }
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingMode = 'SourceCopy'
    $g.DrawImage($big, 0, 0, $size, $size); $g.Dispose()
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
