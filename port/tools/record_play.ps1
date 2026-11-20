# The player plays, the game writes down what they do on the controller: starts the game as a player would
# see it (on screen, with sound, normal priority; native renderer) with the recorder on. From the main menu on,
# every change on controller 1 goes to runs\recorded_pad.txt; closing the game
# ends the recording. tools\to_level.ps1 -Recorded plays it back in a background test.
#   .\record_play.ps1 [-Scale 2] [-Api d3d12] [-Resume <capture folder>]
# -Resume: the capture's saved games instead of none (the player picks Continue), and once the level's HUD is up
# the player is put at the capture's position and the world frozen (F5 lets it go on) - tools\resume.ps1's spot.
param([int]$Scale = 8, [string]$Api = "d3d12", [string]$From = "flash call: MainMenuIntroActivated",
      [string]$Resume = "", [switch]$Fresh)
$port = Split-Path $PSScriptRoot -Parent
$game = Join-Path (Split-Path $port -Parent) "Game Files"
$out = Join-Path $port "runs"
if (Get-Process xmd -ErrorAction SilentlyContinue) { "the game is already running - close it (or tools\stop_test.ps1) first"; exit 1 }
# (the tests' own saves and settings, emptied: the same New Game the test route gets)
Get-ChildItem (Join-Path $out "test_userdata") -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^[0-9A-F]{16}$' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
# (the player's own saved games live in runs\play_saves, apart from the tests that empty the profile: put back
# here, and copied out again when the game closes. -Fresh: a session without them, and they stay as they were.)
$playSaves = Join-Path $out "play_saves"
$keepSaves = -not $Resume -and -not $Fresh
if ($keepSaves -and (Test-Path $playSaves)) {
    Copy-Item (Join-Path $playSaves '*') (Join-Path $out "test_userdata") -Recurse -Force
}
$env:XMD_DEBUG_FILE = Join-Path $out "debug.txt"
Set-Content $env:XMD_DEBUG_FILE "" -NoNewline
if ($Resume) {
    Remove-Item (Join-Path $out "record.log") -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $Resume 'state\save\*') (Join-Path $out "test_userdata") -Recurse -Force
    $position = (Get-Content (Join-Path $Resume 'resume.txt') | Where-Object { $_ -match '^position -?\d' }) -replace '^position\s+', ''
    $watch = @"
`$log = '$(Join-Path $out "record.log")'
for (`$i = 0; `$i -lt 900; `$i++) {
    Start-Sleep 2
    if ((Test-Path `$log) -and (Select-String -Path `$log -Pattern 'flash call: OnSelectMenuOption' -Quiet) -and
        (Select-String -Path `$log -Pattern 'flash call: ShowHudComplete' -Quiet)) { break }
}
Start-Sleep 6
$(if ($position) { "Add-Content '$($env:XMD_DEBUG_FILE)' 'goto $position' -Encoding ascii; Start-Sleep 2" })
Add-Content '$($env:XMD_DEBUG_FILE)' 'freeze' -Encoding ascii
"@
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $watch | Out-Null
}
$env:XMD_INPUT_FILE = $null; $env:XMD_AUDIO_MUTE = $null; $env:XMD_AFFINITY = $null; $env:SVR2011_WINDOW_BEHIND = $null
$env:XMD_USER_DATA = Join-Path $out "test_userdata"
$env:XMD_CONFIG = Join-Path $out "test_config.toml"
$env:XMD_RECORD_FILE = Join-Path $out "recorded_pad.txt"
$env:XMD_RECORD_FROM = $From
$kind = @{ d3d12 = 'dxil'; vulkan = 'spirv'; d3d11 = 'dxbc' }[$Api]
$env:XMD_NATIVE_SHADERS = Join-Path $port "runs\shaders_native\$kind"
$log = Join-Path $out "record.log"
Remove-Item $log -ErrorAction SilentlyContinue
$gargs = @("--log_file=`"$log`"", "--log_level=info", "--fullscreen=false", "--native_renderer=main",
           "--gpu_backend=$Api", "--native_max_scale=$Scale")
$p = Start-Process (Join-Path $game "xmd.exe") -WorkingDirectory $game -ArgumentList $gargs -PassThru
if ($keepSaves) {
    $keep = @"
Wait-Process -Id $($p.Id) -ErrorAction SilentlyContinue
`$from = Get-ChildItem '$(Join-Path $out "test_userdata")' -Directory | Where-Object { `$_.Name -match '^[0-9A-F]{16}`$' }
if (`$from -and (`$from | Get-ChildItem -Recurse -File)) {
    Remove-Item '$playSaves' -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force '$playSaves' | Out-Null
    `$from | Copy-Item -Destination '$playSaves' -Recurse -Force
}
"@
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $keep | Out-Null
}
"started (pid $($p.Id)) - recording to $($env:XMD_RECORD_FILE) from '$From'"
