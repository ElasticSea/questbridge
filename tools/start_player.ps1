# Starts a Unity player that streams to the headset (FirstLight, SensorProbe) on the user's desktop
# without taking focus. Players must stay on the real desktop: on unity_hidden.ps1's hidden desktop
# they reach Streaming but never submit frames (headset stays on Meta's loading dots). The Editor
# and tests keep using unity_hidden.ps1.
#   - window created minimized and not activated (SW_SHOWMINNOACTIVE; -NoMinimize: SW_SHOWNOACTIVATE)
#   - LockSetForegroundWindow while it starts, so it can't call SetForegroundWindow
#   - focus guard for the whole run: if a window of the player becomes foreground anyway, the
#     previous foreground window is restored and the player minimized; each case is logged (FOCUS)
# Waits for exit and returns its exit code (-1 on timeout, after killing it).
#   .\start_player.ps1 -Exe ..\unity\BridgeTest\Build\FirstLight\FirstLight.exe -ArgLine "-seconds 90" -TimeoutS 150
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$ArgLine = "",
    [string]$WorkingDirectory = (Get-Location).Path,
    [int]$TimeoutS = 1800,
    [switch]$NoMinimize
)
$ErrorActionPreference = "Stop"

if (-not ("QuestLink.PlayerLauncher" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace QuestLink {
public static class PlayerLauncher {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out int code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, int code);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool LockSetForegroundWindow(uint code);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);

    const int STARTF_USESHOWWINDOW = 0x1;
    const short SW_SHOWMINNOACTIVE = 7, SW_SHOWNOACTIVATE = 4;
    const uint LSFW_LOCK = 1, LSFW_UNLOCK = 2;
    static IntPtr process;
    static int pid;
    static IntPtr previous;

    public static int Start(string exe, string args, string cwd, bool minimize) {
        previous = GetForegroundWindow();
        LockSetForegroundWindow(LSFW_LOCK);
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = minimize ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
        var cmd = new System.Text.StringBuilder("\"" + exe + "\" " + args);
        PROCESS_INFORMATION pi;
        if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi))
            throw new Win32Exception();
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        pid = pi.dwProcessId;
        return pid;
    }

    // One guard step: if the player owns the foreground window, give it back. Returns true if it had to.
    public static bool Guard() {
        IntPtr fg = GetForegroundWindow();
        uint owner;
        uint fgThread = GetWindowThreadProcessId(fg, out owner);
        if (owner != (uint)pid) return false;
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, true);
        if (previous != IntPtr.Zero) SetForegroundWindow(previous);
        ShowWindow(fg, SW_SHOWMINNOACTIVE);
        AttachThreadInput(me, fgThread, false);
        return true;
    }

    public static void Unlock() { LockSetForegroundWindow(LSFW_UNLOCK); }

    // Exit code, -2 while still running after ms, or -1 after TerminateProcess.
    public static int Poll(uint ms) {
        if (WaitForSingleObject(process, ms) != 0) return -2;
        int code;
        GetExitCodeProcess(process, out code);
        return code;
    }

    public static int Kill() { TerminateProcess(process, -1); return -1; }
}
}
"@
}

$exePath = (Resolve-Path $Exe).Path
$procId = [QuestLink.PlayerLauncher]::Start($exePath, $ArgLine, (Resolve-Path $WorkingDirectory).Path, -not $NoMinimize)
Write-Host "started $([IO.Path]::GetFileName($exePath)) pid $procId (no-activate, focus guard on)"
$started = Get-Date
$deadline = $started.AddSeconds($TimeoutS)
$unlocked = $false
$code = -2
while ($code -eq -2) {
    if ((Get-Date) -gt $deadline) { $code = [QuestLink.PlayerLauncher]::Kill(); break }
    if ([QuestLink.PlayerLauncher]::Guard()) { Write-Host "$(Get-Date -Format HH:mm:ss.fff) FOCUS taken by the player, restored" }
    # Keep the foreground lock while the player starts up (window creation, XR init), then release it.
    if (-not $unlocked -and ((Get-Date) - $started).TotalSeconds -gt 30) { [QuestLink.PlayerLauncher]::Unlock(); $unlocked = $true }
    $code = [QuestLink.PlayerLauncher]::Poll(100)
}
if (-not $unlocked) { [QuestLink.PlayerLauncher]::Unlock() }
exit $code
