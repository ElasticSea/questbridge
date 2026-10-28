"""One-shot test session: run our host while recording the headset log, then
print one merged timeline (headset clock skew corrected) with host events and
the relevant xrspd/Link lines.

Usage: py host/session.py [host-seconds] [--hello-wait S] [--timeout S] [--log-only] [--verbose]
  host-seconds  passed to handshake.py (default 60)
  --hello-wait  XRSP_HELLO_WAIT for the host
  --timeout     hard limit for the whole run, host is killed after it (default 90)
  --log-only    don't start the host, just record the headset for --timeout seconds
  --verbose     keep all xrspd/PCLinkService lines instead of state changes only
Exit code: 0 HELLO received (or --log-only), 1 no HELLO, 2 setup failed.
Before starting it kills stray host/session.py runs and warns if OVRService is running.
Logs and the timeline go to capture/runs/<stamp>-*.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(ROOT, "capture", "runs")
ADB = os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Packages",
                   "Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe", "platform-tools", "adb.exe")
ADB_TARGET = os.environ.get("QUEST_ADB", "192.0.2.10:5555")
HELLO = re.compile(r"Q->PC\s+0x81\b")
REPEATED = re.compile(r"(Q->PC|PC->Q)\s+(topic \d+|clock 0x\w+)")

# Concise headset filter: xrspd state changes, INVITE/session lines, USB events and warnings
CONCISE = re.compile(r"State \[|INVITE|XrspSession|HOST CONNECTING|session (stopped|started)|xrspd event|"
                     r"no subscribed|UsbAvailable_=|XrspdHelperStart|session_state_transition|transitional_message|"
                     r"session_to_start")
NOISE = re.compile(r"Filtering out|Found timings|statsd|Logging |\s->\s|IPreferencesManager|Failed to open file")
VERBOSE_TAGS = re.compile(r"xrspd|LS_PCLinkService|XrspBroker|xrstreaming")
LOGCAT = re.compile(r"\s*(\d+\.\d+)\s+\d+\s+\d+\s+(\w)\s+(.*?)\s*:\s(.*)")

# Kill stray host runs (not tools/handshake.py) and our own logcat recorders; report OVRService
PREFLIGHT = r"""
$me = @(%s)
$killed = @(Get-CimInstance Win32_Process | Where-Object {
    $me -notcontains $_.ProcessId -and $_.CommandLine -and (
    ($_.Name -match '^pythonw?\.exe$|^py\.exe$' -and $_.CommandLine -match '(^|[\s"\\/])(handshake|session)\.py' -and
     $_.CommandLine -notmatch 'tools[\\/]handshake\.py') -or
    ($_.Name -eq 'adb.exe' -and $_.CommandLine -match 'logcat -v epoch')) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "$($_.ProcessId) $($_.CommandLine.Trim())" })
