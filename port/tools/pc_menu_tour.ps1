# Background test of the port's own menu entries (src\pc_menu.cpp): boots to the main menu on the native
# renderer, then plays the steps given, taking the renderer's own screenshots (gshot.ps1) on the way.
# -Scale <n>: the renderer's largest render scale (1: the tests' usual 720p; 8: as a played session).
#   .\pc_menu_tour.ps1 [-Name pcm] [-Steps "A:4:diff", "B:4", "DOWN:1", "A:5:options"] [-Keep] [-KeepConfig] [-Extra '--flag=x']
# A step is <buttons>:<seconds to wait after it>[:<screenshot name>] ; buttons "-" presses nothing.
# Screenshots land in runs\<Name>_<screenshot name>.png. XMD_MENU_TRACE=1 is set: the log (runs\<Name>.log)
# holds every call between the game and its Flash menus ("menu trace:" lines).
param([string]$Name = "pcm", [string[]]$Steps = @(), [switch]$Keep, [string[]]$Extra = @(), [int]$BootTimeout = 420,
      [switch]$KeepConfig, [int]$Scale = 1)
$port = Split-Path $PSScriptRoot -Parent
$Steps = @($Steps | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$env:XMD_MENU_TRACE = '1'
# The settings file of this test only (the Graphics menu writes to it; the shared runs\test_config.toml stays as
# it is). A fresh copy of the shared one each run; -KeepConfig: the one the last run left (settings kept over a
# restart).
$env:XMD_TEST_CONFIG = Join-Path $port 'runs\pcm_config.toml'
if (-not $KeepConfig -or -not (Test-Path $env:XMD_TEST_CONFIG)) { Copy-Item (Join-Path $port 'runs\test_config.toml') $env:XMD_TEST_CONFIG -Force }
$a = @{ Name = $Name; Route = 'route_title.txt'; Wait = 20; Lines = 1; Keep = $true; Native = $true; Scale = $Scale }
if ($Extra) { $a.Extra = $Extra }
& (Join-Path $PSScriptRoot 'route_run.ps1') @a | Out-Null
$log = Join-Path $port "runs\$Name.log"
$t0 = Get-Date
$up = $false
while (((Get-Date) - $t0).TotalSeconds -lt $BootTimeout) {
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game ended during boot"; Get-Content $log -Tail 20; exit 1 }
    if (Select-String -Path $log -Pattern 'flash call: MainMenuIntroActivated' -Quiet) { $up = $true; break }
    Start-Sleep 3
}
if (-not $up) { "no main menu after $BootTimeout s"; exit 1 }
# (route_title.txt goes on: wait 5 s, A = into the menu, which first shows the save notice)
Start-Sleep 14
"main menu after $([int]((Get-Date) - $t0).TotalSeconds) s"
# A screenshot by the renderer itself (as gshot.ps1 asks for one: the path in runs\shot_request.txt), made a PNG
# 1280 wide by Python - a second instead of gshot's twenty.
function Shot([string]$n) {
    $ppm = Join-Path $port "runs\${Name}_$n.ppm"; $png = Join-Path $port "runs\${Name}_$n.png"
    Remove-Item $ppm -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $port 'runs\shot_request.txt'), $ppm)
    $last = -1
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 150
        if (-not (Test-Path $ppm)) { continue }
        $len = (Get-Item $ppm).Length
        if ($len -gt 20 -and $len -eq $last) { break }
        $last = $len
    }
    if (-not (Test-Path $ppm)) { "no picture for $n"; return }
    & python -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]); im.thumbnail((1280, 1280)); im.save(sys.argv[2])" $ppm $png
    Remove-Item $ppm -ErrorAction SilentlyContinue
    $png
}
foreach ($s in $Steps) {
    $f = $s -split ':'
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game is gone before step $s"; break }
    if ($f[0] -ne '-') { Add-Content (Join-Path $port 'runs\input.txt') "press $($f[0]) 200" -Encoding ascii }
    Start-Sleep -Milliseconds ([int]([double]$f[1] * 1000))
    if ($f.Count -gt 2 -and $f[2]) { Shot $f[2] }
}
if (Get-Process xmd -ErrorAction SilentlyContinue) {
    if (-not $Keep) { & (Join-Path $PSScriptRoot 'stop_test.ps1') | Select-Object -Last 1 }
} else { "the game has ended" }
