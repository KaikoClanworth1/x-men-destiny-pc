# Run a command hidden, below normal priority, pinned to CPUs 8-15 (the user
# games on CPUs 0-7), and wait for it. Child processes inherit both.
#   lowrun.ps1 -Exe cmake -ArgList '--build','out\build\Release' -Log runs\build.txt [-Dir <cwd>]
param(
    [Parameter(Mandatory)] [string]$Exe,
    [string[]]$ArgList = @(),
    [string]$Log = "",
    [string]$Dir = (Get-Location).Path,
    [int]$Affinity = 0
)
$ErrorActionPreference = "Stop"
$env:PATH = "C:\Program Files\LLVM\bin;$(Join-Path $PSScriptRoot '..\..\recomp\bin');$env:PATH"
$quoted = $ArgList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
$sp = @{ FilePath = $Exe; WorkingDirectory = $Dir; WindowStyle = 'Hidden'; PassThru = $true }
if ($quoted) { $sp.ArgumentList = $quoted }
if ($Log) {
    $sp.RedirectStandardOutput = $Log
    $sp.RedirectStandardError = "$Log.err"
}
$p = Start-Process @sp
$null = $p.Handle   # keeps ExitCode readable after the wait
# Below normal priority always (builds and tools must not make the desktop lag); all CPUs unless -Affinity or
# XMD_CPU_MASK says otherwise.
if (-not $Affinity -and $env:XMD_CPU_MASK) { $Affinity = [Convert]::ToInt32($env:XMD_CPU_MASK, 16) }
try {
    $p.PriorityClass = 'BelowNormal'
    if ($Affinity) { $p.ProcessorAffinity = [IntPtr]$Affinity }
} catch { }
$p.WaitForExit()
"exit code $($p.ExitCode)"
exit $p.ExitCode

