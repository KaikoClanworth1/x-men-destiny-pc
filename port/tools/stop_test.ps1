# Stop the test game run_test.ps1 -Keep left running (by its recorded pid only),
# and wait until it is really gone: at idle priority a killed game takes a few
# seconds to leave, and the next test refuses to start beside it.
$out = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$pidf = Join-Path $out "test_game.pid"
if (-not (Test-Path $pidf)) { "no test game recorded"; exit 0 }
$id = [int](Get-Content $pidf)
$p = Get-Process -Id $id -ErrorAction SilentlyContinue
if ($p -and $p.ProcessName -eq "xmd") {
    try { $p.PriorityClass = 'Normal' } catch { }   # (so that it can go quickly)
    Stop-Process -Id $id -Force
    [void]$p.WaitForExit(30000)
    "stopped test game $id"
} else { "test game $id already gone" }
Remove-Item $pidf -ErrorAction SilentlyContinue
