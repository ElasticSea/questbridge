# Logs the foreground window's process every 200 ms and fails if a forbidden process ever owns it.
# Run it alongside a Unity run to prove nothing steals focus.
#   .\focus_watch.ps1 -Seconds 300 -Out focus.log [-Forbidden 'Unity|SensorProbe|FirstLight']
# Exit 0 = never in front, 1 = a forbidden process took the foreground (lines marked STOLEN).
param(
    [int]$Seconds = 300,
    [string]$Out = "focus.log",
    [string]$Forbidden = "^(Unity|Unity Hub|SensorProbe|FirstLight|UnityCrashHandler64)$",
    [int]$IntervalMs = 200
)
if (-not ("QuestBridge.Focus" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace QuestBridge {
public static class Focus {
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    public static uint ForegroundPid() {
        uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid;
    }
}
}
"@
}
$end = (Get-Date).AddSeconds($Seconds)
$last = ""; $stolen = 0; $samples = 0
"focus_watch start $(Get-Date -Format o) forbidden=$Forbidden" | Set-Content $Out
while ((Get-Date) -lt $end) {
    $procId = [QuestBridge.Focus]::ForegroundPid()
    $name = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
    $samples++
    $bad = $name -match $Forbidden
    if ($bad) { $stolen++ }
    $now = "$procId $name"
    if ($now -ne $last -or $bad) {
        "$(Get-Date -Format HH:mm:ss.fff) $now$(if ($bad) { '  STOLEN' })" | Add-Content $Out
        $last = $now
    }
    Start-Sleep -Milliseconds $IntervalMs
}
"focus_watch end: $samples samples, $stolen with a forbidden process in front" | Add-Content $Out
Get-Content $Out -Tail 1
exit [int]($stolen -gt 0)
