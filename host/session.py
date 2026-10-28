"""One-shot test session: run our host while recording the headset log, then
print a single summary with host events and the relevant xrspd/Link lines.

Usage: py host/session.py [host-seconds] [--hello-wait S]
Prereqs: OVRService stopped (host/ovr_service.ps1 stop), adb over Wi-Fi.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "capture", "runs")
ADB = os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Packages",
                   "Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe", "platform-tools", "adb.exe")
ADB_TARGET = os.environ.get("QUEST_ADB", "192.0.2.10:5555")

# Headset log lines worth seeing; everything else is noise
KEEP = re.compile(r"xrspd|LS_PCLinkService|XrspBroker|xrstreaming")
DROP = re.compile(r"Filtering out|statsd|Logging |\s->\s|IPreferencesManager|W/Role")


def adb(*args, timeout=20):
    return subprocess.run([ADB, "-s", ADB_TARGET, *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def main():
    seconds = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "60"
    env = dict(os.environ)
    if "--hello-wait" in sys.argv:
        env["XRSP_HELLO_WAIT"] = sys.argv[sys.argv.index("--hello-wait") + 1]
    os.makedirs(RUNS, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    host_log = os.path.join(RUNS, f"{stamp}-host.log")
    head_log = os.path.join(RUNS, f"{stamp}-headset.log")

    adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
    adb("logcat", "-c")
    # Headset clock differs from the PC's; measure the offset so both logs line up
    before = time.time()
    quest_now = float(adb("shell", "date", "+%s.%N").stdout.strip())
    skew = quest_now - (before + time.time()) / 2
    with open(head_log, "w", encoding="utf-8") as hf:
        logcat = subprocess.Popen([ADB, "-s", ADB_TARGET, "logcat", "-v", "epoch"], stdout=hf,
                                  stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
        t0 = time.time()
        with open(host_log, "w", encoding="utf-8") as lf:
            host = subprocess.run([sys.executable, "-u", os.path.join(ROOT, "host", "handshake.py"), seconds],
                                  stdout=lf, stderr=subprocess.STDOUT, env=env)
        time.sleep(1)
        logcat.terminate()

    events = []
    with open(host_log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*([\d.]+) (.*)", line.rstrip())
            if m:
                events.append((t0 + float(m.group(1)), "PC  ", m.group(2)))
            elif line.strip():
                events.append((time.time(), "PC  ", line.rstrip()))
    with open(head_log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*(\d+\.\d+)\s+\d+\s+\d+\s+\w\s+(.*)", line.rstrip())
            if m and KEEP.search(line) and not DROP.search(line):
                events.append((float(m.group(1)) - skew, "QST ", m.group(2)[:170]))
    for ts, who, text in sorted(events):
        print(f"{ts - t0:8.3f} {who}{text}")
    print(f"host exit={host.returncode}; headset clock skew {skew:+.3f}s; logs: {os.path.relpath(host_log, ROOT)}, {os.path.relpath(head_log, ROOT)}")


if __name__ == "__main__":
    main()
