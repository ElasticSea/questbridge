# One UAC prompt for a whole bench slot (tools\bench_slot.ps1): stops OVRService, starts a USB
# capture, then serves trigger files from the non-elevated slot driver until capture\STOP exists:
#   capture\OVR_START -> Start-Service OVRService      capture\OVR_STOP -> Stop-Service OVRService
#   capture\STOP      -> stop the capture, stop OVRService, exit
# Usage: .\bench_elevated.ps1 -Name slot [-Interface \\.\USBPcap1]   (asks for UAC itself)
param([string]$Name = "slot", [string]$Interface = "\\.\USBPcap1", [switch]$Elevated)

$here = $PSScriptRoot
if (-not $Elevated) {
    Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList `
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Name $Name -Interface $Interface -Elevated"
    Write-Host "bench_elevated: UAC requested; status in capture\pcaps\$Name-*.status"
    return
}

$outDir = Join-Path $here "pcaps"
New-Item -ItemType Directory -Force $outDir | Out-Null
foreach ($f in "STOP", "OVR_START", "OVR_STOP") { Remove-Item (Join-Path $here $f) -ErrorAction SilentlyContinue }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$pcap = Join-Path $outDir "$Name-$stamp.pcap"
$status = Join-Path $outDir "$Name-$stamp.status"
function Say($text) { "$(Get-Date -Format HH:mm:ss) $text" | Add-Content $status }

Stop-Service OVRService -Force -ErrorAction SilentlyContinue
Say "ovr $((Get-Service OVRService).Status)"
$cmd = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$cap = Start-Process $cmd -ArgumentList "-d $Interface -o `"$pcap`" -A" -WindowStyle Hidden -PassThru
Say "capture pid $($cap.Id) -> $pcap"
while (-not (Test-Path (Join-Path $here "STOP"))) {
    foreach ($t in "OVR_START", "OVR_STOP") {
        $f = Join-Path $here $t
        if (Test-Path $f) {
            Remove-Item $f
            if ($t -eq "OVR_START") { Start-Service OVRService } else { Stop-Service OVRService -Force }
            Say "ovr $((Get-Service OVRService).Status)"
        }
    }
    Start-Sleep -Milliseconds 500
}
Remove-Item (Join-Path $here "STOP")
Stop-Process -Id $cap.Id -Force -ErrorAction SilentlyContinue
Stop-Service OVRService -Force -ErrorAction SilentlyContinue
Say "stopped; ovr $((Get-Service OVRService).Status)"
