# Launch the game from "Game Files", screenshot its window every few seconds,
# then stop it and print the log's errors/tail.
# Runs in the background: audio muted, window never activated, opened on the
# second screen behind every window there, CPUs 8-15 only. One test game at a
# time; never touches a game the player started; no input is ever sent.
#   .\run_test.ps1 [-Seconds 60] [-Shots 4] [-Name run] [-Extra "--flag=x"] [-Game <folder>] [-Keep]
param([int]$Seconds = 60, [int]$Shots = 4, [string]$Name = "run", [string[]]$Extra = @(), [string]$Game = "",
      [switch]$Keep, [string]$LogLevel = "info", [switch]$Pad)
$ErrorActionPreference = "Stop"
# (several flags through "powershell -File" arrive as one "a,b" string)
$Extra = @($Extra | ForEach-Object { $_ -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class XW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint m);
}
"@
[void][XW]::SetProcessDPIAware()
# No Windows error boxes from a game that fails to load or crashes (children inherit this).
[void][XW]::SetErrorMode(0x8007)

$port = Split-Path $PSScriptRoot -Parent
$game = if ($Game) { $Game } else { Join-Path (Split-Path $port -Parent) "Game Files" }
$out  = (New-Item -ItemType Directory -Force (Join-Path $port "runs")).FullName
$log  = Join-Path $out "$Name.log"
$pidf = Join-Path $out "test_game.pid"

