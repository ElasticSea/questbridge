"""Slice cap stress test: high-entropy clips through qlenc, every slice must fit one XRSP message.

Runs qlenc (the platform encoder: Media Foundation on Windows, VideoToolbox on macOS) on the
noise / motion / cuts patterns, all intra and with P frames, and fails if any slice NAL is over
the cap (qlenc exits 4) or the encoder gives up. Prints the largest slice, how many frames needed
the re-encode fallback, the QP it cost and the slowest encode() call.

Usage: py tools/slice_cap_stress.py [--frames 72] [--qlenc PATH] [--cap 100000] [-- extra qlenc args]
       e.g. -- --codec hevc   or   -- --size 2880x1600
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = [(p, g) for p in ("cuts", "motion", "noise") for g in (1, 72)]


def find_qlenc():
    for rel in ("native/build/bin/Release/qlenc.exe", "native/build/bin/qlenc", "native/encoders/build/qlenc.exe"):
        path = os.path.join(ROOT, rel)
        if os.path.exists(path):
            return os.path.normpath(path)
    sys.exit("qlenc not found; build native/ first")


def run_case(qlenc, pattern, gop, frames, cap, extra):
    name = f"slicecap-{pattern}-gop{gop}"
    cmd = [qlenc, "--frames", str(frames), "--name", name, "--pattern", pattern, "--gop", str(gop),
           "--max-slice-bytes", str(cap), "--root", ROOT] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = p.stdout + p.stderr
    r = {"case": f"{pattern} gop {gop}", "code": p.returncode, "out": out}
    m = re.search(r"largest slice NAL: P (\d+) B, IDR (\d+) B; over \d+ B: (\d+)", out)
    if m:
        r["largest"] = max(int(m[1]), int(m[2]))
        r["over"] = int(m[3])
    m = re.search(r"slice cap: (\d+) frames re-encoded \((\d+) encodes\), largest rejected slice (\d+) B", out)
    if m:
        r["refit_frames"], r["refits"], r["rejected"] = int(m[1]), int(m[2]), int(m[3])
    m = re.search(r"slice QP: normal frames mean ([\d.]+) max (-?\d+); re-encoded frames mean ([\d.]+) max (-?\d+)", out)
    if m:
        r["qp"], r["refit_qp"] = float(m[1]), float(m[3])
    m = re.search(r"encode\(\) call incl\. copy/convert ms/frame: mean ([\d.]+), p50 [\d.]+, p95 [\d.]+, max ([\d.]+)", out)
    if m:
        r["ms_mean"], r["ms_max"] = float(m[1]), float(m[2])
    r["ok"] = p.returncode in (0, 3) and r.get("over") == 0 and r.get("largest", cap + 1) <= cap
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=72)
    ap.add_argument("--qlenc")
    ap.add_argument("--cap", type=int, default=100_000)
    ap.add_argument("extra", nargs="*", help="extra qlenc arguments (after --)")
    a = ap.parse_args()
    qlenc = a.qlenc or find_qlenc()
    print(f"qlenc {qlenc} {' '.join(a.extra)}, {a.frames} frames, cap {a.cap} B")
    print(f"{'case':<14}{'largest':>9}{'rejected':>10}{'refit fr':>9}{'encodes':>8}{'QP':>6}{'refit QP':>9}"
          f"{'ms mean':>8}{'ms max':>8}  result")
    failed = 0
    for pattern, gop in CASES:
        r = run_case(qlenc, pattern, gop, a.frames, a.cap, a.extra)
        qp = f"{r['qp']:.1f}" if "qp" in r else "-"
        rqp = f"{r['refit_qp']:.1f}" if r.get("refit_frames") and "refit_qp" in r else "-"
        print(f"{r['case']:<14}{r.get('largest', '-'):>9}{r.get('rejected', '-'):>10}{r.get('refit_frames', '-'):>9}"
              f"{r.get('refits', '-'):>8}{qp:>6}{rqp:>9}{r.get('ms_mean', 0):>8.1f}{r.get('ms_max', 0):>8.1f}"
              f"  {'OK' if r['ok'] else 'FAIL'}")
        if not r["ok"]:
            failed += 1
            tail = [l for l in r["out"].splitlines() if l.strip()][-5:]
            print("    exit %d: %s" % (r["code"], " | ".join(tail)))
    print("FAILED: %d of %d cases" % (failed, len(CASES)) if failed else f"all {len(CASES)} cases under the cap")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
