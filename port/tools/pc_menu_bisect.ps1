# Which part of the changed Options movie misbehaves: one short tour per variant (tools\pc_menu_tour.ps1), each
# opening Options from the main menu and its first sub-menu, with screenshots runs\pcm<variant>_options.png /
# _sub.png.   .\pc_menu_bisect.ps1 -Variants o,1,2,4 [-Steps ...]
#   o          the disc's movies (XMD_MENU_ORIGINAL=1)
#   <number>   runs\menu_movies\p<number>\XMDMenuOptions.gfx (tools\menu_patch_test.exe <in> <out> <parts>)
#   full       the change built into the game
param([string[]]$Variants = @('o', 'full'), [string[]]$Steps = @('DOWN:1', 'A:7:options', 'A:4:sub', 'B:3:back'))
$port = Split-Path $PSScriptRoot -Parent
$Variants = @($Variants | ForEach-Object { $_ -split "," } | Where-Object { $_ })
foreach ($v in $Variants) {
    $env:XMD_MENU_ORIGINAL = $null; $env:XMD_MENU_MOVIE_DIR = $null
    if ($v -eq 'o') { $env:XMD_MENU_ORIGINAL = '1' }
    elseif ($v -ne 'full') { $env:XMD_MENU_MOVIE_DIR = Join-Path $port "runs\menu_movies\p$v" }
    "== variant $v"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'pc_menu_tour.ps1') -Name "pcm$v" -Steps ($Steps -join ',') | Select-Object -Last 2
    $log = Join-Path $port "runs\pcm$v.log"
    Select-String -Path $log -Pattern 'menus:|flash -> game (PopulateOptionsMenu|SelectMenuOption|PCGraphics|ExitOptionsMenu|CollapseSubMenu|Populate\w+Menu)' |
        ForEach-Object { ($_.Line -replace '^\[\S+ (\S+)\] \[\w+\] \[core\] \[t\d+\] ', '$1 ') } | ForEach-Object { $_.Substring(0, [math]::Min(150, $_.Length)) }
}
$env:XMD_MENU_ORIGINAL = $null; $env:XMD_MENU_MOVIE_DIR = $null
