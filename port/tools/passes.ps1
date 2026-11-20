# Step through one frame of the native renderer in a running test game: for
# each number in -Passes, the frame is ended at that resolve (its texture
# shown instead of the front buffer) and a screenshot taken:
# runs\<Name>_pass<nn>.png. 0 = the whole frame. Needs the game started with
# XMD_NATIVE_STOP_FILE=<port>\runs\stop_after.txt (route_run.ps1 -Native sets it).
#   .\passes.ps1 -Passes 1,2,3,20 [-Name nat]
# -Draws n,m,..: instead, the frame stopped at resolve -At with only its first n (m ...) draws made:
# runs\<Name>_draw<nnn>.png - where in the scene's draws something goes wrong.
#   .\passes.ps1 -At 6 -Draws 40,100,200,275
# -Ps hash,..: the frame's draws ended before / after the first draw with that pixel shader (the first 8 hex
# digits of its hash), shown at resolve -At (0: the frame as presented): runs\<Name>_ps<hash>_0.png / _1.png.
#   .\passes.ps1 -Ps F262C927,1B56E626
param([int[]]$Passes = @(1), [string]$Name = "nat", [int]$At = 0, [int[]]$Draws = @(), [string[]]$Ps = @())
$port = Split-Path $PSScriptRoot -Parent
$file = Join-Path $port "runs\stop_after.txt"
if ($Ps) {
    Set-Content $file "$At" -Encoding ascii
    foreach ($h in $Ps) {
        foreach ($after in 0, 1) {
            Set-Content "$file.ps" "$h $after" -Encoding ascii
            Start-Sleep -Milliseconds 2200
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shot.ps1') -Name ("{0}_ps{1}_{2}" -f $Name, $h, $after) | Out-Null
        }
    }
    Set-Content "$file.ps" "0 0" -Encoding ascii
    Set-Content $file "0" -Encoding ascii
    return
}
if ($Draws) {
    Set-Content $file "$At" -Encoding ascii
    foreach ($n in $Draws) {
        Set-Content "$file.draws" "$n" -Encoding ascii
        Start-Sleep -Milliseconds 1800
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shot.ps1') -Name ("{0}_draw{1:d3}" -f $Name, $n) | Out-Null
    }
    Set-Content "$file.draws" "0" -Encoding ascii
    Set-Content $file "0" -Encoding ascii
    return
}
foreach ($n in $Passes) {
    Set-Content $file "$n" -Encoding ascii
    Start-Sleep -Milliseconds 1800
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shot.ps1') -Name ("{0}_pass{1:d2}" -f $Name, $n) | Out-Null
}
Set-Content $file "0" -Encoding ascii
