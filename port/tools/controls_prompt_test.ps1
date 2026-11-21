# Button-prompt test (controls chat): the game's prompts shown as keyboard / PlayStation / Nintendo pictures, and
# the switch while the game runs. Background test games, scripted input only; pictures: runs\<name>_*.png.
#   .\controls_prompt_test.ps1 [-Name ctlp] [-Kinds keyboard,playstation,nintendo] [-NoLive]
# For each kind (XMD_PROMPTS fixes it): boots to the main menu, opens Options (its legend shows the accept and
# back prompts), takes a picture. Then one run without a fixed kind: pad input, key input, pad input - the log
# must show the prompt textures changing each time, with a picture after each.
param([string]$Name = "ctlp", [string[]]$Kinds = @('keyboard', 'playstation', 'nintendo'), [switch]$NoLive)
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port 'runs'
$log = Join-Path $runs "$Name.log"
function In([string]$l) { Add-Content (Join-Path $runs 'input.txt') $l -Encoding ascii }
function Shot([string]$n) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gshot.ps1') -Name "${Name}_$n" | Out-Null
    "picture: runs\${Name}_$n.png"
}
function Boot {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'route_run.ps1') -Name $Name -Route route_title.txt -Wait 5 -Lines 1 -Keep -Native | Out-Null
    $t0 = Get-Date
    while (-not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MainMenuIntroActivated' -Quiet))) {
        if (((Get-Date) - $t0).TotalSeconds -gt 240 -or -not (Get-Process xmd -ErrorAction SilentlyContinue)) { "FAIL  boot: no main menu"; return $false }
        Start-Sleep 3
    }
    Start-Sleep 12
    # (Options: without saved games DOWN from New Game lands on it; its legend shows the back prompt)
    In "press DOWN 150`nwait 800`npress A 150"; Start-Sleep 5
    return $true
}
function Stop-Game { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null }

foreach ($kind in $Kinds) {
    $env:XMD_PROMPTS = $kind
    if (Boot) {
        Shot "${kind}_menu"
        "${kind}: " + ((Select-String -Path $log -Pattern "prompts: the game's picture of button").Count) + " prompt texture(s) recognized so far"
    }
    Stop-Game
}
$env:XMD_PROMPTS = $null
if (-not $NoLive) {
    if (Boot) {
        $n0 = (Select-String -Path $log -Pattern 'prompts: the pictures change').Count
        In 'stick R 20000 0 200'; Start-Sleep 3; Shot 'live_1_pad'
        In 'key V 150'; Start-Sleep 3; Shot 'live_2_keys'
        $n1 = (Select-String -Path $log -Pattern 'prompts: the pictures change').Count
        In 'stick R 20000 0 200'; Start-Sleep 3; Shot 'live_3_pad'
        $n2 = (Select-String -Path $log -Pattern 'prompts: the pictures change').Count
        "{0}  live switch pad -> keys -> pad: pictures changed {1} time(s) on the key press, {2} on the pad press after it" -f $(if ($n1 -gt $n0 -and $n2 -gt $n1) { 'PASS' } else { 'FAIL' }), ($n1 - $n0), ($n2 - $n1)
        Select-String -Path $log -Pattern 'prompts:' | Select-Object -Last 6 | ForEach-Object { $_.Line -replace '^.*\] ', '' }
    }
    Stop-Game
}
