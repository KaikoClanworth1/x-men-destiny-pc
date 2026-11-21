# One background test along a scripted route: starts the game (muted, behind
# other windows, CPUs 8-15 - see run_test.ps1), feeds it the route, waits,
# takes a screenshot, prints the frame rate lines and stops the game.
#   .\route_run.ps1 -Name base [-Route route_game.txt] [-Wait 300] [-Extra '--flag=x'] [-Keep]
# XMD_FPS_LOG=1 is set for the game (src\d3d_probe.cpp: "fps:" log lines).
param([string]$Name = "route", [string]$Route = "route_game.txt", [int]$Wait = 300, [string[]]$Extra = @(),
      [switch]$Keep, [int]$Lines = 8, [switch]$Native, [string]$Api = "d3d12", [int]$Scale = 1)
$port = Split-Path $PSScriptRoot -Parent
$env:XMD_FPS_LOG = '1'
# -Native: the native renderer (its converted shaders from runs\shaders_native, the API's kind), with the
# pass-stepping file of tools\passes.ps1.
if ($Native) {
    $kind = @{ d3d12 = 'dxil'; vulkan = 'spirv'; d3d11 = 'dxbc' }[$Api]
    $env:XMD_NATIVE_SHADERS = Join-Path $port "runs\shaders_native\$kind"
    $env:XMD_NATIVE_STOP_FILE = Join-Path $port 'runs\stop_after.txt'
    $env:XMD_NATIVE_DUMP_TRIGGER = Join-Path $port 'runs\dump_now.txt'; $env:XMD_NATIVE_DUMP_FILE = Join-Path $port 'runs\native_draws.txt'; $env:XMD_NATIVE_DUMP_COUNT = '9000'
    Set-Content $env:XMD_NATIVE_STOP_FILE '0' -Encoding ascii
    # (the renderer's own screenshot: tools\gshot.ps1)
    $env:XMD_TEST_SHOT_REQUEST = Join-Path $port 'runs\shot_request.txt'
    $Extra += '--native_renderer=main', "--gpu_backend=$Api", "--native_max_scale=$Scale"
} elseif (-not ($Extra -match 'native_renderer')) {
    # (the game's own default is the native renderer since 2026-11-21: without -Native a test asks for the emulated one)
    $Extra += '--native_renderer=off'
}
# (the emulated GPU's frame dump: create runs\gpu_dump_now.txt -> runs\gpu_draws.txt)
$env:XMD_GPU_DUMP_TRIGGER = Join-Path $port 'runs\gpu_dump_now.txt'; $env:XMD_GPU_DUMP_FILE = Join-Path $port 'runs\gpu_draws.txt'
# (the render thread's profile: create runs\profile_now.txt -> runs\profile.txt, 5 s later; src\profile_sampler.cpp)
$env:XMD_PROFILE_TRIGGER = Join-Path $port 'runs\profile_now.txt'; $env:XMD_PROFILE_FILE = Join-Path $port 'runs\profile.txt'
$a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run_test.ps1'),
       '-Seconds', '15', '-Shots', '1', '-Name', $Name, '-Pad', '-Keep')
if ($Extra) { $a += '-Extra'; $a += ($Extra -join ',') }
$o = & powershell @a 2>&1
if ($LASTEXITCODE) { $o | Select-Object -Last 5; exit 1 }
Get-Content (Join-Path $PSScriptRoot $Route) | Where-Object { $_ -notmatch '^#' } |
    Add-Content (Join-Path $port 'runs\input.txt') -Encoding ascii
Start-Sleep -Seconds $Wait
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shot.ps1') -Name "${Name}_end" | Out-Null
$log = Get-ChildItem (Join-Path $port "runs\$Name*.log") | Sort-Object LastWriteTime | ForEach-Object { Get-Content $_.FullName }
$log | Select-String ' fps: ' | Select-Object -Last $Lines | ForEach-Object { $_.Line -replace '\[info\] \[core\] \[t\d+\] ', '' }
if (-not $Keep) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stop_test.ps1') | Select-Object -Last 1 }
