# A screenshot taken by the native renderer itself (its finished frame read back from the GPU), for when a
# picture of the window (shot.ps1) comes back black - the window is parked off every screen, and Windows stops
# composing it at times (a full-screen game on the main screen). Needs the game started by route_run.ps1 -Native
# (XMD_TEST_SHOT_REQUEST).   .\gshot.ps1 -Name fight   -> runs\fight.png
param([Parameter(Mandatory)] [string]$Name, [switch]$Full)   # -Full: not shrunk to 1280 wide
Add-Type -AssemblyName System.Drawing
$port = Split-Path $PSScriptRoot -Parent
$ppm = Join-Path $port "runs\$Name.ppm"; $png = Join-Path $port "runs\$Name.png"
Remove-Item $ppm -ErrorAction SilentlyContinue
[IO.File]::WriteAllText((Join-Path $port 'runs\shot_request.txt'), $ppm)
for ($i = 0; $i -lt 50 -and -not (Test-Path $ppm); $i++) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 400
if (-not (Test-Path $ppm)) { "no picture came (is the game running with XMD_TEST_SHOT_REQUEST?)"; exit 1 }
# (the game may still be writing it - a 2560 x 1440 picture is 11 MB: read once it opens and has stopped growing;
# reading it locked left $b empty and the header loop below never ended)
$b = $null; $last = -1
for ($i = 0; $i -lt 100 -and -not $b; $i++) {
    $len = (Get-Item $ppm).Length
    if ($len -gt 20 -and $len -eq $last) { try { $b = [IO.File]::ReadAllBytes($ppm) } catch { $b = $null } }
    $last = $len
    if (-not $b) { Start-Sleep -Milliseconds 200 }
}
if (-not $b -or $b.Length -lt 20) { "the picture could not be read: $ppm"; exit 1 }
# header: "P6\n<w> <h>\n255\n"
$pos = 0; $tok = @()
while ($tok.Count -lt 4) { $s = $pos; while ($b[$pos] -gt 32) { $pos++ }; $tok += [Text.Encoding]::ASCII.GetString($b, $s, $pos - $s); $pos++ }
$w = [int]$tok[1]; $h = [int]$tok[2]
$bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$d = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), 'WriteOnly', $bmp.PixelFormat)
$row = New-Object byte[] ($w * 3)
for ($y = 0; $y -lt $h; $y++) {
    [Array]::Copy($b, $pos + $y * $w * 3, $row, 0, $w * 3)
    for ($x = 0; $x -lt $w * 3; $x += 3) { $t = $row[$x]; $row[$x] = $row[$x + 2]; $row[$x + 2] = $t }   # RGB -> BGR
    [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]::Add($d.Scan0, $y * $d.Stride), $w * 3)
}
$bmp.UnlockBits($d)
$scale = if ($Full) { 1.0 } else { [math]::Min(1.0, 1280.0 / $w) }
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($h * $scale))
$small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose(); $bmp.Dispose(); Remove-Item $ppm
$png
