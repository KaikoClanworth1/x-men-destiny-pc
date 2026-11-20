# Screenshot the running test game's window (started by run_test.ps1 -Keep) without touching focus.
#   .\shot.ps1 -Name menu   -> runs\menu.png
param([Parameter(Mandatory)] [string]$Name)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class XS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][XS]::SetProcessDPIAware()
$out = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$id = [int](Get-Content (Join-Path $out "test_game.pid"))
$p = Get-Process -Id $id -ErrorAction SilentlyContinue
if (-not $p -or $p.ProcessName -ne "xmd") { "test game not running"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object XS+RECT
[void][XS]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $gfx.GetHdc()
[void][XS]::PrintWindow($h, $dc, 2)
$gfx.ReleaseHdc($dc)
$png = Join-Path $out "$Name.png"
$scale = [math]::Min(1.0, 1280.0 / $w)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($hh * $scale))
$small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose(); $gfx.Dispose(); $bmp.Dispose()
$png
