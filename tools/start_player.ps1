# Starts a Unity player that streams to the headset (such as FirstLight) on the user's desktop
# without taking focus. Players must stay on the real desktop: on unity_hidden.ps1's hidden desktop
# they reach Streaming but never submit frames (headset stays on Meta's loading dots). The Editor
# and tests keep using unity_hidden.ps1.
#   - embedded (Unity -parentHWND) as a child of a host window this script owns: WS_EX_NOACTIVATE
#     tool window, off-screen (-NoMinimize: visible in the bottom-left corner of the primary screen).
#     A child window can't be foreground, so the player can't take focus even while its engine
#     init blocks its window (a top-level player window, minimized or not, held the foreground for
#     ~10 s at startup when the user was idle; -NoEmbed keeps that old top-level mode)
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
    [switch]$NoMinimize,
    [switch]$NoEmbed
)
$ErrorActionPreference = "Stop"

if (-not ("QuestBridge.PlayerHost" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace QuestBridge {
public static class PlayerHost {
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

    delegate IntPtr WndProc(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS {
        public uint style; public WndProc lpfnWndProc; public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground; public string lpszMenuName, lpszClassName;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClass(ref WNDCLASS c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateWindowEx(uint ex, string cls, string title, uint style, int x, int y, int w, int h,
        IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProc(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    static WndProc proc = DefWindowProc;  // kept alive for the window's lifetime
    static IntPtr host;

    const uint WS_POPUP = 0x80000000, WS_CLIPCHILDREN = 0x02000000;
    const uint WS_EX_NOACTIVATE = 0x08000000, WS_EX_TOOLWINDOW = 0x80;

    // Host window for -parentHWND: never activated, off-screen unless visible is set
    public static long CreateHost(int w, int h, bool visible) {
        var c = new WNDCLASS();
        c.lpfnWndProc = proc;
        c.lpszClassName = "QuestBridgePlayerHost";
        RegisterClass(ref c);
        int x = visible ? 0 : -10 * w, y = visible ? GetSystemMetrics(1) - h - 48 : -10 * h;
        host = CreateWindowEx(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, c.lpszClassName, "QuestBridge player",
            WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (host == IntPtr.Zero) throw new Win32Exception();
        ShowWindow(host, SW_SHOWNOACTIVATE);
        return host.ToInt64();
    }

    // Child windows of another process send messages to the host: keep its queue moving
    public static void Pump() {
        MSG m;
        while (PeekMessage(out m, IntPtr.Zero, 0, 0, 1)) { TranslateMessage(ref m); DispatchMessage(ref m); }
    }

    public static void DestroyHost() { if (host != IntPtr.Zero) DestroyWindow(host); host = IntPtr.Zero; }

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
if (-not $NoEmbed) {
    $hwnd = [QuestBridge.PlayerHost]::CreateHost(320, 180, [bool]$NoMinimize)
    $ArgLine = "-parentHWND $hwnd $ArgLine"
}
$procId = [QuestBridge.PlayerHost]::Start($exePath, $ArgLine, (Resolve-Path $WorkingDirectory).Path, -not $NoMinimize)
Write-Host "started $([IO.Path]::GetFileName($exePath)) pid $procId ($(if ($NoEmbed) { "top-level" } else { "embedded" }), no-activate, focus guard on)"
$started = Get-Date
$deadline = $started.AddSeconds($TimeoutS)
$unlocked = $false
$code = -2
while ($code -eq -2) {
    if ((Get-Date) -gt $deadline) { $code = [QuestBridge.PlayerHost]::Kill(); break }
    if ([QuestBridge.PlayerHost]::Guard()) { Write-Host "$(Get-Date -Format HH:mm:ss.fff) FOCUS taken by the player, restored" }
    # Keep the foreground lock while the player starts up (window creation, XR init), then release it.
    if (-not $unlocked -and ((Get-Date) - $started).TotalSeconds -gt 30) { [QuestBridge.PlayerHost]::Unlock(); $unlocked = $true }
    [QuestBridge.PlayerHost]::Pump()
    $code = [QuestBridge.PlayerHost]::Poll(20)
}
[QuestBridge.PlayerHost]::DestroyHost()
if (-not $unlocked) { [QuestBridge.PlayerHost]::Unlock() }
exit $code
