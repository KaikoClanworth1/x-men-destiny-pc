# Packages the release: the setup (XMDSetup.exe) and the port's own prebuilt code (sdk\), so players build the game
# on their own PC from their own copy. It contains none of the game: no translated code (port\generated), no game
# data, no converted shaders, no icon made from the game.
#   powershell -NoProfile -ExecutionPolicy Bypass -File port\tools\make_release.ps1 [-Zip] [-AllowPaths]
# Output: port\installer\release\XMenDestiny-v<version>-Windows-x64\ (+ .zip), and files.txt listing every file.
#
# The port's objects and libraries are taken from the game's build folder (-Build, default port\out\build\Release)
# as they are: build the game first (build.ps1). The script itself builds only the setup, into
# port\installer\setup_build.
param(
    [string]$Build = "",      # the game's build folder
    [string]$Repo = "",       # GitHub owner/name for the launcher's update check (empty: none)
    [switch]$Zip,
    [switch]$Clean,           # a release: copy the sources to -CleanRoot and build there, so no binary names this folder
    [string]$CleanRoot = "",  # default <this drive>:\xmdrel
    [int]$Jobs = 12,
    [switch]$AllowPaths       # a test package: do not fail when binaries hold this PC's build paths
)
$ErrorActionPreference = "Stop"
$port = Split-Path $PSScriptRoot -Parent
$root = Split-Path $port -Parent
if (-not $CleanRoot) { $CleanRoot = (Split-Path $root -Qualifier) + "\xmdrel" }
$srcRoot = if ($Clean) { $CleanRoot } else { $root }   # where the game is built: this tree, or its clean copy
$sdkSrc = Join-Path $srcRoot "recomp\rexglue-sdk"
$sdkOut = Join-Path $sdkSrc "out\win-amd64"
if (-not $Build) { $Build = Join-Path $srcRoot "port\out\build\Release" }
$version = (Get-Content (Join-Path $port "VERSION.txt") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "port\VERSION.txt must hold one line like 0.1.0" }
$name = "XMenDestiny-v$version-Windows-x64"   # (system and processor in the name: launchers pick the file by them)
$rel = Join-Path $port "installer\release"
$out = Join-Path $rel $name
$ninja = Join-Path $root "recomp\bin\ninja.exe"
$env:PATH = "C:\Program Files\LLVM\bin;$(Split-Path $ninja -Parent);$env:PATH"

function Low([string]$exe, [string[]]$argList, [string]$log) {
    $quoted = $argList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle
    try { $p.PriorityClass = 'BelowNormal' } catch {}
    $p.WaitForExit()
    if ($p.ExitCode) { Get-Content $log, "$log.err" -Tail 30 | Write-Host; throw "$exe failed ($($p.ExitCode)), see $log" }
}

