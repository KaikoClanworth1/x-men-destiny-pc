# Controls test (controls chat): keyboard and mouse play checked in a background test game - scripted keys and
# mouse movement only (the game's keyboard-and-mouse driver in its test mode; no real device, no desktop input).
#   .\controls_test.ps1 [-Name ctl] [-NoStart]      (-NoStart: a test game in the level is already running)
# 1. boots into level 1 with KEYS ONLY (tools\route_level_keys.txt)   2. mouse look: yaw / pitch per count, a
# fast flick, a slow drag, pitch limits   3. nothing turns while paused (Escape opens / closes the pause menu)
# 4. W walks, Space jumps   5. the camera keeps the mouse's direction while walking.
param([string]$Name = "ctl", [switch]$NoStart)
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port 'runs'
$log = Join-Path $runs "$Name.log"
function In([string]$l) { Add-Content (Join-Path $runs 'input.txt') $l -Encoding ascii }
function Dbg([string]$l) { Add-Content (Join-Path $runs 'debug.txt') $l -Encoding ascii }
function Last([string]$pattern) { (Select-String -Path $log -Pattern $pattern | Select-Object -Last 1).Line }
function Angles {
    Dbg 'cam peek 828 3'; Start-Sleep -Milliseconds 1500
    $l = Last 'cam: peek'
    if ($l -match '\+832: (\S+) .*\+836: (\S+) ') {
        $p = [double]$Matches[1]; if ($p -gt 3.1416) { $p -= 6.2832 }
        return @{ pitch = $p; yaw = [double]$Matches[2] }
    }
    return @{ pitch = 0; yaw = 0 }
}
function Turn([double]$a, [double]$b) { $d = $b - $a; while ($d -gt 3.1416) { $d -= 6.2832 }; while ($d -lt -3.1416) { $d += 6.2832 }; $d }
function Where-Player {
    Dbg 'where'; In "stick L 0 1 50"; Start-Sleep -Milliseconds 2500
    $l = Last 'debug command: where'
    if ($l -match 'where: (.*)$') { return $Matches[1] } else { return '?' }
}
function Check([string]$what, [bool]$ok, [string]$detail) { "{0}  {1}: {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $detail }

if (-not $NoStart) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'route_run.ps1') -Name $Name -Route route_level_keys.txt -Wait 20 -Lines 1 -Keep -Native | Out-Null
    $t0 = Get-Date
    while (-not (Select-String -Path $log -Pattern 'flash call: ShowHudComplete' -Quiet)) {
        if (((Get-Date) - $t0).TotalSeconds -gt 600 -or -not (Get-Process xmd -ErrorAction SilentlyContinue)) { "FAIL  keys-only boot: no HUD ($([int]((Get-Date) - $t0).TotalSeconds) s)"; exit 1 }
        Start-Sleep 5
    }
    Check 'keys-only boot into level 1' $true "HUD after $([int]((Get-Date) - $t0).TotalSeconds) s"
    Start-Sleep 14
}

