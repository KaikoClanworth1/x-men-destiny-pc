# Start a test game on the native renderer and take it into the first level
# (tools\route_level.txt), waiting until the game says its HUD is up. Leaves the
# game running (tools\stop_test.ps1 ends it) - for tools\passes.ps1, shot.ps1,
# dump triggers. Prints how long it took, or that the HUD never came.
#   .\to_level.ps1 [-Name natgame] [-Api d3d12] [-Timeout 900] [-Extra '--flag=x'] [-Emulated]
param([string]$Name = "natgame", [string]$Api = "d3d12", [int]$Timeout = 900, [string[]]$Extra = @(), [switch]$Emulated, [int]$Scale = 1, [switch]$Recorded)
$port = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
foreach ($old in [IO.Directory]::GetFiles((Join-Path $port 'runs'), "$Name*.log")) { [IO.File]::Delete($old) }
$t0 = Get-Date
$route = 'route_level.txt'
if ($Recorded) {
    $rec = Join-Path $port 'runs/recorded_pad.txt'
    if (-not (Test-Path $rec)) { "no recording: $rec (tools/record_play.ps1 makes one)"; return }
    $route = 'route_fight.txt'
    Set-Content (Join-Path $PSScriptRoot $route) ((Get-Content (Join-Path $PSScriptRoot 'route_boot.txt')) + (Get-Content $rec)) -Encoding ascii
}
$a = @{ Name = $Name; Route = $route; Wait = 20; Lines = 1; Keep = $true; Extra = $Extra }
if (-not $Emulated) { $a.Native = $true; $a.Api = $Api; $a.Scale = $Scale }
& (Join-Path $PSScriptRoot 'route_run.ps1') @a | Out-Null
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
    if (Select-String -Path (Join-Path $port "runs\$Name*.log") -Pattern 'flash call: ShowHudComplete' -Quiet) {
        if ($Recorded) { "in the level after $([int]((Get-Date) - $t0).TotalSeconds) s - the recording plays"; return }
        # (the route may still be on an earlier "pressuntil" whose line came before it started: "clear" drops
        # what is left of it, or it goes on pressing A - jumping - in the level. Then: B closes the pause menu a
        # START may have opened, BACK the tutorial box.)
        Add-Content (Join-Path $port 'runs\input.txt') "clear`nwait 6000`npress B 200`nwait 1500`npress B 200`nwait 2500`npress BACK 600`nwait 2500`npress BACK 600`nwait 2000" -Encoding ascii
        Start-Sleep 18
        "in the level after $([int]((Get-Date) - $t0).TotalSeconds) s"
        return
    }
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game ended after $([int]((Get-Date) - $t0).TotalSeconds) s"; return }
    Start-Sleep 5
}
"no HUD after $Timeout s"


