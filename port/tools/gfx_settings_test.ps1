# Background test of live graphics-setting changes (the Graphics menu's rows): boots to the main menu on the
# native renderer as a played session (largest render scale 8, 1280x720 window), then sets the settings given
# through the debug line "set <setting> <value>" (what the menu's rows do), several rounds, and reports whether
# the GPU hung, the device was lost or the game ended, with the renderer's own lines about each change.
#   .\gfx_settings_test.ps1 [-Name gfxset] [-Steps "native_scene_height=720","native_scene_height=1440"]
#       [-Rounds 3] [-StepMs 450] [-Hold 8] [-Start "native_aa=2"] [-Shot] [-Keep]
# -Start: settings written into the test's own settings file before the start (runs\<Name>_config.toml, a copy
#         of runs\test_config.toml - the shared file is not touched).
# -Steps default: the session of 2026-11-20 19:21 (720 -> 900 -> 1080 -> 1440 with anti-aliasing 2x).
# Test switches are taken from the environment as set by the caller, e.g.
#   $env:XMD_TEST_STALE_RESOLVED_VIEWS = '1'   the renderer keeps freed resolve textures' views (the old fault)
#   $env:XMD_TEST_NO_EFFECT_CAP = '1'          shadow buffers scaled without limit (as before)
# Exit code 0: all steps applied and the game still draws; 1: a hang, a lost device or the game ended.
param([string]$Name = "gfxset",
      [string[]]$Steps = @("native_scene_height=720", "native_scene_height=900", "native_scene_height=1080", "native_scene_height=1440"),
      [int]$Rounds = 3, [int]$StepMs = 450, [int]$Hold = 8, [string[]]$Start = @("native_aa=2"), [switch]$Shot,
      [switch]$Keep, [int]$BootTimeout = 420)
$port = Split-Path $PSScriptRoot -Parent
$Steps = @($Steps | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Start = @($Start | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$config = Join-Path $port "runs\${Name}_config.toml"
$lines = @(Get-Content (Join-Path $port 'runs\test_config.toml'))
foreach ($s in $Start) {
    $n, $v = $s -split '=', 2
    if ($v -notmatch '^(-?\d+(\.\d+)?|true|false)$') { $v = '"' + $v + '"' }
    $hit = $false
    $lines = @($lines | ForEach-Object { if ($_ -match "^$n\s*=") { $hit = $true; "$n = $v" } else { $_ } })
    if (-not $hit) { $lines = @("$n = $v") + $lines }
}
[IO.File]::WriteAllLines($config, $lines)
$env:XMD_TEST_CONFIG = $config
& (Join-Path $PSScriptRoot 'route_run.ps1') -Name $Name -Route 'route_title.txt' -Wait 20 -Lines 1 -Keep -Native -Scale 8 | Out-Null
$log = Join-Path $port "runs\$Name.log"
$t0 = Get-Date
$up = $false
while (((Get-Date) - $t0).TotalSeconds -lt $BootTimeout) {
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game ended during boot"; Get-Content $log -Tail 20; exit 1 }
    if (Select-String -Path $log -Pattern 'flash call: MainMenuIntroActivated' -Quiet) { $up = $true; break }
    Start-Sleep 3
}
if (-not $up) { "no main menu after $BootTimeout s"; exit 1 }
Start-Sleep 14
"main menu after $([int]((Get-Date) - $t0).TotalSeconds) s"
$bad = 'did not finish|device removed|DEVICE_HUNG|stopped answering|can.t draw the game|crash:'
$sent = 0
:rounds for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($s in $Steps) {
        if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game is gone before '$s' (round $r)"; break rounds }
        if (Select-String -Path $log -Pattern $bad -Quiet) { "a GPU fault before '$s' (round $r)"; break rounds }
        $n, $v = $s -split '=', 2
        Add-Content (Join-Path $port 'runs\debug.txt') "set $n $v" -Encoding ascii
        $sent++
        Start-Sleep -Milliseconds $StepMs
    }
}
Start-Sleep $Hold
$alive = [bool](Get-Process xmd -ErrorAction SilentlyContinue)
if ($Shot -and $alive) {
    $ppm = Join-Path $port "runs\${Name}_end.ppm"; $png = Join-Path $port "runs\${Name}_end.png"
    Remove-Item $ppm, $png -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $port 'runs\shot_request.txt'), $ppm)
    $last = -1
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 150
        if (-not (Test-Path $ppm)) { continue }
        $len = (Get-Item $ppm).Length
        if ($len -gt 20 -and $len -eq $last) { break }
        $last = $len
    }
    if (Test-Path $ppm) {
        & python -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]); im.thumbnail((1280, 1280)); im.save(sys.argv[2])" $ppm $png
        Remove-Item $ppm -ErrorAction SilentlyContinue
        "picture: $png"
    } else { "no picture" }
}
$text = Get-Content $log
$clean = { param($l) $l -replace '^\[\S+ (\S+)\] \[(\w+)\] \[\w+\] \[t\d+\] ', '$1 [$2] ' }
"steps sent: $sent of $($Steps.Count * $Rounds); debug lines applied: $(@($text | Select-String 'debug command: set ').Count); scale changes: $(@($text | Select-String 'native renderer: output ').Count)"
"-- the renderer about the changes (last 12) --"
$text | Select-String 'native renderer: (output |render scale |.*views of resolve|texture quality|.*lowered|the GPU has not)' | Select-Object -Last 12 | ForEach-Object { & $clean $_.Line }
"-- test notes (first 6) --"
$text | Select-String 'TEST - ' | Select-Object -First 6 | ForEach-Object { & $clean $_.Line }
"stale views used: $(@($text | Select-String 'TEST - a freed').Count) logged"
"-- faults --"
$faults = @($text | Select-String $bad)
$faults | Select-Object -First 8 | ForEach-Object { & $clean $_.Line }
"-- last frame rate line --"
$text | Select-String 'native perf: .* fps' | Select-Object -Last 1 | ForEach-Object { & $clean $_.Line }
if ($alive -and -not $Keep) { & (Join-Path $PSScriptRoot 'stop_test.ps1') | Select-Object -Last 1 }
if (-not $alive) { "RESULT: the game ended"; exit 1 }
if ($faults.Count) { "RESULT: GPU fault"; exit 1 }
"RESULT: ok - $sent changes, no fault"
exit 0
