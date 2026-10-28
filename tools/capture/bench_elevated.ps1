# One UAC prompt for a whole bench slot (tools\bench_slot.ps1): stops OVRService, then serves
# trigger files from the non-elevated slot driver until capture\STOP exists:
#   capture\CAP_START -> start a USB capture (pcaps\<Name>-<stamp>.pcap)  capture\CAP_STOP -> stop it
#   capture\REPLUG    -> restart the hub the Quest is on (only with a host running: an unanswered
#                       HELLO across a replug wedges xrspd, docs/protocol/connecting.md)
#   capture\OVR_START -> Start-Service OVRService      capture\OVR_STOP -> Stop-Service OVRService
#   capture\STOP      -> stop the capture, stop OVRService, exit
# Usage: .\bench_elevated.ps1 -Name slot [-Interface \\.\USBPcap1]   (asks for UAC itself)
param([string]$Name = "slot", [string]$Interface = "", [switch]$Elevated)

$here = $PSScriptRoot
if (-not $Elevated) {
    $ifaceArg = if ($Interface) { "-Interface $Interface" } else { "" }
    Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Name $Name $ifaceArg -Elevated"
    Write-Host "bench_elevated: UAC requested; status in out\captures\$Name-*.status"
    return
}

$outDir = Join-Path $here "pcaps"
New-Item -ItemType Directory -Force $outDir | Out-Null
$triggers = "CAP_START", "CAP_STOP", "REPLUG", "OVR_START", "OVR_STOP"
foreach ($f in @("STOP") + $triggers) { Remove-Item (Join-Path $here $f) -ErrorAction SilentlyContinue }
$status = Join-Path $outDir "$Name-$(Get-Date -Format "yyyyMMdd-HHmmss").status"
function Say($text) { "$(Get-Date -Format HH:mm:ss) $text" | Add-Content $status }

Stop-Service OVRService -Force -ErrorAction SilentlyContinue
Say "ovr $((Get-Service OVRService).Status)"
$cmd = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
# USBPcap filter whose device tree contains the Quest
$iface = $Interface
$tmp = Join-Path $env:TEMP "usbpcap_cfg_bench.txt"
foreach ($n in $(if ($iface) { @() } else { 1..16 })) {
    cmd /c "`"$cmd`" --extcap-interface \\.\USBPcap$n --extcap-config > `"$tmp`" 2>&1"
    if ((Get-Content $tmp -Raw -ErrorAction SilentlyContinue) -match 'Reality Labs|Oculus|Quest') { $iface = "\\.\USBPcap$n"; break }
}
Say "interface $iface"
$cap = $null
while (-not (Test-Path (Join-Path $here "STOP"))) {
    foreach ($t in $triggers) {
        $f = Join-Path $here $t
        if (-not (Test-Path $f)) { continue }
        Remove-Item $f
        switch ($t) {
            "CAP_START" {
                if (-not $cap -and $iface) {
                    $pcap = Join-Path $outDir "$Name-$(Get-Date -Format "yyyyMMdd-HHmmss").pcap"
                    $cap = Start-Process $cmd -ArgumentList "-d $iface -o `"$pcap`" -A" -WindowStyle Hidden -PassThru
                    Say "capture pid $($cap.Id) -> $pcap"
                }
            }
            "CAP_STOP" {
                if ($cap) { Stop-Process -Id $cap.Id -Force -ErrorAction SilentlyContinue; Say "capture stopped"; $cap = $null }
            }
            "REPLUG" {
                $quest = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\VID_2833&PID_' } | Select-Object -First 1
                if ($quest) {
                    $hub = (Get-PnpDeviceProperty -InstanceId $quest.InstanceId -KeyName DEVPKEY_Device_Parent).Data
                    pnputil /restart-device "$hub" | Out-Null
                    Say "replugged via $hub"
                } else { Say "replug: no Quest present" }
            }
            "OVR_START" { Start-Service OVRService; Say "ovr $((Get-Service OVRService).Status)" }
            "OVR_STOP" { Stop-Service OVRService -Force; Say "ovr $((Get-Service OVRService).Status)" }
        }
    }
    Start-Sleep -Milliseconds 500
}
Remove-Item (Join-Path $here "STOP")
if ($cap) { Stop-Process -Id $cap.Id -Force -ErrorAction SilentlyContinue }
Stop-Service OVRService -Force -ErrorAction SilentlyContinue
Say "stopped; ovr $((Get-Service OVRService).Status)"
