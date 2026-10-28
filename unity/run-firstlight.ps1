# Run the FirstLight player for a bench run without taking mouse or focus: through
# tools\start_player.ps1 (user's desktop, minimized, not activated, focus guard; on unity_hidden.ps1's
# hidden desktop it never submits frames), small window, native log on. Blocks until the player
# exits (run the script in the background). Logs go to out\runs\firstlight-<stamp>\.
#   .\run-firstlight.ps1                      guided -sequence run, 600 s
#   .\run-firstlight.ps1 -Seconds 120 -Flags "-hands"
#   .\run-firstlight.ps1 -Flags "-sequence-audio"   audio part, then hands the session over
# On exit the native log ends with "resume with XRSP_..." for tools\pyhost\handshake.py XRSP_ATTACH.
# Stop OVRService first (tools\windows\ovr_service.ps1 stop). Don't kill the player mid-handshake: the
# headset then sends no new HELLO until it reboots (adb reboot).
# -NoMinimize: show the small window (still not activated) in case a minimized player stops rendering.
param([int]$Seconds = 600, [string]$Flags = "-sequence", [switch]$NoMinimize)

$root = Split-Path $PSScriptRoot
$exe = Join-Path $PSScriptRoot "BridgeTest\Build\FirstLight\FirstLight.exe"
$dir = Join-Path $root ("out\runs\firstlight-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $dir | Out-Null

# Headset media volume (the user can't hear anything at 0)
$adb = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"
$vol = & $adb -s QUEST0SERIAL00 shell cmd media_session volume --stream 3 --get 2>$null | Select-String "volume is (\d+)"
if ($vol) {
    $level = [int]$vol.Matches[0].Groups[1].Value
    Write-Host "headset media volume $level/15$(if ($level -eq 0) { ' -- MUTED, turn it up' })"
} else {
    Write-Host "headset media volume: adb not reachable"
}

$env:QL_LOG = "1"
$env:QL_LOG_FILE = Join-Path $dir "native.log"
$argList = "$Flags -seconds $Seconds -screen-fullscreen 0 -screen-width 320 -screen-height 180 -logFile `"$(Join-Path $dir 'player.log')`""
Write-Host "FirstLight logs in $dir"
& (Join-Path $root "tools\start_player.ps1") -Exe $exe -ArgLine $argList -TimeoutS ($Seconds + 60) -NoMinimize:$NoMinimize
exit $LASTEXITCODE