$a = Angles; In 'mouse 1000 0 500'; Start-Sleep -Milliseconds 1500; $b = Angles
$d = Turn $a.yaw $b.yaw; Check 'mouse yaw, 1000 counts in 0.5 s' ([math]::Abs($d - 1.2) -lt 0.06) ("turned {0:N3} rad (1.200 wanted)" -f $d)
$a = $b; In 'mouse 3000 0 60'; Start-Sleep -Milliseconds 1200; $b = Angles
$d = Turn $a.yaw $b.yaw; $w = Turn 0 3.6; Check 'fast flick, 3000 counts in 0.06 s' ([math]::Abs($d - $w) -lt 0.1) ("turned {0:N3} rad ({1:N3} wanted: nothing lost)" -f $d, $w)
$a = $b; In 'mouse 100 0 2500'; Start-Sleep -Milliseconds 3500; $b = Angles
$d = Turn $a.yaw $b.yaw; Check 'slow drag, 100 counts in 2.5 s' ([math]::Abs($d - 0.12) -lt 0.02) ("turned {0:N3} rad (0.120 wanted: no dead zone)" -f $d)
$a = $b; In 'mouse -500 0 300'; Start-Sleep -Milliseconds 1300; $b = Angles
$d = Turn $a.yaw $b.yaw; Check 'mouse left' ([math]::Abs($d + 0.6) -lt 0.05) ("turned {0:N3} rad (-0.600 wanted)" -f $d)
$a = $b; In 'mouse 0 100 300'; Start-Sleep -Milliseconds 1300; $b = Angles
Check 'mouse down = look down' ([math]::Abs(($b.pitch - $a.pitch) - 0.12) -lt 0.03) ("pitch {0:N3} -> {1:N3} (+0.120 wanted)" -f $a.pitch, $b.pitch)
In 'mouse 0 -3000 400'; Start-Sleep -Milliseconds 1400; $b = Angles
Check 'pitch limit up' ([math]::Abs($b.pitch + 0.485) -lt 0.03) ("pitch {0:N3} (-0.485 = the stick's limit)" -f $b.pitch)
In 'mouse 0 5000 400'; Start-Sleep -Milliseconds 1400; $b = Angles
Check 'pitch limit down' ([math]::Abs($b.pitch - 0.955) -lt 0.03) ("pitch {0:N3} (0.955 = the stick's limit)" -f $b.pitch)
In 'mouse 0 -700 300'; Start-Sleep -Milliseconds 1300

# Pause: Escape opens the pause menu, the mouse must not turn the camera behind it, Escape closes it.
$a = Angles; $n = (Select-String -Path $log -Pattern 'flash call: PopulatePauseMenu').Count
In 'key Escape 150'; Start-Sleep 3
Check 'Escape opens the pause menu' ((Select-String -Path $log -Pattern 'flash call: PopulatePauseMenu').Count -gt $n) 'PopulatePauseMenu'
In 'mouse 1500 300 500'; Start-Sleep 2
$r = (Select-String -Path $log -Pattern 'flash call: ResumeGame').Count
In 'key Escape 150'; Start-Sleep 3
Check 'Escape closes the pause menu' ((Select-String -Path $log -Pattern 'flash call: ResumeGame').Count -gt $r) 'ResumeGame'
$b = Angles; $d = Turn $a.yaw $b.yaw
Check 'no turning while paused' ([math]::Abs($d) -lt 0.03 -and [math]::Abs($b.pitch - $a.pitch) -lt 0.03) ("yaw moved {0:N3}, pitch {1:N3}" -f $d, ($b.pitch - $a.pitch))

# Walking and jumping with keys.
$p0 = Where-Player; In 'key W 1500'; Start-Sleep -Milliseconds 2200; $p1 = Where-Player
Check 'W walks' ($p0 -ne $p1 -and $p1 -ne '?') "$p0 -> $p1"
In 'key Space 150'; Start-Sleep -Milliseconds 350; $p2 = Where-Player
Check 'Space jumps (position read in the air)' ($p2 -ne $p1) "$p1 -> $p2"
Start-Sleep 2
# The camera keeps the direction the mouse gave it while the player walks (no pull back behind the player).
In 'mouse 1300 0 300'; Start-Sleep -Milliseconds 1200; $a = Angles
In 'key S 3000'; Start-Sleep -Milliseconds 4200; $b = Angles; $d = Turn $a.yaw $b.yaw
Check 'camera keeps its direction while walking 3 s' ([math]::Abs($d) -lt 0.15) ("yaw drifted {0:N3} rad" -f $d)
"problems: " + ((Get-Content (Join-Path $runs "${Name}_problems.log") -ErrorAction SilentlyContinue | Measure-Object).Count) + " line(s) in ${Name}_problems.log"
