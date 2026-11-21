# Background test of a plain start: no renderer flag and no settings file (XMD_CONFIG points at a file that is
# not there), so the game's own defaults decide what draws. Boots to the main menu, takes a picture and says
# which renderer drew and whether any shader was missing.
#   .\default_start_test.ps1 [-Name plain] [-Shaders <folder> | -NoShaders] [-Wait 75]
# -Shaders: the prepared shaders (default runs\shaders_native\dxil, what the tests use; an installed copy has
#           them in <game>\native_shaders). -NoShaders: an empty folder - the fall-back to the emulated renderer.
# The usual test conditions still apply (run_test.ps1: muted, off-screen, no vsync, low priority).
param([string]$Name = "plain", [string]$Shaders = "", [switch]$NoShaders, [int]$Wait = 75)
$port = Split-Path $PSScriptRoot -Parent
if ($NoShaders) {
    $Shaders = Join-Path $port 'runs\no_shaders'
    New-Item -ItemType Directory -Force $Shaders | Out-Null
} elseif (-not $Shaders) { $Shaders = Join-Path $port 'runs\shaders_native\dxil' }
$env:XMD_NATIVE_SHADERS = $Shaders
$env:XMD_TEST_SHOT_REQUEST = Join-Path $port 'runs\shot_request.txt'
$env:XMD_TEST_CONFIG = Join-Path $port "runs\${Name}_no_config.toml"
Remove-Item $env:XMD_TEST_CONFIG -ErrorAction SilentlyContinue
$o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_test.ps1') -Seconds 5 -Shots 1 -Name $Name -Pad -Keep 2>&1
if ($LASTEXITCODE) { $o | Select-Object -Last 5; exit 1 }
Get-Content (Join-Path $PSScriptRoot 'route_title.txt') | Where-Object { $_ -notmatch '^#' } | Add-Content (Join-Path $port 'runs\input.txt') -Encoding ascii
$log = Join-Path $port "runs\$Name.log"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    if (-not (Get-Process xmd -ErrorAction SilentlyContinue)) { "the game ended during boot"; Get-Content $log -Tail 15; exit 1 }
    if (Select-String -Path $log -Pattern 'flash call: MainMenuIntroActivated' -Quiet) { break }
    Start-Sleep 3
}
Start-Sleep $Wait
$text = Get-Content $log
# (without a settings file the log has the emulated GPU's "viz query" lines too and rolls over: the start-up
# lines may be gone, the renderer's "native perf" lines every 5 s are not)
$native = [bool]($text | Select-String 'native perf: \d' | Select-Object -Last 1) -and -not (Select-String -Path (Join-Path $port "runs\${Name}_problems.log") -Pattern "native renderer: can't draw" -Quiet)
$png = Join-Path $port "runs\${Name}_menu.png"
Remove-Item $png -ErrorAction SilentlyContinue
if ($native) {   # (the renderer's own picture)
    $ppm = Join-Path $port "runs\${Name}_menu.ppm"
    [IO.File]::WriteAllText($env:XMD_TEST_SHOT_REQUEST, $ppm)
    for ($i = 0; $i -lt 100 -and -not (Test-Path $ppm); $i++) { Start-Sleep -Milliseconds 150 }
    Start-Sleep 1
    if (Test-Path $ppm) { & python -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]); im.thumbnail((1280, 1280)); im.save(sys.argv[2])" $ppm $png; Remove-Item $ppm }
} else {         # (the window's picture)
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shot.ps1') -Name "${Name}_menu" | Out-Null
}
$clean = { param($l) $l -replace '^\[\S+ (\S+)\] \[(\w+)\] \[\w+\] \[t\d+\] ', '$1 [$2] ' }
"settings file there: $(Test-Path $env:XMD_TEST_CONFIG); shaders: $Shaders"
"renderer: $(if ($native) { 'NATIVE' } else { 'EMULATED' })"
$text | Select-String "native renderer: (main renderer|D3D12 backend|Vulkan backend|ready|can't draw|its shaders|output )" | Select-Object -First 8 | ForEach-Object { & $clean $_.Line }
"shaders without a converted one: $(@($text | Select-String 'no converted shader').Count); made by the game and saved: $(@($text | Select-String 'the game made a shader').Count)"
$text | Select-String 'native draws per frame|native perf: \d| fps: ' | Select-Object -Last 2 | ForEach-Object { & $clean $_.Line }
"faults: $(@($text | Select-String 'crash:|device removed|did not finish').Count)"
"picture: $(if (Test-Path $png) { $png } else { 'none' })"
& (Join-Path $PSScriptRoot 'stop_test.ps1') | Select-Object -Last 1
