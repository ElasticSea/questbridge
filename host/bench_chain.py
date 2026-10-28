"""Stream several replay clips back to back in one Link session, with the headset log alongside.

Each stage runs host/handshake.py once. The first stage does the handshake (or attaches with
--resume, a "resume with ..." line from an earlier host); every later stage attaches with the
resume line the stage before printed. adb logcat -s xrstreaming runs for the whole chain.

Usage: py host/bench_chain.py NAME [--codec N] [--resume "XRSP_FRAME_START=... ..."]
                              clip.json:seconds[:hold] [clip.json:seconds[:hold] ...]
  --codec N   first-stage CONFIG codec (XRSP_CODEC; 0 H.264, 1 HEVC inferred)
  hold        XRSP_HOLD for 1 frame/s clips (72)
Logs: capture/runs/<NAME>/stage<i>.log, logcat.txt, summary.txt (fps, Glitches, decoder errors).
Does not stop OVRService, replug or kill other hosts: the caller owns the USB handover.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADB = os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "WinGet", "Packages",
                   "Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe", "platform-tools", "adb.exe")
ADB_DEVICE = os.environ.get("QL_ADB_DEVICE", "192.0.2.10:5555")
SUMMARY = re.compile(r"Glitch|fps|decod|VideoReceiver|codec|mime|deadline|strip|error|fail", re.I)


def main():
    args = sys.argv[1:]
    name, stages, codec, resume = args.pop(0), [], None, None
    while args:
        a = args.pop(0)
        if a == "--codec":
            codec = args.pop(0)
        elif a == "--resume":
            resume = args.pop(0)
        else:
            stages.append(a)
    out = os.path.join(ROOT, "capture", "runs", name)
    os.makedirs(out, exist_ok=True)
    adb = [ADB, "-s", ADB_DEVICE]
    subprocess.run(adb + ["shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close"],
                   capture_output=True)
    logcat_file = open(os.path.join(out, "logcat.txt"), "w", encoding="utf-8", errors="replace")
    logcat = subprocess.Popen(adb + ["logcat", "-v", "time", "-T", "1", "-s", "xrstreaming"], stdout=logcat_file,
                              stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        for i, stage in enumerate(stages):
            clip, seconds, *hold = stage.split(":")
            env = dict(os.environ, XRSP_REPLAY=clip, XRSP_HOLD=hold[0] if hold else "1")
            env.pop("XRSP_CODEC", None)
            if resume:
                env["XRSP_ATTACH"] = "1"
                env.update(kv.split("=", 1) for kv in resume.split())
            elif codec is not None:
                env["XRSP_CODEC"] = codec
            print(f"stage {i}: {clip} {seconds}s {'attach' if resume else 'handshake'}", flush=True)
            with open(os.path.join(out, f"stage{i}.log"), "w", encoding="utf-8") as log:
                code = subprocess.run([sys.executable, os.path.join(ROOT, "host", "handshake.py"), seconds],
                                      cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
            text = open(os.path.join(out, f"stage{i}.log"), encoding="utf-8").read()
            m = re.search(r"resume with (.*)", text)
            print(f"  exit {code}; " + (m.group(1) if m else "no resume line"), flush=True)
            if code or not m:
                break
            resume = m.group(1).strip()
    finally:
        time.sleep(2)
        logcat.terminate()
        logcat_file.close()
    with open(os.path.join(out, "logcat.txt"), encoding="utf-8", errors="replace") as f:
        lines = [line.rstrip() for line in f if SUMMARY.search(line)]
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} summary lines -> {os.path.relpath(os.path.join(out, 'summary.txt'), ROOT)}")


if __name__ == "__main__":
    main()
