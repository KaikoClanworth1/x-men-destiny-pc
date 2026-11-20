# Copy the built program (exe + DLLs) into "..\Game Files" beside the disc files.
#   .\deploy.ps1 [-Build out\build\Release] [-Game <folder>]
param([string]$Build = (Join-Path $PSScriptRoot "out\build\Release"), [string]$Game = "")
$ErrorActionPreference = "Stop"
if (-not $Game) { $Game = Join-Path (Split-Path $PSScriptRoot -Parent) "Game Files" }
if (@(Get-CimInstance Win32_Process -Filter "Name = 'xmd.exe'" | Where-Object { $_.ExecutablePath -like "$Game*" }).Count) {
    throw "xmd.exe is running from $Game - close it first."
}
$exe = Get-ChildItem $Build -Recurse -Filter xmd.exe | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) { throw "no xmd.exe under $Build" }
Copy-Item $exe.FullName $Game -Force
# The SDK's runtime and plugin DLLs: beside the exe, or in the SDK's own output folder.
$dirs = @($exe.DirectoryName, (Join-Path $PSScriptRoot "..\recomp\rexglue-sdk\out\win-amd64")) | Where-Object { Test-Path $_ }
foreach ($d in $dirs) {
    Get-ChildItem $d -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $Game -Force }
}
"deployed $($exe.FullName) -> $Game"
Get-ChildItem $Game -Include *.exe,*.dll -File -Recurse -Depth 0 | Select-Object Name,Length,LastWriteTime | Format-Table -AutoSize
