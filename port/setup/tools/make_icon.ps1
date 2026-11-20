# Draws the port's own icon (a yellow "X" stroke mark on dark blue; nothing from the game) and writes it as an .ico
# with 256..16 px images. Run by port\setup\CMakeLists.txt at configure time; the icon is not kept in the repository.
#   powershell -NoProfile -ExecutionPolicy Bypass -File make_icon.ps1 -Out <file.ico>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$sizes = 256, 128, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    # rounded square, dark blue gradient
    $r = [single]($s * 0.18); $d = $r * 2; $m = [single]($s * 0.03); $w = [single]($s - 2 * $m)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($m, $m, $d, $d, 180, 90); $path.AddArc($m + $w - $d, $m, $d, $d, 270, 90)
    $path.AddArc($m + $w - $d, $m + $w - $d, $d, $d, 0, 90); $path.AddArc($m, $m + $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, [System.Drawing.Color]::FromArgb(255, 30, 44, 84), [System.Drawing.Color]::FromArgb(255, 12, 16, 34), 90)
    $g.FillPath($bg, $path)
    # two crossing strokes
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 255, 208, 40), [single]($s * 0.15))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $a = [single]($s * 0.27); $b = [single]($s * 0.73)
    $g.DrawLine($pen, $a, $a, $b, $b)
    $g.DrawLine($pen, $b, $a, $a, $b)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$f = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($f)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $bw.Write([byte]($sizes[$i] % 256)); $bw.Write([byte]($sizes[$i] % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out -Parent) | Out-Null
[IO.File]::WriteAllBytes($Out, $f.ToArray())
"wrote $Out ($($f.Length) bytes)"
