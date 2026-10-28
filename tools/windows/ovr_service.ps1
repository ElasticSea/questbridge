# Stop or start Meta's Link service so our host can own the Quest's USB interface.
#   .\ovr_service.ps1 stop | start | status
param([ValidateSet("stop", "start", "status")][string]$Action = "status")

if ($Action -eq "status") {
    Get-Service OVRService | Select-Object Name, Status
    Get-Process OVRServer_x64, OVRRedir -ErrorAction SilentlyContinue | Select-Object Name, Id
    return
}

$verb = if ($Action -eq "stop") { "Stop-Service OVRService -Force" } else { "Start-Service OVRService" }
Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "-NoProfile -Command $verb"
Get-Service OVRService | Select-Object Name, Status
