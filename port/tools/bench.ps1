# The same frame over and over for a speed comparison: a capture's spot (tools\resume.ps1), the world frozen
# (time almost stopped: every frame draws the same scene), then the renderer's "native perf" lines averaged over
# -Seconds. Each -Extra entry is one run (a set of game flags, ';' between flags; '' = as is).
#   .\bench.ps1 -Capture runs\cap_095611 -Extra '', '--native_record_thread=true' [-Seconds 30]
param([Parameter(Mandatory = $true)][string]$Capture, [string[]]$Extra = @(''), [int]$Seconds = 30)
$port = Split-Path $PSScriptRoot -Parent
$log = Join-Path $port 'runs\bench.log'
foreach ($e in $Extra) {
    $flags = @($e -split ';' | Where-Object { $_ })
    & (Join-Path $PSScriptRoot 'resume.ps1') -Capture $Capture -Name bench -ExtraFlags $flags | Out-Null
    Start-Sleep 12
    $from = (Get-Content $log).Count
    Start-Sleep $Seconds
    $lines = Get-Content $log | Select-Object -Skip $from | Where-Object { $_ -match 'native perf: ([0-9.]+) fps, per frame: draw ([0-9.]+) ms.*thread cpu ([0-9.]+) ms' }
    $fps = @(); $draw = @(); $cpu = @()
    foreach ($l in $lines) {
        if ($l -match 'native perf: ([0-9.]+) fps, per frame: draw ([0-9.]+) ms.*thread cpu ([0-9.]+) ms') {
            $fps += [double]$Matches[1]; $draw += [double]$Matches[2]; $cpu += [double]$Matches[3]
        }
    }
    $avg = { param($a) if ($a.Count) { ($a | Measure-Object -Average).Average } else { 0 } }
    "{0,-45} {1,6:N1} fps  render thread {2,5:N2} ms  renderer's draws {3,5:N2} ms  ({4} samples)" -f $(if ($e) { $e } else { '(as is)' }), (& $avg $fps), (& $avg $cpu), (& $avg $draw), $fps.Count
    & (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
}
