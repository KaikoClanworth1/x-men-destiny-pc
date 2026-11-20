# Back to the spot of an F4 capture, in a test game: the capture's saved games (state\save) put in the test
# profile, the main menu's Continue, then the player moved to the capture's position (resume.txt) and the world
# frozen (time almost stopped, frames still drawn: debug line "unfreeze" / F5 lets it go on). Leaves the game
# running (tools\stop_test.ps1 ends it). Mission state is the game's last save (its checkpoint), not the exact
# moment: see src\state_capture.h. -NoPause: not frozen.
#   .\resume.ps1 -Capture <capture folder> [-Name resume] [-Emulated] [-NoPause] [-Timeout 600]
param([Parameter(Mandatory = $true)][string]$Capture, [string]$Name = "resume", [switch]$Emulated, [switch]$NoPause,
      [int]$Timeout = 600, [string]$Api = "d3d12", [string[]]$ExtraFlags = @())
$port = Split-Path $PSScriptRoot -Parent
$save = Join-Path $Capture 'state\save'
if (-not (Test-Path $save)) { "no saves in $save (a capture from before 2026-11-19?)"; return }
$position = (Get-Content (Join-Path $Capture 'resume.txt') -ErrorAction SilentlyContinue |
    Where-Object { $_ -match '^position ' } | Select-Object -First 1) -replace '^position\s+', ''

& (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
$userdata = Join-Path $port 'runs\test_userdata'
Get-ChildItem $userdata -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^[0-9A-F]{16}$' } |
    Remove-Item -Recurse -Force
Copy-Item (Join-Path $save '*') $userdata -Recurse -Force
foreach ($old in [IO.Directory]::GetFiles((Join-Path $port 'runs'), "$Name*.log")) { [IO.File]::Delete($old) }

$env:XMD_TEST_KEEP_SAVES = '1'
$t0 = Get-Date
$a = @{ Name = $Name; Route = 'route_continue.txt'; Wait = 20; Lines = 1; Keep = $true; Extra = $ExtraFlags }
if (-not $Emulated) { $a.Native = $true; $a.Api = $Api }
& (Join-Path $PSScriptRoot 'route_run.ps1') @a | Out-Null
$env:XMD_TEST_KEEP_SAVES = $null
$log = Join-Path $port "runs\$Name.log"
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
    if (Select-String -Path $log -Pattern 'flash call: ShowHudComplete' -Quiet) { break }
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game ended after $([int]((Get-Date) - $t0).TotalSeconds) s"; return }
    Start-Sleep 5
}
if (-not (Select-String -Path $log -Pattern 'flash call: ShowHudComplete' -Quiet)) { "no HUD after $Timeout s"; return }
Start-Sleep 8
if ($position -and $position -ne 'none') {
    & (Join-Path $PSScriptRoot 'debug_cmd.ps1') "goto $position" | Out-Null
    Start-Sleep 3
}
# (held still with the scene on screen - the debug line "unfreeze" or F5 lets it go on)
if (-not $NoPause) { & (Join-Path $PSScriptRoot 'debug_cmd.ps1') 'freeze' | Out-Null; Start-Sleep 2 }
$moved = Select-String -Path $log -Pattern 'debug command: goto: (.*)' | Select-Object -Last 1
"in the level after $([int]((Get-Date) - $t0).TotalSeconds) s; " + $(if ($moved) { $moved.Matches[0].Groups[1].Value } else { 'not moved (no position in the capture)' })
