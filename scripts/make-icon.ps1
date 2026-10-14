# Draws res\qoqdir.ico: a yellow folder filling its square, like Windows' own, shaded a little lighter at the top,
# with a red bookmark ribbon hanging over its front from above the top edge, and Qoqdir's mark on the ribbon: a white
# Q, a rounded square whose tail starts inside it. Every shape is drawn here from numbers on a 256 grid, nothing is
# taken from another icon. There is a picture for every size Windows asks for at 100 to 200% scaling. The sizes up to
# 64 are drawn eight times larger and shrunk, each straight edge first moved onto a whole pixel so that it comes out
# solid; the Q is a letter from 40 up, is set pixel by pixel at 32 and 24, and is left out at 20 and 16, where the
# ribbon alone marks the folder. 16 to 32 are stored as plain bitmaps (the title bar and Explorer's lists use them
# most), 40 to 256 PNG-compressed, which Windows reads: about 23 KB in all.
Add-Type -AssemblyName System.Drawing
function Color($html, $alpha = 255) { $c = [System.Drawing.ColorTranslator]::FromHtml($html); [System.Drawing.Color]::FromArgb($alpha, $c) }
function Shade($y, $h, $top, $bottom) {   # a brush running from $top at $y to $bottom at $y + $h
    New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, $y, 256, $h), (Color $top), (Color $bottom), ([float]90)
}
function Pen($brush, $width) { $p = New-Object System.Drawing.Pen $brush, ([float]$width); $p.LineJoin = 'Round'; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p }
# One pixel of the size being drawn, on the 256 grid (0 above 64); S moves a coordinate onto a whole pixel.
$script:unit = 0
function S($c) { if ($script:unit) { [Math]::Floor($c / $script:unit + 0.5) * $script:unit } else { $c } }
function RoundRect($x, $y, $w, $h, $r) {
    $x0 = S $x; $y0 = S $y; $x1 = S ($x + $w); $y1 = S ($y + $h); $r = [Math]::Max((S $r), 1)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc([float]$x0, [float]$y0, [float](2 * $r), [float](2 * $r), 180, 90); $p.AddArc([float]($x1 - 2 * $r), [float]$y0, [float](2 * $r), [float](2 * $r), 270, 90)
    $p.AddArc([float]($x1 - 2 * $r), [float]($y1 - 2 * $r), [float](2 * $r), [float](2 * $r), 0, 90); $p.AddArc([float]$x0, [float]($y1 - 2 * $r), [float](2 * $r), [float](2 * $r), 90, 90)
    $p.CloseFigure()
    return $p
}
# The folder's back with its tab: a rounded box from ($x0,$top) to ($x1,$bottom) rising to $tabTop over the first
# $tabW of its width, the tab's right side sloping down over $slope.
function Back($x0, $tabTop, $top, $x1, $bottom, $tabW, $slope, $r) {
    $x0 = S $x0; $tabTop = S $tabTop; $top = S $top; $x1 = S $x1; $bottom = S $bottom; $tabW = S $tabW; $slope = S $slope; $r = [Math]::Max((S $r), 1)
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
function Ribbon($g, $x, $y, $w, $h) {   # hangs from ($x,$y), its end cut in a V, over a soft shadow; returns its sides
    $x0 = S $x; $y0 = S $y; $x1 = S ($x + $w); $y1 = S ($y + $h); $v = S (($x1 - $x0) * 0.45)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddLines([System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $x0, $y0), (New-Object System.Drawing.PointF $x1, $y0), (New-Object System.Drawing.PointF $x1, $y1), (New-Object System.Drawing.PointF (($x0 + $x1) / 2), ($y1 - $v)), (New-Object System.Drawing.PointF $x0, $y1)))
    $p.CloseFigure()
    $d = S 4
    $g.TranslateTransform(0, $d); $g.FillPath((New-Object System.Drawing.SolidBrush (Color '#7A4A00' 50)), $p); $g.TranslateTransform(0, -$d)
    $g.FillPath((Shade $y0 ($y1 - $y0) '#E0526E' '#C8375A'), $p)
    return $x0, $x1
}
function Draw($size) {
    $ss = if ($size -le 64) { 8 } else { 1 }   # the small sizes are drawn this many times larger, then shrunk
    $script:unit = if ($size -le 64) { 256 / $size } else { 0 }
    $big = New-Object System.Drawing.Bitmap ($size * $ss), ($size * $ss)
    $g = [System.Drawing.Graphics]::FromImage($big); $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $g.ScaleTransform($size * $ss / 256.0, $size * $ss / 256.0)   # from here on, the 256 grid
    $g.FillPath((Shade 24 212 '#F6B52E' '#E39310'), (Back 8 24 52 248 236 92 16 14))   # the back with its tab
    $g.FillPath((Shade 84 152 '#FFD75A' '#FFBE2E'), (RoundRect 8 84 240 152 14))       # the front
    if ($size -ge 48) { $g.DrawLine((Pen (Color '#FFFFFF' 110) 5), 22, (S 84) + 2.5, 234, (S 84) + 2.5) }   # the light along its top edge
    $x0, $x1 = Ribbon $g 150 36 $(if ($size -eq 16) { 64 } else { 70 }) 166   # four pixels wide at 16: five would be a third of the folder
    if ($size -gt 32) { $pen = Pen (Color '#FFFFFF') 13; $g.DrawPath($pen, (RoundRect 166 90 38 38 8)); $g.DrawLine($pen, 190, 114, 214, 138) }   # the Q
    $g.Dispose()
    if ($ss -eq 1) { return $big }
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingMode = 'SourceCopy'
    $g.DrawImage($big, 0, 0, $size, $size); $g.Dispose()
    if ($size -eq 32 -or $size -eq 24) {   # the Q pixel by pixel: a square ring of five in the middle of the ribbon, its tail from inside
        $x = [Math]::Floor(($x0 + $x1) / $script:unit / 2 - 2); $y = [Math]::Floor(92 / $script:unit + 0.5); $w = [System.Drawing.Color]::White
        for ($i = 0; $i -lt 5; $i++) { $bmp.SetPixel($x + $i, $y, $w); $bmp.SetPixel($x + $i, $y + 4, $w); $bmp.SetPixel($x, $y + $i, $w); $bmp.SetPixel($x + 4, $y + $i, $w) }
        $bmp.SetPixel($x + 3, $y + 3, $w); $bmp.SetPixel($x + 5, $y + 5, $w)
    }
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
foreach ($size in 16, 20, 24, 32) { [void]$images.Add(@{ size = $size; bytes = [byte[]](Dib (Draw $size)) }) }
foreach ($size in 40, 48, 64, 256) { [void]$images.Add(@{ size = $size; bytes = [byte[]](Png (Draw $size)) }) }
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
