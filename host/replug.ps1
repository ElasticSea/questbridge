# Software "replug" of the Quest: restart the USB hub it sits behind (one UAC prompt).
# Optionally start/stop Meta's OVRService in the same elevated step.
#   .\replug.ps1                 just replug
#   .\replug.ps1 -Service start  start OVRService, then replug
#   .\replug.ps1 -Service stop   stop OVRService, then replug
param([ValidateSet("", "start", "stop")][string]$Service = "")

# Hub whose downstream port the Quest is on (location PCI(1400)#USBROOT(0)#USB(21))
$quest = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\VID_2833&PID_50[0-9A-F]{2}\\' } | Select-Object -First 1
if ($quest) {
    $hub = (Get-PnpDeviceProperty -InstanceId $quest.InstanceId -KeyName DEVPKEY_Device_Parent).Data
} else {
    $hub = 'USB\VID_0BDA&PID_0420\5&30741CDD&0&21'
}

$cmds = @()
if ($Service -eq "start") { $cmds += "Start-Service OVRService" }
if ($Service -eq "stop") { $cmds += "Stop-Service OVRService -Force" }
$cmds += "pnputil /restart-device '$hub' | Out-Null"
Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "-NoProfile -Command $($cmds -join '; ')"
Write-Host "replugged via $hub"