$svc = (Get-Service OVRService -ErrorAction SilentlyContinue).Status
$srv = @(Get-Process OVRServer_x64, OVRRedir -ErrorAction SilentlyContinue).Count
@{killed = $killed; service = "$svc"; servers = $srv} | ConvertTo-Json -Compress
"""


def adb(*args, timeout=10):
    try:
        return subprocess.run([ADB, "-s", ADB_TARGET, *args], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def preflight(out):
    me = ",".join(str(p) for p in {os.getpid(), os.getppid()})
    r = subprocess.run(["powershell", "-NoProfile", "-Command", PREFLIGHT % me], capture_output=True, text=True,
                       timeout=30)
    try:
        out.update(json.loads(r.stdout.strip().splitlines()[-1]))
    except (ValueError, IndexError):
        out["error"] = (r.stderr or r.stdout).strip()[:200]


def prepare_headset(out):
    """Wake the headset, clear its log and measure its clock offset; one round trip each."""
    cmd = ("shell", "am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; logcat -c; echo ok")
    r = adb(*cmd)
    if not (r and "ok" in r.stdout):
        subprocess.run([ADB, "connect", ADB_TARGET], capture_output=True, timeout=10)
        r = adb(*cmd)
        if not (r and "ok" in r.stdout):
            out["error"] = f"adb {ADB_TARGET} unreachable"
            return
    best = None
    for _ in range(3):
        before = time.time()
        r = adb("shell", "date +%s.%N")
        after = time.time()
        try:
            sample = (after - before, float(r.stdout.strip()) - (before + after) / 2)
        except (AttributeError, ValueError):
            continue
        best = min(best or sample, sample)
    if best is None:
        out["error"] = "cannot read headset clock"
        return
    out["rtt"], out["skew"] = best


def run_host(args, env, deadline, host_log, lines):
    """Run handshake.py until it exits or the deadline passes; record (arrival time, line)."""
    host = subprocess.Popen([sys.executable, "-u", os.path.join(ROOT, "host", "handshake.py"), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True,
                            encoding="utf-8", errors="replace")

    def pump():
        with open(host_log, "w", encoding="utf-8") as f:
            for line in host.stdout:
                lines.append((time.time(), line.rstrip()))
                f.write(line)
                f.flush()
                if HELLO.search(line):
                    print("  HELLO received", flush=True)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    status = None
    try:
        host.wait(timeout=max(0, deadline - time.time()))
        status = f"exit={host.returncode}"
    except subprocess.TimeoutExpired:
        status = "killed at timeout"
    except KeyboardInterrupt:
        status = "interrupted"
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    reader.join(timeout=2)
    return status


def headset_events(path, skew, start, verbose):
    events, last = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LOGCAT.match(line.rstrip())
            if not m:
                continue
            ts, level, tag, msg = float(m.group(1)) - skew, m.group(2), m.group(3), m.group(4)
            if ts < start:
                continue
            if verbose:
                keep = VERBOSE_TAGS.search(line) and not NOISE.search(msg)
            else:
                keep = tag in ("xrspd", "xrstreaming") and (
                    CONCISE.search(msg) or (level in "WE" and not NOISE.search(msg)))
            text = f"{tag}: {msg.replace('oculus_xrs_', '')}"[:170]
            if keep and text != last:
                events.append((ts, "QST ", text))
                last = text
    return events


def main():
    argv = sys.argv[1:]

    def opt(name, default):
        if name in argv:
            i = argv.index(name)
            value = argv[i + 1]
            del argv[i:i + 2]
            return value
        return default

    timeout = float(opt("--timeout", "90"))
    hello_wait = opt("--hello-wait", None)
    log_only = "--log-only" in argv
    verbose = "--verbose" in argv
    argv = [a for a in argv if not a.startswith("--")]
    seconds = argv[0] if argv else "60"

    os.makedirs(RUNS, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    host_log, head_log, timeline_path = (os.path.join(RUNS, f"{stamp}-{n}") for n in
                                         ("host.log", "headset.log", "timeline.txt"))

    pre, head = {}, {}
    jobs = [threading.Thread(target=prepare_headset, args=(head,))]
    if not log_only:
        jobs.append(threading.Thread(target=preflight, args=(pre,)))
    for j in jobs:
        j.start()
    for j in jobs:
        j.join()
    notes = []
    for k in pre.get("killed") or []:
        notes.append(f"killed stray {k[:100]}")
    if pre.get("error"):
        notes.append(f"preflight failed: {pre['error']}")
    elif not log_only and (pre.get("service") == "Running" or pre.get("servers")):
        notes.append("WARNING: OVRService is running; it owns the XRSP interface (host/ovr_service.ps1 stop)")
    if head.get("error"):
        notes.append(f"WARNING: {head['error']}; no headset log")
    for n in notes:
        print(n)
    if log_only and head.get("error"):
        return 2

    logcat = None
    if "skew" in head:
        with open(head_log, "w") as hf:
            logcat = subprocess.Popen([ADB, "-s", ADB_TARGET, "logcat", "-v", "epoch"], stdout=hf,
                                      stderr=subprocess.DEVNULL)
    t0 = time.time()
    deadline = t0 + timeout
    lines, status = [], "not run"
    try:
        if log_only:
            print(f"recording headset for {timeout:.0f}s (Ctrl+C to stop)", flush=True)
            try:
                time.sleep(timeout)
            except KeyboardInterrupt:
                pass
        else:
            env = dict(os.environ, XRSP_WAIT=str(int(timeout)))
            if hello_wait:
                env["XRSP_HELLO_WAIT"] = hello_wait
            print(f"host running (limit {timeout:.0f}s); select Link in the headset now", flush=True)
            status = run_host([seconds], env, deadline, host_log, lines)
            if logcat:
                time.sleep(0.7)  # let the last headset lines arrive
    finally:
        if logcat:
            logcat.terminate()
            logcat.wait()

    events = []
    # Host lines carry seconds since the host's own start; anchor them by the earliest arrival
    parsed = [(arr, float(m.group(1)), m.group(2)) for arr, line in lines
              if (m := re.match(r"\s*([\d.]+) (.*)", line))]
    host_t0 = min((arr - rel for arr, rel, _ in parsed), default=t0)
    repeats = {}
    for arr, line in lines:
        m = re.match(r"\s*([\d.]+) (.*)", line)
        if m:
            # Stream topics and clock pings flood the log; keep the first of each kind and count the rest
            r = REPEATED.match(m.group(2))
            if r and not verbose:
                key = f"{r.group(1)} {r.group(2)}"
                repeats[key] = repeats.get(key, 0) + 1
                if repeats[key] > 1:
                    continue
            events.append((host_t0 + float(m.group(1)), "PC  ", m.group(2)))
        elif line.strip():
            events.append((arr, "PC  ", line))
    if logcat:
        events += headset_events(head_log, head["skew"], t0 - 2, verbose)

    hello = any(HELLO.search(line) for _, line in lines)
    out = [f"{ts - t0:8.3f} {who}{text}" for ts, who, text in sorted(events, key=lambda e: e[0])]
    summary = ("headset only; " if log_only else f"HELLO {'received' if hello else 'NOT received'}; host {status}; ")
    summary += (
        f"headset skew {head['skew']:+.3f}s (rtt {head['rtt'] * 1000:.0f}ms)" if logcat else "no headset log")
    summary += f"; logs capture/runs/{stamp}-*"
    if repeats:
        out.append("counts: " + ", ".join(f"{k} x{v}" for k, v in sorted(repeats.items())))
    with open(timeline_path, "w", encoding="utf-8") as f:
        f.write("\n".join(notes + out + [summary]) + "\n")
    print("\n".join(out + [summary]))
    return 0 if hello or log_only else 1


if __name__ == "__main__":
    sys.exit(main())
