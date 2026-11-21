# Remapping test (controls chat): bindings changed by press-to-bind and directly, their effect in the game, the
# file they are kept in and a restart - in background test games, scripted pad / keys only.
#   .\controls_bind_test.ps1 [-Name ctlb]
# Bindings of this test live in runs\<name>_controls.toml (XMD_CONTROLS_FILE), never in a player's file.
param([string]$Name = "ctlb")
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port 'runs'
$log = Join-Path $runs "$Name.log"
$file = Join-Path $runs "${Name}_controls.toml"
$env:XMD_CONTROLS_FILE = $file
Remove-Item $file -ErrorAction SilentlyContinue
function In([string]$l) { Add-Content (Join-Path $runs 'input.txt') $l -Encoding ascii }
function Dbg([string]$l) { Add-Content (Join-Path $runs 'debug.txt') $l -Encoding ascii }
function Last([string]$pattern) { (Select-String -Path $log -Pattern $pattern | Select-Object -Last 1).Line }
function Check([string]$what, [bool]$ok, [string]$detail) { "{0}  {1}: {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $detail }
function State { Dbg 'controls state'; Start-Sleep -Milliseconds 1300; $l = Last 'controls: capture state'; if ($l -match 'state (\d) text "(.*)"') { return @([int]$Matches[1], $Matches[2]) }; return @(-1, '') }
function Row([int]$i) {   # -> @(key text, pad text)
    Dbg 'controls list'; Start-Sleep -Milliseconds 1500
    $l = (Select-String -Path $log -Pattern ("controls: +$i .*key: ") | Select-Object -Last 1).Line
    if ($l -match 'key: (.*?)\s+pad: (.*)$') { return @($Matches[1].Trim(), $Matches[2].Trim()) }; return @('?', '?')
}
function Height {
    Dbg 'where'; In "stick L 0 1 50"; Start-Sleep -Milliseconds 2200
    $l = Last 'debug command: where'
    if ($l -match 'where: \S+ \S+ (\S+)') { return [double]$Matches[1] }; return 0
}
function Jumps([string]$inputLine) {   # does this input lift the player?
    $h0 = Height; In $inputLine; Start-Sleep -Milliseconds 350; $h1 = Height; Start-Sleep 2
    return @(($h1 - $h0 -gt 40), ("height {0:N0} -> {1:N0}" -f $h0, $h1))
}
function Start-Level {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'route_run.ps1') -Name $Name -Route route_level.txt -Wait 20 -Lines 1 -Keep -Native | Out-Null
    $t0 = Get-Date
    while (-not (Select-String -Path $log -Pattern 'flash call: ShowHudComplete' -Quiet)) {
        if (((Get-Date) - $t0).TotalSeconds -gt 600 -or -not (Get-Process xmd -ErrorAction SilentlyContinue)) { "FAIL  boot: no HUD"; exit 1 }
        Start-Sleep 5
    }
    In "clear`nwait 6000`npress B 200`nwait 1500`npress BACK 600`nwait 2500`npress BACK 600`nwait 2000"
    Start-Sleep 16
}

Start-Level
$r = Row 6; Check 'defaults' ($r[0] -eq 'Space' -and $r[1] -eq 'A') "Jump = $($r[0]) / $($r[1])"

# Press-to-bind, keyboard: Light Attack (4) <- K
Dbg 'controls bind key 4'; Start-Sleep 1; In 'key K 150'; Start-Sleep 1; $s = State; $r = Row 4
Check 'press-to-bind a key' ($s[0] -eq 2 -and $s[1] -eq 'K' -and $r[0] -eq 'K') "state $($s[0]) '$($s[1])', Light Attack = $($r[0])"
# A key another action has: Heavy Attack (5) <- K  => the two swap
Dbg 'controls bind key 5'; Start-Sleep 1; In 'key K 150'; Start-Sleep 1; $s = State; $a = Row 4; $b = Row 5
Check 'a taken key swaps' ($b[0] -eq 'K' -and $a[0] -eq 'Right Mouse') "Heavy Attack = $($b[0]), Light Attack = $($a[0])"
# Escape cancels and does not reach the game (no pause menu)
$n = (Select-String -Path $log -Pattern 'flash call: PopulatePauseMenu').Count
Dbg 'controls bind key 4'; Start-Sleep 1; In 'key Escape 150'; Start-Sleep 2; $s = State; $a = Row 4
Check 'Escape cancels, unseen by the game' ($s[0] -eq 3 -and $a[0] -eq 'Right Mouse' -and (Select-String -Path $log -Pattern 'flash call: PopulatePauseMenu').Count -eq $n) "state $($s[0]), Light Attack still $($a[0])"
# A port key cannot be bound: F5 is passed over, then J binds
Dbg 'controls bind key 4'; Start-Sleep 1; In 'key F5 150'; Start-Sleep 1; $s1 = State; In 'key J 150'; Start-Sleep 1; $s = State
Check 'F keys are passed over' ($s1[0] -eq 1 -and $s[0] -eq 2 -and $s[1] -eq 'J') "after F5: state $($s1[0]); after J: state $($s[0]) '$($s[1])'"
# Nothing pressed: ends by itself
Dbg 'controls bind key 4'; Start-Sleep 7; $s = State
Check 'times out after 5 s' ($s[0] -eq 4) "state $($s[0])"

