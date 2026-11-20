# Build the X-Men: Destiny port against the ReXGlue SDK source tree
# (..\recomp\rexglue-sdk) and optionally deploy to "..\Game Files".
#   .\build.ps1 [-Deploy] [-Config Release] [-Jobs 12]
# All CPUs (tools\lowrun.ps1; XMD_CPU_MASK=FF00 with -Jobs 8 keeps it off the CPUs the user games on).
param([switch]$Deploy, [string]$Config = "Release", [int]$Jobs = 12, [string]$Target = "")
$ErrorActionPreference = "Continue"

$root  = $PSScriptRoot
$low   = Join-Path $root "tools\lowrun.ps1"
$build = Join-Path $root "out\build\$Config"
$runs  = Join-Path $root "runs"
$sdk   = (Resolve-Path (Join-Path $root "..\recomp\rexglue-sdk")).Path

if (-not (Test-Path "$build\build.ninja")) {
    & $low -Exe cmake -Dir $root -Log "$runs\configure_log.txt" -ArgList @(
        "-S", $root, "-B", $build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_C_FLAGS=-march=x86-64-v2", "-DCMAKE_CXX_FLAGS=-march=x86-64-v2",
        "-DREXSDK_DIR=$sdk", "-DREXGLUE_USE_VULKAN=ON")
    if ($LASTEXITCODE) { Get-Content "$runs\configure_log.txt" -Tail 30; Get-Content "$runs\configure_log.txt.err" -Tail 30; throw "configure failed" }
}
$bargs = @("--build", $build, "-j", "$Jobs")
if ($Target) { $bargs += @("--target", $Target) }
& $low -Exe cmake -Dir $root -Log "$runs\build_log.txt" -ArgList $bargs
if ($LASTEXITCODE) { throw "build failed (see runs\build_log.txt)" }

if ($Deploy) { & (Join-Path $root "deploy.ps1") -Build $build }

