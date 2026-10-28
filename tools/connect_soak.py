"""Connect soak: cycle the native host against the plugged-in Quest and check that every cycle
reaches phase 1 (the headset's HELLO) with no replug and nobody in the headset.

Usage: py tools/connect_soak.py [--cycles 12] [--qbhost PATH] [--hello-timeout 10]
                                [--no-sleep] [--force]

Each cycle first leaves the headset in a state that used to need a replug, then starts a
fresh qbhost and times open -> HELLO -> phase 1. Disturbances rotate:
  none      the previous cycle's host was killed after phase 1 (headset HOST_CONNECTED)
  orphan    a host killed right after its BYE: the headset's new HELLO goes unanswered
  early     a host killed 50 ms after opening, before it said anything
  sleep     the headset sleeps 15 s (adb KEYCODE_SLEEP), then wakes
Needs OVRService stopped and no other host (FirstLight, qbhost, handshake.py) on the bench.
Phase 2 (Launch) needs a person, so it is not part of the soak; see docs/protocol/connecting.md.
Exit code 0 only if every cycle reached phase 1. Log: out/runs/connect-soak-<stamp>.txt
"""
import argparse
import os
import queue
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QBHOST = os.path.join(REPO, "native", "build", "bin", "Release", "qbhost.exe")
ADB = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages",
                   "Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe", "platform-tools", "adb.exe")
SERIAL = "QUEST0SERIAL00"
DISTURBANCES = ["none", "orphan", "early", "sleep"]
OTHER_HOSTS = ("FirstLight.exe", "qbhost.exe")


def adb(*args, timeout=15):
    try:
        r = subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def bench_busy():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    busy = sorted({n for n in OTHER_HOSTS if f'"{n}"' in out})
    svc = subprocess.run(["sc", "query", "OVRService"], capture_output=True, text=True).stdout
    if "RUNNING" in svc:
        busy.append("OVRService")
    return busy


class Host:
    """qbhost in listen mode; lines arrive on a queue as (pc_time, text)"""

    def __init__(self, exe, hello_timeout, bye_retry_ms=None):
        env = dict(os.environ, XRSP_WAIT="10", XRSP_HELLO_WAIT=str(hello_timeout))
        env.pop("XRSP_REPLAY", None)
        if bye_retry_ms is not None:
            env["XRSP_BYE_RETRY_MS"] = str(bye_retry_ms)
        self.t0 = time.monotonic()
        self.p = subprocess.Popen([exe, "600"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.lines = queue.Queue()
        self.log = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.put((time.monotonic() - self.t0, line.rstrip()))
        self.lines.put(None)

    def wait_for(self, text, timeout):
        """Seconds since start when a line containing text appeared, or None"""
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                item = self.lines.get(timeout=left)
            except queue.Empty:
                break
            if item is None:
                return None
            self.log.append(f"{item[0]:7.3f} | {item[1]}")
            if text in item[1]:
                return item[0]
        return None

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        time.sleep(0.2)  # let Windows release the WinUSB handle


def disturb(kind, exe, out):
    if kind == "orphan":
        h = Host(exe, 5)
        ok = h.wait_for("BYE", 3) is not None
        time.sleep(0.3)  # the line is logged when queued; let the write go out
        h.kill()
        out(f"  disturb orphan: host killed right after its BYE ({'sent' if ok else 'no BYE seen'})")
        time.sleep(3)  # the headset's fresh HELLO goes out to nobody
    elif kind == "early":
        h = Host(exe, 5)
        h.wait_for("XRSP interface opened", 3)
        time.sleep(0.05)
        h.kill()
        out("  disturb early: host killed 50 ms after opening")
    elif kind == "sleep":
        adb("shell", "input keyevent KEYCODE_SLEEP")
        out("  disturb sleep: headset asleep for 15 s")
        time.sleep(15)
        adb("shell", "input keyevent KEYCODE_WAKEUP; am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null")
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cycles", type=int, default=12)
    ap.add_argument("--qbhost", default=QBHOST)
    ap.add_argument("--hello-timeout", type=float, default=10, help="seconds from open to phase 1")
    ap.add_argument("--no-sleep", action="store_true", help="skip the headset sleep disturbance")
    ap.add_argument("--force", action="store_true", help="run even if another host seems active")
    a = ap.parse_args()

    busy = bench_busy()
    if busy and not a.force:
        print("bench busy: " + ", ".join(busy) + " (stop it, or --force)")
        return 2
    if not os.path.exists(a.qbhost):
        print(f"no qbhost at {a.qbhost} (run native\\build.ps1)")
        return 2

    os.makedirs(os.path.join(REPO, "capture", "runs"), exist_ok=True)
    path = os.path.join(REPO, "capture", "runs", f"connect-soak-{datetime.now():%Y%m%d-%H%M%S}.txt")
    logf = open(path, "w", encoding="utf-8")

    def out(s):
        print(s)
        logf.write(s + "\n")
        logf.flush()

    kinds = [k for k in DISTURBANCES if not (a.no_sleep and k == "sleep")]
    adb("shell", "am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null")
    results = []
    for i in range(a.cycles):
        kind = kinds[i % len(kinds)]
        out(f"cycle {i + 1}/{a.cycles} ({kind})")
        disturb(kind, a.qbhost, out)
        state = adb("shell", "getprop ovr.xrspd.status") or "?"
        h = Host(a.qbhost, a.hello_timeout + 5)
        opened = h.wait_for("XRSP interface opened", 10)
        hello = h.wait_for("0x81 v1", a.hello_timeout) if opened is not None else None
        done = h.wait_for("phase 1 done", 3) if hello is not None else None
        byes = sum(1 for line in h.log if "BYE" in line)
        h.kill()
        ok = done is not None
        lat = hello - opened if ok else None
        results.append((kind, ok, lat, byes))
        out(f"  xrspd.status {state}: " + (f"HELLO {lat:.2f} s after open, {byes} BYE" if ok else "FAILED, no phase 1"))
        if not ok:
            for line in h.log[-15:]:
                out("    " + line)

    good = [r for r in results if r[1]]
    lats = [r[2] for r in good]
    out("")
    out(f"{len(good)}/{len(results)} cycles reached phase 1 without a replug")
    if lats:
        out(f"HELLO after open: median {statistics.median(lats):.2f} s, max {max(lats):.2f} s")
    for k in kinds:
        rs = [r for r in results if r[0] == k]
        if rs:
            out(f"  {k:7s} {sum(r[1] for r in rs)}/{len(rs)}")
    out(f"log: {path}")
    return 0 if len(good) == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
