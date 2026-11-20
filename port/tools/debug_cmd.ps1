# Send debug commands to the running test game (src\debug_commands.cpp) - they run as Lua in the game:
#   .\debug_cmd.ps1 'teleport <actor id>' 'spawn <node id> <node id>' 'event <name>' 'flag <name>' 'lua <code>'
# Actor IDs, event and flag names: the mission scripts, tools\lua_scripts.py -> runs\lua\*.lua.
# The game runs them at its next call into its scripts; -Nudge walks the player a step so that one comes
# (a still player in an empty street calls none). Prints the result lines from the log.
param([Parameter(ValueFromRemainingArguments)] [string[]]$Lines, [switch]$Nudge)
$port = Split-Path $PSScriptRoot -Parent
Add-Content (Join-Path $port 'runs\debug.txt') ($Lines -join "`n") -Encoding ascii
if ($Nudge) { Add-Content (Join-Path $port 'runs\input.txt') "stick L 0 32000 700`nwait 300`nstick L 0 -32000 700`nwait 300" -Encoding ascii }
Start-Sleep 4
$log = Get-ChildItem (Join-Path $port 'runs\*.log') | Where-Object { $_.Name -notmatch 'problems|build' } | Sort-Object LastWriteTime | Select-Object -Last 1
Select-String -Path $log.FullName -Pattern 'debug command: (ok|error|running)' | Select-Object -Last ($Lines.Count + 1) |
    ForEach-Object { $_.Line -replace '^\S+ (\S+)\] \[\w+\] \[core\] \[t\d+\] ', '$1 ' }
