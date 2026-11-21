# Tries mod-menu entries one after another in a background test game and writes down what each did:
#   .\mod_sweep.ps1 -Ids cp_mission1,cp_m1_fb_bqsintrocompleted,...  [-Out runs\sweep\cp.csv] [-Kind cp|scene]
# cp: "mod run <id>", waits for the warp's fade-in (90 s at most), then the player's position and a window picture
#     (runs\sweep\<id>.png). Result: ok / save (the saved game's own spot: the load fell back) / same (did not
#     move) / hang (no fade-in: the game is restarted) / crash.
# scene: -Before <checkpoint id> is loaded first (once per run of ids that share it), then the scene is started and
#     a picture is taken 14 s later (7 s into the scene); the note has the game's menu calls seen meanwhile.
# The test slot is shared: the sweep waits while a game that is not its own runs, and stops its game while the file
# runs\sweep_pause.txt exists (another chat that needs the slot creates it and deletes it afterwards).
param([string[]]$Ids, [string]$Out = "", [string]$Kind = "cp", [string]$Before = "", [int]$SceneWait = 14,
      [string]$Capture = "", [switch]$Keep)   # -Keep: the game is left running, and a running sweep game is taken over
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port 'runs'
$dir = Join-Path $runs 'sweep'
[void](New-Item -ItemType Directory -Force $dir)
if (-not $Out) { $Out = Join-Path $dir "$Kind.csv" }
if (-not $Capture) { $Capture = Join-Path $runs 'test_userdata\captures\capture_20261120_082736' }
$log = Join-Path $runs 'sweep.log'
$pause = Join-Path $runs 'sweep_pause.txt'
$saveSpot = @(-25464.869, -1183.900, 3082.643)
$script:mine = 0
if ($Keep -and (Test-Path (Join-Path $runs 'sweep.log'))) {
    $id = [int](Get-Content (Join-Path $runs 'test_game.pid') -ErrorAction SilentlyContinue)
    if ($id -and (Get-Process -Id $id -ErrorAction SilentlyContinue | Where-Object ProcessName -eq 'xmd')) { $script:mine = $id }
}

function Alive { $script:mine -and (Get-Process -Id $script:mine -ErrorAction SilentlyContinue | Where-Object ProcessName -eq 'xmd') }
function StopMine {
    if (Alive) {
        $id = [int](Get-Content (Join-Path $runs 'test_game.pid') -ErrorAction SilentlyContinue)
        if ($id -eq $script:mine) { & (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null }
    }
    $script:mine = 0
}
function Lines { if (Test-Path $log) { @([IO.File]::ReadAllLines($log)) } else { @() } }
function ReadShared($path) {
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader $fs).ReadToEnd() -split "`r?`n" } finally { $fs.Close() }
}
function LogFrom([int]$from) { $all = @(ReadShared $log); if ($all.Count -gt $from) { $all[$from..($all.Count - 1)] } else { @() } }
function LogCount { if (Test-Path $log) { @(ReadShared $log).Count } else { 0 } }
function Send([string]$line) { Add-Content (Join-Path $runs 'debug.txt') $line -Encoding ascii }
function Where-Now([int]$Tries = 10) {
    $n = LogCount
    Send 'where'
    for ($i = 0; $i -lt $Tries; $i++) {
        Start-Sleep 1
        $m = LogFrom $n | Select-String -Pattern 'debug command: where: (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' | Select-Object -Last 1
        if ($m) { return @([double]$m.Matches[0].Groups[1].Value, [double]$m.Matches[0].Groups[2].Value, [double]$m.Matches[0].Groups[3].Value) }
    }
    return $null
}
function Near($a, $b) { $a -and $b -and ([math]::Abs($a[0] - $b[0]) + [math]::Abs($a[1] - $b[1]) + [math]::Abs($a[2] - $b[2])) -lt 30 }
function Shot([string]$name) {
    & (Join-Path $PSScriptRoot 'gshot.ps1') -Name "sweep_tmp" 2>$null | Out-Null   # (the window picture, shot.ps1, came out black)
    $src = Join-Path $runs 'sweep_tmp.png'
    if (Test-Path $src) { Move-Item $src (Join-Path $dir "$name.png") -Force }
}
function Start-Game {
    for (;;) {
        while (Test-Path $pause) { Start-Sleep 10 }
        # (a game or a build that is not this sweep's: wait for it)
        $quiet = 0
        while ($quiet -lt 3) {
            # (builds of other ports on this PC do not count)
            $busy = Get-Process xmd -ErrorAction SilentlyContinue
            if (-not $busy) {
                $busy = Get-CimInstance Win32_Process -Filter "name='ninja.exe' or name='cmake.exe'" |
                    Where-Object { "$($_.CommandLine) $($_.ExecutablePath)" -match 'X-Men Destiny' }
            }
            if ($busy) { $quiet = 0 } else { $quiet++ }
            Start-Sleep 2
        }
        $r = & (Join-Path $PSScriptRoot 'resume.ps1') -Capture $Capture -Name sweep -NoPause -Timeout 420
        if ("$r" -match 'in the level') {
            $script:mine = [int](Get-Content (Join-Path $runs 'test_game.pid'))
            Send 'mod run god'
            Start-Sleep 2
            return
        }
        "start failed: $r"
        $id = [int](Get-Content (Join-Path $runs 'test_game.pid') -ErrorAction SilentlyContinue)
        if ($id) { & (Join-Path $PSScriptRoot 'stop_test.ps1') | Out-Null }
        Start-Sleep 10
    }
}
# One warp; returns status and position text.
function Warp-To([string]$id, [string]$shot) {
    $before = Where-Now
    $n = LogCount
    $t0 = Get-Date
    Send "mod run $id"
    $faded = $false
    while (((Get-Date) - $t0).TotalSeconds -lt 300) {
        Start-Sleep 3
        if (-not (Alive)) { return @('crash', '', [int]((Get-Date) - $t0).TotalSeconds) }
        $new = LogFrom $n
        if ($new | Select-String -Pattern 'no entry' -Quiet) { return @('no-entry', '', 0) }
        if ($new | Select-String -Pattern 'checkpoint \S+ NOT loaded' -Quiet) { return @('refused', '', [int]((Get-Date) - $t0).TotalSeconds) }
        if ($new | Select-String -Pattern 'ok: SetPPFadeInOutByName' -Quiet) { $faded = $true; break }
    }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    if (-not $faded) {
        if ($shot) { Shot $shot }
        # (what the game was doing: the end of its log, and every thread's stack)
        $tag = if ($shot) { $shot } else { $id }
        @(ReadShared $log) | Select-Object -Last 120 | Set-Content (Join-Path $dir "${tag}_hang.log") -Encoding ascii
        $lldb = 'C:\Program Files\LLVM\bin\lldb.exe'
        if ((Test-Path $lldb) -and (Alive)) {
            & $lldb --batch -p $script:mine -o 'thread backtrace all' -o 'detach' 2>&1 | Set-Content (Join-Path $dir "${tag}_hang_stacks.txt") -Encoding ascii
        }
        return @('hang', '', $secs)
    }
    Start-Sleep 5
    # (a slow disk: the player may come some time after the fade-in)
    $now = $null
    for ($try = 0; $try -lt 8 -and -not $now -and (Alive); $try++) { $now = Where-Now }
    if ($shot) { Shot $shot }
    if (-not (Alive)) { return @('crash', '', $secs) }
    if (-not $now) { return @('hang', '', $secs) }
    $text = '{0:F1} {1:F1} {2:F1}' -f $now[0], $now[1], $now[2]
    if (Near $now $saveSpot) { return @('save', $text, $secs) }
    if (Near $now $before) { return @('same', $text, $secs) }
    return @('ok', $text, $secs)
}

