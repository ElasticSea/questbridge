# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
# The only allowed way to start the Unity Editor (batchmode builds and tests) from scripts. Players
# that stream to the headset use start_player.ps1 instead: on this hidden desktop they never submit frames.
# The process runs on a separate, never-shown desktop (WinSta0\QuestBridgeHidden), so none of its
# windows can take focus. (SW_HIDE on the visible desktop is not enough: a test window
# started that way still took the foreground.) Waits for exit and returns the exit code (-1 on
# timeout, after killing it). The hidden desktop goes away with this script, so keep it running
# while the child runs: run the script itself in the background instead of detaching the child.
#   .\unity_hidden.ps1 -ArgLine "-batchmode -nographics -quit -projectPath ... -logFile x.log"
param(
    [string]$Exe = $(if ($env:UNITY_EXE) { $env:UNITY_EXE } else { "C:\Program Files\Unity\Hub\Editor\6000.3.11f1\Editor\Unity.exe" }),
    [string]$ArgLine = "",
    [string]$WorkingDirectory = (Get-Location).Path,
    [int]$TimeoutS = 1800
)
$ErrorActionPreference = "Stop"

if (-not ("QuestBridge.HiddenLauncher" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace QuestBridge {
public static class HiddenLauncher {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktop(string name, IntPtr device, IntPtr devmode, int flags, uint access, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out int code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, int code);

    const uint DESKTOP_ALL = 0x01FF | 0x000F0000;
    const int STARTF_USESHOWWINDOW = 0x1;
    const short SW_HIDE = 0;
    const uint CREATE_NO_WINDOW = 0x08000000;
    static IntPtr desktop;  // kept open while this script runs
    static IntPtr process;

    // Starts exe on the hidden desktop; returns the PID.
    public static int Start(string exe, string args, string cwd) {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (desktop == IntPtr.Zero) {
            desktop = CreateDesktop("QuestBridgeHidden", IntPtr.Zero, IntPtr.Zero, 0, DESKTOP_ALL, IntPtr.Zero);
            if (desktop == IntPtr.Zero) throw new Win32Exception();
        }
        si.lpDesktop = @"WinSta0\QuestBridgeHidden";
        var cmd = new System.Text.StringBuilder("\"" + exe + "\" " + args);
        PROCESS_INFORMATION pi;
        if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, CREATE_NO_WINDOW, IntPtr.Zero, cwd, ref si, out pi))
            throw new Win32Exception();
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return pi.dwProcessId;
    }

    // Exit code, or -1 after killing the child when it runs longer than ms.
    public static int Wait(uint ms) {
        if (WaitForSingleObject(process, ms) != 0) { TerminateProcess(process, -1); return -1; }
        int code;
        GetExitCodeProcess(process, out code);
        return code;
    }
}
}
"@
}

$exePath = (Resolve-Path $Exe).Path
$procId = [QuestBridge.HiddenLauncher]::Start($exePath, $ArgLine, (Resolve-Path $WorkingDirectory).Path)
Write-Host "started $([IO.Path]::GetFileName($exePath)) pid $procId on the hidden desktop"
exit [QuestBridge.HiddenLauncher]::Wait([uint32]($TimeoutS * 1000))