# One test game at a time, and never while the player's own game runs.
$running = @(Get-CimInstance Win32_Process -Filter "Name = 'xmd.exe'" -ErrorAction SilentlyContinue)
foreach ($r in $running) {
    if ([string]$r.CommandLine -match [regex]::Escape("\port\runs\")) {
        throw "A test game is already running (pid $($r.ProcessId)) - one test at a time."
    }
    throw "The game is running (pid $($r.ProcessId), not started by a test) - not starting a test while someone is playing."
}
Remove-Item $log -ErrorAction SilentlyContinue
# A test starts without saved games: New Game fills a slot every run, and with all six full the game refuses
# to start another ("no free save slots"). XMD_TEST_KEEP_SAVES=1 keeps them (tests of Continue / Load Game).
if (-not $env:XMD_TEST_KEEP_SAVES) {
    Get-ChildItem (Join-Path $out "test_userdata") -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^[0-9A-F]{16}$' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

# -Pad: the game's only controller is the scripted one, fed by runs\input.txt
# (append lines to it: "press START", "stick L 0 32000 1000" ... see src\script_input.h).
if ($Pad) {
    $env:XMD_INPUT_FILE = Join-Path $out "input.txt"
    Set-Content $env:XMD_INPUT_FILE "" -NoNewline
} else { $env:XMD_INPUT_FILE = $null }
$env:XMD_AUDIO_MUTE = "1"
# Debug commands (src\debug_commands.cpp): lines appended to runs\debug.txt run as Lua in the game - "teleport
# <actor id>", "spawn <node id>", "event <name>", "lua <code>". The session's problems: runs\<name>_problems.log.
$env:XMD_DEBUG_FILE = Join-Path $out "debug.txt"
Set-Content $env:XMD_DEBUG_FILE "" -NoNewline
$env:XMD_PROBLEM_FILE = Join-Path $out "${Name}_problems.log"
# The test game must not be felt on the desktop (the user, 2026-11-16: tests made Chrome lag and flicker):
# Below-normal priority on all CPUs (the user is not gaming on CPUs 0-7 for now; other ports' builds often fill the PC, and on eight CPUs the test crawled)
# - was: CPUs 8-15 only at below-normal priority (idle priority starved it to under 1 fps whenever another build ran), no variable
# refresh rate or tearing (a windowed game on a G-Sync screen drags the whole screen's refresh rate to its own
# frame rate - other windows flicker), and its window off every screen (below). No vsync either: presenting
# an off-screen window in step with the display stalled for seconds at a time (tests crawled at 2-10 fps);
# the game paces itself at 30 fps anyway.
# XMD_CPU_MASK=<hex> picks other CPUs.
$mask = if ($env:XMD_CPU_MASK) { $env:XMD_CPU_MASK } else { "FFFF" }
$env:XMD_AFFINITY = $mask
$env:XMD_USER_DATA = Join-Path $out "test_userdata"
# (XMD_TEST_CONFIG=<file>: a test that changes settings in the game - the Graphics menu - keeps its own file)
$env:XMD_CONFIG = if ($env:XMD_TEST_CONFIG) { $env:XMD_TEST_CONFIG } else { Join-Path $out "test_config.toml" }
$env:SVR2011_WINDOW_BEHIND = "1"           # (SDK switch: window opens behind every other window)
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"

# The second screen (any screen that is not the primary one).
$second = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1

$gargs = @("--log_file=`"$log`"", "--log_level=$LogLevel", "--audio_mute=true", "--fullscreen=false", "--mnk_mode=false",
           "--vsync=false", "--d3d12_allow_variable_refresh_rate_and_tearing=false",
           "--vulkan_allow_present_mode_immediate=false", "--vulkan_allow_present_mode_mailbox=false") + $Extra
# (SDL lists the primary screen first, so with two screens the other one is 2)
if ($second) { $gargs += "--monitor=2" }
$p = Start-Process (Join-Path $game "xmd.exe") -WorkingDirectory $game -ArgumentList $gargs -PassThru -WindowStyle Minimized
$null = $p.Handle
Set-Content $pidf $p.Id
try { $p.PriorityClass = if ($env:XMD_TEST_PRIORITY) { $env:XMD_TEST_PRIORITY } else { 'BelowNormal' }; $p.ProcessorAffinity = [IntPtr][Convert]::ToInt32($mask, 16) } catch { }

# Park the window off every screen (below the lowest one), behind everything, without activating it: nothing of
# it is composited onto a screen, and screenshots still work (PrintWindow asks the window for its picture).
for ($t = 0; $t -lt 150; $t++) {
    $p.Refresh()
    if ($p.HasExited -or $p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 100
}
if (-not $p.HasExited -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    $bottom = ([System.Windows.Forms.Screen]::AllScreens | ForEach-Object { $_.Bounds.Bottom } | Measure-Object -Maximum).Maximum
    $left = ([System.Windows.Forms.Screen]::AllScreens | ForEach-Object { $_.Bounds.Left } | Measure-Object -Minimum).Minimum
    [void][XW]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $left + 40, $bottom + 200, 0, 0, 0x11)  # NOSIZE|NOACTIVATE, HWND_BOTTOM
}

function Save-Shot([string]$png) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    $r = New-Object XW+RECT
    [void][XW]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $gfx.GetHdc()
    [void][XW]::PrintWindow($h, $dc, 2)   # PW_RENDERFULLCONTENT
    $gfx.ReleaseHdc($dc)
    $scale = [math]::Min(1.0, 1280.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $scale)), ([int]($hh * $scale))
    $small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $gfx.Dispose(); $bmp.Dispose()
    return $true
}

$interval = [math]::Max(1, [int]($Seconds / [math]::Max(1, $Shots)))
for ($i = 1; $i -le $Shots; $i++) {
    for ($s = 0; $s -lt $interval -and -not $p.HasExited; $s++) { Start-Sleep 1 }
    if ($p.HasExited) { break }
    $png = Join-Path $out ("{0}_{1}.png" -f $Name, $i)
    if (Save-Shot $png) { "shot $i : $png" } else { "shot $i : no window yet" }
}

if ($p.HasExited) { "EXITED early, code 0x{0:X8}" -f $p.ExitCode }
elseif ($Keep) { "left running, pid $($p.Id)" }
else { Stop-Process -Id $p.Id -Force; "stopped after ${Seconds}s" }
if (-not $Keep -or $p.HasExited) { Remove-Item $pidf -ErrorAction SilentlyContinue }
Start-Sleep 1
if (Test-Path $log) {
    $lines = Get-Content $log
    "log: $log ($($lines.Count) lines)"
    "--- errors/warnings (first 40) ---"
    $lines | Select-String -Pattern "\[(error|critical|warning|warn)\]|FATAL" | Select-Object -First 40 | ForEach-Object { $_.Line }
    "--- tail ---"
    $lines | Select-Object -Last 25
} else { "no log written" }


