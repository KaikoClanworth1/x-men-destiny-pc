# How many pixels of the main menu's scene no colour draw reaches (a depth pass's triangle its colour pass lost:
# the black specks on faces), for each depth tolerance in -Slack: the scene is cleared to magenta
# (XMD_NATIVE_DEBUG_CLEAR) and tools\burst.py counts what is left of it over -Frames frames.
#   .\holes_ab.ps1 -Slack 16,128,512 [-Frames 60]
param([double[]]$Slack = @(16), [int]$Frames = 60, [string]$Size = '2432x1368')
$port = Split-Path $PSScriptRoot -Parent
foreach ($s in $Slack) {
    & (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
    $env:XMD_TEST_OUTPUT_SIZE = $Size; $env:XMD_NATIVE_DEBUG_CLEAR = '1'; $env:XMD_NATIVE_DEPTH_SLACK = "$s"
    & (Join-Path $PSScriptRoot 'route_run.ps1') -Name menu -Route route_boot.txt -Wait 70 -Keep -Native -Scale 2 -Lines 1 | Out-Null
    Add-Content (Join-Path $port 'runs\input.txt') "press A 200`nwait 1500" -Encoding ascii
    Start-Sleep 5
    $counts = & python (Join-Path $PSScriptRoot 'burst.py') $Frames "holes$s" 1000 | Select-String 'magenta' |
        ForEach-Object { [int](($_.Line -split 'magenta ')[1] -replace '\s.*', '') }
    $with = @($counts | Where-Object { $_ -gt 20 }).Count
    "slack {0}: {1} frames, {2} with holes, {3} pixels in all, worst {4}" -f $s, $counts.Count, $with, ($counts | Measure-Object -Sum).Sum, ($counts | Measure-Object -Maximum).Maximum
}
& (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
$env:XMD_NATIVE_DEBUG_CLEAR = $null; $env:XMD_NATIVE_DEPTH_SLACK = $null
