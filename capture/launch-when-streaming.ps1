# Waits until the running capture grows like a Link stream (> MinMB per 2 s), then starts a program minimized.
# Use it so nobody waits in the headset during a capture.
#   .\launch-when-streaming.ps1 -Pcap pcaps\x.pcap -Exe ..\unity\SensorProbe\Build\SensorProbe.exe -ArgLine "-probeSteps passthrough,envdepth"
param(
    [Parameter(Mandatory)][string]$Pcap,
    [Parameter(Mandatory)][string]$Exe,
    [string]$ArgLine = "",
    [int]$MinMB = 2,
    [int]$TimeoutS = 600
)
$deadline = (Get-Date).AddSeconds($TimeoutS)
while ((Get-Date) -lt $deadline) {
    $a = (Get-Item $Pcap).Length
    Start-Sleep 2
    $b = (Get-Item $Pcap).Length
    if (($b - $a) -gt $MinMB * 1MB) {
        Write-Host "$(Get-Date -Format HH:mm:ss) streaming ($([int](($b - $a) / 1MB)) MB/2s), starting $Exe"
        if ($ArgLine) { Start-Process $Exe -ArgumentList $ArgLine -WindowStyle Minimized } else { Start-Process $Exe -WindowStyle Minimized }
        exit 0
    }
}
Write-Host "capture never reached $MinMB MB/2s: capture blind or Link not streaming"
exit 1