# ---- 0. -Clean: the game built again from a copy of the sources in a folder with a neutral name, with the build
# paths mapped away (-ffile-prefix-map), so no packaged object or DLL names this PC's project folder.
if ($Clean) {
    New-Item -ItemType Directory -Force $CleanRoot | Out-Null
    $rc = @("/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/R:1", "/W:1")
    & robocopy (Join-Path $root "recomp\rexglue-sdk") (Join-Path $CleanRoot "recomp\rexglue-sdk") @rc /XD out .git | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    & robocopy (Join-Path $root "recomp\plume") (Join-Path $CleanRoot "recomp\plume") @rc /XD .git build | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    & robocopy $port (Join-Path $CleanRoot "port") @rc /XD out runs installer screenshots setup __pycache__ | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    # the translated code's build step reads the game through the manifest's ..\Game Files: a link to the real one
    $gf = Join-Path $CleanRoot "Game Files"
    if (-not (Test-Path $gf)) { cmd /c mklink /J "$gf" "$(Join-Path $root 'Game Files')" | Out-Null }
    $map = $CleanRoot.Replace('\', '/').TrimEnd('/')
    $flags = "-march=x86-64-v2 -ffile-prefix-map=$map/="
    $cb = Join-Path $CleanRoot "port\out\build\Release"
    New-Item -ItemType Directory -Force $cb | Out-Null
    Low "cmake" @("-S", (Join-Path $CleanRoot "port"), "-B", $cb, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_C_FLAGS=$flags", "-DCMAKE_CXX_FLAGS=$flags",
        "-DREXSDK_DIR=$(Join-Path $CleanRoot 'recomp\rexglue-sdk')", "-DREXGLUE_USE_VULKAN=ON") (Join-Path $CleanRoot "configure.log")
    Low "cmake" @("--build", $cb, "-j", "$Jobs") (Join-Path $CleanRoot "build.log")
}

# ---- 1. the setup
$sb = Join-Path $port "installer\setup_build"
New-Item -ItemType Directory -Force $sb | Out-Null
Low "cmake" @("-S", (Join-Path $port "setup"), "-B", $sb, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_RC_COMPILER=llvm-rc", "-DXMD_REPO=$Repo") (Join-Path $sb "configure.log")
Low "cmake" @("--build", $sb, "-j", "8") (Join-Path $sb "build.log")
$setupExe = Join-Path $sb "XMDSetup.exe"
if (-not (Test-Path $setupExe)) { throw "XMDSetup.exe was not built" }

# ---- 2. what the game's exe is linked from, read from the game's build (so a new source can't be left out)
$nl = Get-Content (Join-Path $Build "build.ninja")
$link = $nl | Where-Object { $_ -match '^build xmd\.exe:' } | Select-Object -First 1
if (-not $link) { throw "no 'build xmd.exe' in $Build\build.ninja - build the game first" }
$inputs = ($link -replace '^build xmd\.exe: \S+ ', '' -replace ' \|.*$', '') -split ' (?=CMakeFiles/)' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$portObjs = $inputs | Where-Object { $_ -notmatch '^CMakeFiles/xmd_recomp\.dir/' -and $_ -match '\.obj$' }
if ($portObjs.Count -lt 20) { throw "only $($portObjs.Count) port objects found in the link line" }
$genObjs = $inputs | Where-Object { $_ -match '^CMakeFiles/xmd_recomp\.dir/' }
# the headers the translated code is compiled against
$deps = @{}
foreach ($t in @('CMakeFiles/xmd_recomp.dir/cmake_pch.hxx.pch') + $genObjs) {
    & $ninja -C $Build -t deps $t | Select-Object -Skip 1 | ForEach-Object { $h = $_.Trim() -replace '\\', '/'; if ($h) { $deps[$h] = 1 } }
}
if ($deps.Count -lt 200) { throw "ninja has no dependency record for the translated code (build the game first)" }
$sdkFwd = ($sdkSrc -replace '\\', '/')
$buildFwd = ((Resolve-Path $Build).Path -replace '\\', '/')
$roots = [ordered]@{   # header root -> folder under sdk\include
    "$sdkFwd/include/"                         = "rexglue"
    "$buildFwd/rexglue-sdk/include/"           = "rexglue"
    "$sdkFwd/thirdparty/simde/"                = "simde"
    "$sdkFwd/thirdparty/fmt/include/"          = "fmt"
    "$sdkFwd/thirdparty/spdlog/include/"       = "spdlog"
}
$msvc = ($deps.Keys | Where-Object { $_ -match '/VC/Tools/MSVC/14\.(\d+)\.' } | Select-Object -First 1)
if (-not $msvc) { throw "could not tell which Microsoft C++ tools the game was built with" }
$null = $msvc -match '/VC/Tools/MSVC/14\.(\d+)\.'; $msvcMinor = [int]$Matches[1]

# ---- 3. stage
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
foreach ($d in 'sdk\include', 'sdk\lib', 'sdk\obj', 'sdk\bin', 'sdk\codegen', 'licenses') { New-Item -ItemType Directory -Force (Join-Path $out $d) | Out-Null }
Copy-Item $setupExe $out
# headers: every one the translated code used, plus the whole of the SDK's own include folder
New-Item -ItemType Directory -Force (Join-Path $out "sdk\include\rexglue") | Out-Null   # (must exist: else the first folder copied becomes it)
Copy-Item (Join-Path $sdkSrc "include\*") (Join-Path $out "sdk\include\rexglue") -Recurse -Force
$unplaced = @()
foreach ($h in $deps.Keys) {
    $placed = $false
    foreach ($r in $roots.Keys) {
        if ($h.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) {
            $dst = Join-Path $out ("sdk\include\" + $roots[$r] + "\" + $h.Substring($r.Length).Replace('/', '\'))
            New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
            Copy-Item -LiteralPath $h -Destination $dst -Force
            $placed = $true; break
        }
    }
    # the compiler's, Microsoft's and Windows' own headers come from the player's tools; the translated code itself
    # and its build's stubs are made on the player's PC
    if (-not $placed -and $h -notmatch '/LLVM/|/VC/Tools/|/Windows Kits/|/port/generated/|/CMakeFiles/') { $unplaced += $h }
}
if ($unplaced) { throw "headers the translated code needs that the package has no place for:`n" + ($unplaced -join "`n") }
# the port's own objects (flat names; a clash would silently drop code)
$seen = @{}
foreach ($o in $portObjs) {
    $leaf = Split-Path $o -Leaf
    if ($seen[$leaf]) { throw "two objects are both named $leaf" }
    $seen[$leaf] = 1
    Copy-Item -LiteralPath (Join-Path $Build $o) -Destination (Join-Path $out "sdk\obj\$leaf")
}
# the port's own icon for xmd.exe (drawn by setup\tools\make_icon.ps1: nothing from the game)
$ico = Join-Path $sb "setup.ico"
Copy-Item $ico (Join-Path $sb "xmd.ico") -Force
Set-Content (Join-Path $sb "xmd_icon.rc") '1 ICON "xmd.ico"' -Encoding ascii
& llvm-rc /fo (Join-Path $out "sdk\obj\xmd_icon.res") (Join-Path $sb "xmd_icon.rc") | Out-Null
if ($LASTEXITCODE) { throw "llvm-rc failed" }
foreach ($l in "rexruntime.lib", "spdlog.lib", "fmt.lib", "SDL3-static.lib") { Copy-Item (Join-Path $sdkOut $l) (Join-Path $out "sdk\lib") }
Copy-Item (Join-Path $Build "plume\plume.lib") (Join-Path $out "sdk\lib")
foreach ($b in "rexruntime.dll", "rexgpu-xenos.dll", "rexffmpeg.dll", "rexmspack.dll", "rexglue.exe") { Copy-Item (Join-Path $sdkOut $b) (Join-Path $out "sdk\bin") }
Copy-Item (Join-Path $port "xmd_config.toml") (Join-Path $out "sdk\codegen")
# the version of Default.xex the port was made for: its SHA-1 only
$xex = Join-Path $root "Game Files\Default.xex"
$sha = (Get-FileHash $xex -Algorithm SHA1).Hash.ToLower()
@"
# Written by tools\make_release.ps1.
version=$version
min_msvc_minor=$msvcMinor
xex_sha1=$sha
"@ | Set-Content (Join-Path $out "sdk\sdk.ini") -Encoding ascii
if (Test-Path (Join-Path $root "recomplauncher.json")) {
    # prepare.version is this release's: RecompLauncher re-runs prepare (rebuilding the game) only when it changes
    $rl = Get-Content (Join-Path $root "recomplauncher.json") -Raw
    $rl = [regex]::Replace($rl, '("prepare"\s*:\s*\{)(\s*"version"\s*:\s*"[^"]*"\s*,)?', "`$1`n    `"version`": `"$version`",")
    [IO.File]::WriteAllText((Join-Path $out "recomplauncher.json"), $rl, (New-Object Text.UTF8Encoding($false)))
}
# licences of what is inside
$lic = [ordered]@{
    "ReXGlue SDK (BSD-3-Clause; derived from Xenia)" = (Join-Path $sdkSrc "LICENSE")
    "plume (MIT)"                                    = (Join-Path $root "recomp\plume\LICENSE")
    "Dear ImGui (MIT)"                               = (Join-Path $sdkSrc "thirdparty\imgui\LICENSE.txt")
    "SDL3 (zlib)"                                    = (Join-Path $sdkSrc "thirdparty\sdl3\LICENSE.txt")
    "fmt (MIT)"                                      = (Join-Path $sdkSrc "thirdparty\fmt\LICENSE")
    "spdlog (MIT)"                                   = (Join-Path $sdkSrc "thirdparty\spdlog\LICENSE")
    "SIMDe (MIT)"                                    = (Join-Path $sdkSrc "thirdparty\simde\COPYING")
    "FFmpeg (LGPL-2.1-or-later; rexffmpeg.dll)"      = (Join-Path $sdkSrc "thirdparty\FFmpeg\COPYING.LGPLv2.1")
    "libmspack (LGPL-2.1; rexmspack.dll)"            = (Join-Path $sdkSrc "thirdparty\libmspack\libmspack\COPYING.LIB")
}
$notice = @("X-Men Destiny PC port - third-party software in this package", "", "The full licence texts are in the licenses folder.", "")
foreach ($k in $lic.Keys) {
    $f = $lic[$k]
    $leaf = ($k -replace ' \(.*$', '' -replace '[^A-Za-z0-9]+', '_') + ".txt"
    if (Test-Path $f) { Copy-Item $f (Join-Path $out "licenses\$leaf"); $notice += "- ${k}: licenses\$leaf" }
    else { $notice += "- $k"; Write-Warning "licence text not found: $f" }
}
$notice += @("", "rexffmpeg.dll and rexmspack.dll are separate libraries under the LGPL: you may replace them with your own builds.")
$notice | Set-Content (Join-Path $out "THIRD-PARTY-NOTICES.txt") -Encoding utf8
@"
X-Men Destiny - PC port v$version
=================================
By KaikoClanworth

A native Windows port of X-Men: Destiny (Xbox 360), built on your PC from your own copy of the game.
This package contains no part of the game.

1. You need your own X-Men: Destiny for Xbox 360 as a disc image (.iso), or a folder it was extracted to
   (the one that holds Default.xex).
2. The game is compiled on your PC with Microsoft's free "Build Tools for Visual Studio 2026" (workload
   "Desktop development with C++", with the component "C++ Clang Compiler for Windows"). If you don't have
   them, the setup offers to install them for you.
3. Run XMDSetup.exe. It finds your game (or Browse / drop it on the window), asks where to install and shows
   the space needed. Press "Build the game": it copies the game's files, translates and compiles the game
   (about 10-20 minutes; your PC stays usable).
4. Choose a desktop shortcut and a Start-menu entry, then Play. Settings are in the game (Options > Graphics).
   Run the launcher again, or use Windows Settings > Apps, to update, repair or uninstall. Uninstalling keeps
   your saves unless you say otherwise.

Keep the sdk folder beside XMDSetup.exe until the game is built.
"@ | Set-Content (Join-Path $out "READ ME FIRST.txt") -Encoding utf8

# ---- 4. nothing of the game, nothing personal
$files = Get-ChildItem $out -Recurse -File
$bad = $files | Where-Object { $_.Name -match '\.(xex|iso|xiso|zzz|gz|bnk|xma|usm|gfx|swf|xsc|pdb|log|pak|dxil|dxbc|spv|toml\.bak)$' -or $_.Name -match '^xmd_(recomp|funcs|init|register)|^pipelines\.list$|^Default\.' -or $_.FullName -match '\\(generated|native_shaders|UserData)\\' }
if ($bad) { throw "game material in the package: $($bad.FullName -join ', ')" }
if (($files | Where-Object { $_.Name -like '*.toml' }).Count -ne 1) { throw "exactly one .toml (sdk\codegen\xmd_config.toml) is expected" }
if ($setupExe -and (Get-Item (Join-Path $out "XMDSetup.exe")).Length -gt 32MB) { throw "XMDSetup.exe is far larger than expected" }
# translated game code linked into a packaged object would show as thousands of sub_8XXXXXXX definitions
# (the user name only as part of a path: alone it matched "OrangeRed" in FFmpeg's colour table)
$needles = @($env:USERPROFILE, "Users\$env:USERNAME", $root) | Where-Object { $_ -and $_.Length -ge 4 } | ForEach-Object { $_, ($_ -replace '\\', '/') } | Select-Object -Unique
$leaks = @()
foreach ($f in $files | Where-Object { $_.Extension -in '.exe', '.dll', '.obj', '.lib', '.res' }) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    foreach ($n in $needles) {
        $wide = ($n.ToCharArray() | ForEach-Object { "$_`0" }) -join ''
        if ($text.IndexOf($n, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $text.IndexOf($wide, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $leaks += "$($f.FullName.Substring($out.Length + 1)): holds '$n'"
        }
    }
}
if ($leaks) {
    $leaks | Select-Object -First 40 | Write-Host
    if (-not $AllowPaths) { throw "$($leaks.Count) packaged binaries hold this PC's build paths (a release is built in a folder with a neutral name and -ffile-prefix-map; -AllowPaths for a test package)" }
    Write-Warning "$($leaks.Count) binaries hold build paths: a TEST package, not for publishing"
}
$list = $files | Sort-Object FullName | ForEach-Object { "{0,12}  {1}" -f $_.Length, $_.FullName.Substring($out.Length + 1) }
$size = ($files | Measure-Object Length -Sum).Sum
$head = @("$name - $($files.Count) files, $([math]::Round($size / 1MB, 1)) MB" + $(if ($leaks) { "  (TEST package: holds build paths)" }), "")
($head + $list) | Set-Content (Join-Path $rel "$name.files.txt") -Encoding utf8
"packaged $out ($([math]::Round($size / 1MB, 1)) MB, $($files.Count) files); list: $rel\$name.files.txt"
if ($Zip) {
    $zipPath = "$out.zip"
    Remove-Item $zipPath -ErrorAction SilentlyContinue
    Compress-Archive -Path $out -DestinationPath $zipPath -CompressionLevel Optimal
    "zip $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB, 1)) MB)"
}