# The effect in the game: Jump (6) on G; Space no longer jumps... (Enter, the helper, still does)
Dbg 'controls set key 6 G'; Start-Sleep 1
$j = Jumps 'key G 150'; Check 'the new key acts (G jumps)' $j[0] $j[1]
$j = Jumps 'key Space 150'; Check 'the old key no longer acts (Space)' (-not $j[0]) $j[1]
$j = Jumps 'key Return 150'; Check 'the helper key still acts (Enter)' $j[0] $j[1]

# Pad. Press-to-bind: Jump (6) <- Y (the pad's Heavy Attack gets A)
Dbg 'controls bind pad 6'; Start-Sleep 1; In 'press Y 150'; Start-Sleep 1; $s = State; $a = Row 6; $b = Row 5
Check 'press-to-bind a pad button, swap' ($s[0] -eq 2 -and $a[1] -eq 'Y' -and $b[1] -eq 'A') "state $($s[0]) '$($s[1])', Jump = $($a[1]), Heavy Attack = $($b[1])"
$j = Jumps 'press Y 150'; Check 'the pad remap acts (Y jumps)' $j[0] $j[1]
$j = Jumps 'press A 150'; Check 'the old pad button no longer jumps (A)' (-not $j[0]) $j[1]
# A trigger as a button: Jump <- RT
Dbg 'controls bind pad 6'; Start-Sleep 1; In 'trigger R 255 200'; Start-Sleep 1; $a = Row 6; $b = Row 10
Check 'a trigger can be bound' ($a[1] -eq 'RT' -and $b[1] -eq 'Y') "Jump = $($a[1]), PowerShift = $($b[1])"
$j = Jumps 'trigger R 255 200'; Check 'the trigger acts (RT jumps)' $j[0] $j[1]
# The left stick's rows cannot be bound on a pad
Dbg 'controls bind pad 0'; Start-Sleep 1; Check 'Move rows are fixed on the pad' ((Last 'controls: bind pad') -match 'cannot be bound') 'refused'

# Kept for the next start.
Check 'the file is written' ((Test-Path $file) -and (Select-String -Path $file -Pattern 'key.jump = "G"' -Quiet) -and (Select-String -Path $file -Pattern 'pad.jump = "rt"' -Quiet)) $file
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
# (the restarted game has Jump on RT and A on Heavy Attack: the pad route into the level would not work - the
# bindings are read back at the title, and tried in the level only after the reset)
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_test.ps1') -Seconds 25 -Shots 1 -Name $Name -Pad -Keep -Extra '--native_renderer=main' | Out-Null
$a = Row 6; $b = Row 4
Check 'bindings kept over a restart' ($a[0] -eq 'G' -and $a[1] -eq 'RT' -and $b[0] -eq 'J') "Jump = $($a[0]) / $($a[1]), Light Attack = $($b[0])"
# Reset.
Dbg 'controls reset key'; Dbg 'controls reset pad'; Start-Sleep 1; $a = Row 6; $b = Row 4
Check 'reset to defaults' ($a[0] -eq 'Space' -and $a[1] -eq 'A' -and $b[0] -eq 'Left Mouse') "Jump = $($a[0]) / $($a[1]), Light Attack = $($b[0])"
Check 'the file holds the defaults again' ((Select-String -Path $file -Pattern 'key.jump = "Space"' -Quiet) -and (Select-String -Path $file -Pattern 'pad.jump = "a"' -Quiet)) $file
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null
$env:XMD_CONTROLS_FILE = $null
