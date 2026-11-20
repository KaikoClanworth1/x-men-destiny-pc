# Waits until no test game and no build is running (the build folder, the deployed exe and the test-game slot
# are shared by several chats), then builds + deploys when -Build is given and reports whether that worked.
#   .\when_free.ps1 [-Build] [-Timeout 1500]
# Exit code 0: free (and built); 1: never free, or the build failed (its errors are printed).
param([switch]$Build, [int]$Timeout = 1500)
$port = Split-Path $PSScriptRoot -Parent
$t0 = Get-Date
$quiet = 0
while ($quiet -lt 3) {
    if (((Get-Date) - $t0).TotalSeconds -gt $Timeout) { "still busy after $Timeout s"; exit 1 }
    # (only this port's: other ports build with the same tools)
    $busy = Get-CimInstance Win32_Process -Filter "Name='xmd.exe' or Name='ninja.exe' or Name='cmake.exe' or Name='clang++.exe' or Name='clang.exe'" |
        Where-Object { $_.Name -eq 'xmd.exe' -or $_.CommandLine -match 'X-Men Destiny' }
    if ($busy) { $quiet = 0 } else { $quiet++ }
    Start-Sleep 2
}
"free after $([int]((Get-Date) - $t0).TotalSeconds) s"
if (-not $Build) { exit 0 }
$before = (Get-Item (Join-Path $port "..\Game Files\xmd.exe")).LastWriteTime
$o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $port 'build.ps1') -Deploy 2>&1
$after = (Get-Item (Join-Path $port "..\Game Files\xmd.exe")).LastWriteTime
if (-not ($o -match 'deployed')) {
    "BUILD FAILED"
    Get-Content (Join-Path $port 'runs\build_log.txt') | Select-String -Pattern 'error|FAILED' | Select-Object -First 25 | ForEach-Object { $_.Line }
    exit 1
}
"built and deployed: exe $before -> $after"
exit 0
