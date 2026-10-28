# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Recovery live check, PC side: follows the FirstLight player's log during "-sequence-recovery"
and disturbs the headset over adb when the board announces each drop, then summarises.

Usage: py tools/recovery_check.py --log PLAYER_LOG [--qblog QB_LOG_FILE] [--timeout 600] [--dry-run]

The player shows every step in the headset (RecoveryCheck.cs); this script only reacts to its
"[Recovery] drop N <kind>" lines:
  sleep     adb KEYCODE_SLEEP, 6 s asleep, KEYCODE_WAKEUP + prox_close (the stream may survive)
  linkquit  adb force-stop com.oculus.xrstreamingclient (Link quit: the headset drops its USB
            function; the runtime re-handshakes and the user presses Launch)
Exit code 0 when every drop came back (or was kept) and the user answered YES.
Procedure and pass criteria: docs/testing.md
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ADB = os.environ.get("ADB") or shutil.which("adb") or "adb"
SERIAL = os.environ.get("QUEST_ADB", "")  # adb serial; empty = the only device

DROP = re.compile(r"\[Recovery\] drop (\d+) (\w+)")
BACK = re.compile(r"\[Recovery\] back (\d+) (\w+): (.*)")
DONE = re.compile(r"\[Recovery\] done: (.*)")

# adb shell commands per drop kind; ("wait", s) pauses between them
ACTIONS = {
    "sleep": [("shell", "input keyevent KEYCODE_SLEEP"), ("wait", 6),
              ("shell", "input keyevent KEYCODE_WAKEUP; am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null")],
    "linkquit": [("shell", "am force-stop com.oculus.xrstreamingclient")],
}


def actions_for(line):
    """The adb steps a player log line asks for (empty for any other line)"""
    m = DROP.search(line)
    return ACTIONS.get(m.group(2), []) if m else []


def verdict(done_line):
    """Pass when every drop is back_* or kept and the answer is YES"""
    results = dict(r.split("=", 1) for r in done_line.split(", ") if "=" in r)
    drops_ok = all(v == "kept" or v.startswith("back_") for k, v in results.items() if k != "came_back")
    return drops_ok and results.get("came_back") == "YES", results


def follow(path, timeout):
    """Yield new lines of a growing file until timeout"""
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > end:
            return
        time.sleep(0.2)
    with open(path, encoding="utf-8", errors="replace") as f:
        while time.monotonic() < end:
            line = f.readline()
            if line:
                yield line.rstrip()
            else:
                time.sleep(0.1)


def run(step, serial, dry_run, out):
    if step[0] == "wait":
        out(f"  wait {step[1]} s")
        if not dry_run:
            time.sleep(step[1])
        return
    out(f"  adb {' '.join(step)}")
    if not dry_run:
        subprocess.run([ADB, *(["-s", serial] if serial else []), *step], capture_output=True, timeout=20)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True, help="the player's -logFile")
    ap.add_argument("--qblog", help="QB_LOG_FILE of the player: its link transitions are summarised")
    ap.add_argument("--serial", default=SERIAL)
    ap.add_argument("--timeout", type=float, default=600, help="seconds; the user paces the steps")
    ap.add_argument("--dry-run", action="store_true", help="print the adb steps, don't run them")
    a = ap.parse_args()

    def out(s):
        print(f"{time.strftime('%H:%M:%S')} {s}", flush=True)

    done = None
    for line in follow(a.log, a.timeout):
        if "[Recovery]" not in line:
            continue
        out(line[line.index("[Recovery]"):])
        for step in actions_for(line):
            run(step, a.serial, a.dry_run, out)
        m = DONE.search(line)
        if m:
            done = m.group(1)
            break
    if a.qblog and os.path.exists(a.qblog):
        with open(a.qblog, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "link:" in line or "resum" in line or "restart" in line or ("BYE" in line and "headset" in line):
                    out("  native " + line.strip())
    if done is None:
        out("FAIL: no '[Recovery] done' line before the timeout")
        return 1
    ok, results = verdict(done)
    out(("PASS" if ok else "FAIL") + f": {results}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
