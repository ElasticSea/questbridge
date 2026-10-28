"""Find and decode controller, haptics and passthrough traffic in a Link capture.

Built for the SensorProbe guided run: its log has `STEP n name begin/end` lines
with UTC ms timestamps, and the pcap records carry UTC epoch times, so each
step becomes a time window in the capture.

Modes:
  steps <pcap> <dev> <probe.log>       per step: message counts per topic/dir and
                                       which Cap'n Proto bytes change vs the
                                       "still" step (topics in DIFF_TOPICS)
  dump <pcap> <dev> <topic> [n] [from-s] [--pc|--quest]
                                       pretty-print n Cap'n Proto messages

Usage: py decode_controllers.py <mode> ...
"""
import struct
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone

from capnp_codec import decode
from xrsp import messages

DIFF_TOPICS = (2, 7, 28, 34, 35)


def pcap_start(path):
    """UTC epoch seconds of the first record (xrsp/usbpcap times are relative to it)."""
    with open(path, "rb") as f:
        f.read(24)
        sec, usec = struct.unpack("<II", f.read(8))
    return sec + usec / 1e6


def probe_steps(log_path):
    """[(name, begin_utc, end_utc)] from a SensorProbe log."""
    begins, out = {}, []
    for line in open(log_path, encoding="utf-8"):
        parts = line.split()
        if len(parts) < 6 or parts[3] != "STEP":
            continue
        t = datetime.strptime(parts[0], "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc).timestamp()
        name, what = parts[5], parts[6]
        if what == "begin":
            begins[name] = t
        elif name in begins:
            out.append((name, begins.pop(name), t))
    return out


def capnp_messages(path, device, topics):
    """Yield (ts, to_quest, topic, buf) with buf a whole Cap'n Proto message
    (segment table + segment). Joins the separately-sent 8-byte table."""
    pending = {}
    for m in messages(path, device):
        if m.topic not in topics:
            continue
        key = (m.topic, m.to_quest)
        b = m.body
        if m.flags & 0x08 and b:
            b = b[:-b[-1]]  # padded body: last byte = pad count
        if len(b) == 8 and struct.unpack_from("<I", b)[0] == 0:
            pending[key] = b
            continue
        table = pending.pop(key, None)
        if table is not None:
            b = table + b
        if len(b) >= 8 and struct.unpack_from("<I", b)[0] == 0:
            size = 8 + struct.unpack_from("<I", b, 4)[0] * 8
            if len(b) >= size:
                yield m.ts, m.to_quest, m.topic, b[:size]
                continue
        yield m.ts, m.to_quest, m.topic, None  # not Cap'n Proto (or truncated)


def leaves(node, path="r", out=None):
    """Flatten a decoded tree into {path: bytes} for diffing."""
    if out is None:
        out = {}
    if node is None:
        out[path] = b"<null>"
    elif "s" in node:
        out[path + ".d"] = struct.pack(f"<{len(node['s'])}Q", *node["s"])
        for i, c in enumerate(node["p"]):
            leaves(c, f"{path}.p{i}", out)
    elif "l" in node:
        out[path + ".l"] = bytes.fromhex(node["b"])
    elif "lp" in node:
        for i, c in enumerate(node["lp"]):
            leaves(c, f"{path}[{i}]", out)
    elif "lc" in node:
        out[path + ".n"] = struct.pack("<I", len(node["lc"]))
        for i, c in enumerate(node["lc"]):
            leaves(c, f"{path}[{i}]", out)
    return out


def changed_bytes(samples):
    """{(path, byte): set(values)} for byte positions that vary within samples."""
    vals = defaultdict(set)
    for lv in samples:
        for p, b in lv.items():
            for i, x in enumerate(b):
                vals[(p, i)].add(x)
    return vals


def steps(path, device, log_path):
    t0 = pcap_start(path)
    windows = [(n, b - t0, e - t0) for n, b, e in probe_steps(log_path)]
    if not windows:
        sys.exit("no STEP lines in log")
    print(f"pcap start {t0:.3f}; {len(windows)} steps, "
          f"{windows[0][1]:.1f}..{windows[-1][2]:.1f} s into capture")
    counts = defaultdict(Counter)
    samples = defaultdict(lambda: defaultdict(list))  # step -> (topic, dir) -> [leaves]
    for ts, to_q, topic, buf in capnp_messages(path, device, set(range(1, 40))):
        step = next((n for n, b, e in windows if b <= ts < e), None)
        if step is None:
            continue
        counts[step][(topic, "pc>q" if to_q else "q>pc")] += 1
        if topic in DIFF_TOPICS and buf is not None:
            try:
                tree, _ = decode(buf)
            except ValueError:
                continue
            samples[step][(topic, to_q)].append(leaves(tree))
    base = {k: changed_bytes(v) for k, v in samples.get("still", {}).items()}
    for name, b, e in windows:
        c = counts[name]
        print(f"\n== {name} ({b:.1f}-{e:.1f} s) " +
              " ".join(f"{t}{d[0]}:{n}" for (t, d), n in sorted(c.items())))
        for key, lv in sorted(samples[name].items()):
            ref = base.get(key, {})
            vals = changed_bytes(lv)
            hits = [(p, i, v) for (p, i), v in vals.items()
                    if len(v) > 1 and len(ref.get((p, i), ())) <= 1]
            if hits:
                print(f"  topic {key[0]} {'pc>q' if key[1] else 'q>pc'}: {len(hits)} bytes newly vary")
                for p, i, v in sorted(hits)[:24]:
                    print(f"    {p}+{i:#x}: {len(v)} values {sorted(v)[:6]}")


def dump(path, device, topic, count, start, want_dir):
    from capnp_dump import pretty
    n = 0
    for ts, to_q, t, buf in capnp_messages(path, device, {topic}):
        if ts < start or (want_dir is not None and to_q != want_dir):
            continue
        print(f"t={ts:.6f} {'pc>q' if to_q else 'q>pc'} topic {t} {len(buf) if buf else 0} B")
        if buf is not None:
            print(pretty(buf)[0])
        n += 1
        if n >= count:
            break


if __name__ == "__main__":
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mode = args[0]
    if mode == "steps":
        steps(args[1], int(args[2]), args[3])
    elif mode == "dump":
        d = True if "--pc" in flags else False if "--quest" in flags else None
        dump(args[1], int(args[2]), int(args[3]), int(args[4]) if len(args) > 4 else 5,
             float(args[5]) if len(args) > 5 else 0.0, d)
    else:
        sys.exit(__doc__)