if (-not (Test-Path $Out)) { Set-Content $Out 'id,status,seconds,position,note' -Encoding ascii }
$done = @{}
foreach ($row in (Import-Csv $Out)) { $done[$row.id] = $true }
$prepared = $false
foreach ($id in $Ids) {
    if ($done[$id]) { continue }
    if (Test-Path $pause) { StopMine }
    if (-not (Alive)) { Start-Game; $prepared = $false }
    $note = ''
    if ($Kind -eq 'cp') {
        $r = Warp-To $id $id
        # (a load that fell back may be the fault of the one before it: once more in a fresh game)
        if ($r[0] -in 'hang', 'crash') { StopMine }
    } else {
        if ($Before -and -not $prepared) {
            $w = Warp-To $Before ''
            $note = "before: $($w[0])"
            if ($w[0] -in 'hang', 'crash', 'refused') {
                # (once more in a fresh game before giving the scene up)
                StopMine; Start-Game
                $w = Warp-To $Before ''
                $note = "before, 2nd try: $($w[0])"
                if ($w[0] -in 'hang', 'crash', 'refused') { StopMine; Add-Content $Out "$id,no-level,0,,$note" -Encoding ascii; continue }
            }
            $prepared = $true
            Start-Sleep 3
        }
        $n = LogCount
        $t0 = Get-Date
        Send "mod run $id"
        Start-Sleep $SceneWait
        Shot $id
        $new = LogFrom $n
        $err = $new | Select-String -Pattern 'debug command: error' | Select-Object -First 1
        $calls = @($new | Select-String -Pattern 'flash call: (\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -Unique) -join ' '
        $status = if (-not (Alive)) { 'crash' } elseif ($err) { 'error' } else { 'ran' }
        $r = @($status, '', [int]((Get-Date) - $t0).TotalSeconds)
        if ($status -eq 'crash') { StopMine } else {
            # While a cut-scene plays (and during a load) the game runs none of the commands waiting: a position
            # question without an answer says "busy". It gets 40 s to end; then the game is started again. (No
            # button is pressed to skip: START opened the pause menu when the "scene" was a loading screen.)
            $answers = [bool](Where-Now 5)
            if (-not $answers) {
                $t1 = Get-Date
                for ($try = 0; $try -lt 4 -and -not $answers; $try++) { $answers = [bool](Where-Now 10) }
                $note = (($note, "busy $([int]((Get-Date) - $t1).TotalSeconds + 5) s") | Where-Object { $_ }) -join '; '
                if (-not $answers) { $note += ' and more'; Shot "${id}_later"; StopMine }
            }
            if (Alive) { Send 'mod run control' }
            $prepared = $false
        }
        $note = (($note, $calls) | Where-Object { $_ }) -join '; '
    }
    Add-Content $Out ("{0},{1},{2},{3},{4}" -f $id, $r[0], $r[2], $r[1], ($note -replace ',', ';')) -Encoding ascii
    "{0,-45} {1,-8} {2,3}s {3} {4}" -f $id, $r[0], $r[2], $r[1], $note
}
if (-not $Keep) { StopMine }
"sweep done: $Out"
