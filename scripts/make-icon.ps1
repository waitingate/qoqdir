# Draws src\qoqdir.ico: a folder in the flat style of Windows' own icons, so it sits in with the system's, with a Q
# on its front the way the special folders carry their mark: a pink ring with a leaf for its tail, which still shows
# on the taskbar. Each size is drawn afresh from the same shapes, laid out on a 256 grid; at 48 and below the folder
# is snapped to whole pixels so its edges stay sharp, and the Q is drawn from 24 pixels up, where it can be read.
# 16, 24 and 32 are stored as plain bitmaps (what the title bar and Explorer use most), 48 and 256 PNG-compressed,
# which Windows reads: about 14 KB in all.
Add-Type -AssemblyName System.Drawing
function RoundRect($x, $y, $w, $h, $r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90); $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90); $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    return $p
}
function Draw($size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.SmoothingMode = 'AntiAlias'
    $k = $size / 256.0
    function S($v) { if ($size -le 48) { [Math]::Round($v * $k) } else { $v * $k } }   # a length on the 256 grid, at this size
    $back = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#D9A031'))
    $g.FillPath($back, (RoundRect (S 20) (S 36) (S 96) (S 60) (S 12)))      # the tab
    $g.FillPath($back, (RoundRect (S 20) (S 56) (S 216) (S 164) (S 14)))    # the back
    $top = New-Object System.Drawing.PointF 0, (S 90); $bottom = New-Object System.Drawing.PointF 0, (S 220)
    $front = New-Object System.Drawing.Drawing2D.LinearGradientBrush ($top, $bottom, [System.Drawing.ColorTranslator]::FromHtml('#FFD85E'), [System.Drawing.ColorTranslator]::FromHtml('#F5BC3A'))
    $g.FillPath($front, (RoundRect (S 20) (S 90) (S 216) (S 130) (S 14)))   # the front
    if ($size -ge 24) {   # the Q, on the 256 grid
        $g.ScaleTransform($k, $k)
        $ring = New-Object System.Drawing.Drawing2D.GraphicsPath
        $ring.AddEllipse(84, 110, 76, 80); $ring.AddEllipse(101, 127, 42, 46)   # the inner one cuts the hole
        $leaf = New-Object System.Drawing.Drawing2D.GraphicsPath
        $leaf.AddBezier(126, 166, 127, 192, 154, 213, 180, 208); $leaf.AddBezier(180, 208, 172, 190, 145, 169, 126, 166)
        $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#E3008C'))), $ring)
        $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#2F8F3A'))), $leaf)
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
foreach ($size in 48, 256) { [void]$images.Add(@{ size = $size; bytes = [byte[]](Png (Draw $size)) }) }
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
$path = Join-Path (Split-Path $PSScriptRoot) 'src\qoqdir.ico'
[IO.File]::WriteAllBytes($path, $ms.ToArray())
"wrote $path, $((Get-Item $path).Length) bytes"
