# Launches USBPcapCMD elevated (one UAC prompt) on the hub the Quest is on.
# Runs until tools\capture\STOP exists — create it with stop-capture.ps1.
# Output goes to out\captures\<name>-<stamp>.pcap (and .log).
# -Interface \\.\USBPcapN skips auto-detection: start the capture with the Quest unplugged,
# then plug it in, so USBPcap sees it enumerate (a Quest enumerated before the capture may stay invisible).
param([string]$Name = "session", [string]$Interface = "")

$here = $PSScriptRoot
$cmd = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$outDir = Join-Path (Split-Path (Split-Path $here)) "out\captures"
$stop = Join-Path $here "STOP"
New-Item -ItemType Directory -Force $outDir | Out-Null
Remove-Item $stop -ErrorAction SilentlyContinue

# Find the USBPcap filter whose device tree contains the Quest (output must go via cmd redirection)
$iface = if ($Interface) { $Interface } else { $null }
$tmp = Join-Path $env:TEMP "usbpcap_cfg.txt"
foreach ($n in $(if ($iface) { @() } else { 1..16 })) {
    $candidate = "\\.\USBPcap$n"
    cmd /c "`"$cmd`" --extcap-interface $candidate --extcap-config > `"$tmp`" 2>&1"
    if ((Get-Content $tmp -Raw -ErrorAction SilentlyContinue) -match 'Reality Labs|Oculus|Quest') { $iface = $candidate; break }
}
if (-not $iface) { throw "No USBPcap interface sees the Quest." }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$pcap = Join-Path $outDir "$Name-$stamp.pcap"
$log = Join-Path $outDir "$Name-$stamp.log"

# Batch file run elevated: start capture, wait for STOP, kill capture.
$bat = Join-Path $env:TEMP "quest-capture.cmd"
@"
@echo off
start "" /b cmd /c ""$cmd" -d $iface -o "$pcap" -A > "$log" 2>&1"
:wait
if exist "$stop" goto done
timeout /t 1 /nobreak > nul
goto wait
:done
taskkill /f /im USBPcapCMD.exe > nul 2>&1
del "$stop"
"@ | Set-Content -Encoding ascii $bat

Start-Process cmd -Verb RunAs -WindowStyle Minimized -ArgumentList "/c `"$bat`""
Write-Host "Capturing $iface -> $pcap"
