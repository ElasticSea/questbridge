"""Find and decode controller, haptics and passthrough traffic in a Link capture.

Built for the SensorProbe guided run: its log has `STEP n name begin/end` lines
with UTC ms timestamps, and the pcap records carry UTC epoch times, so each
step becomes a time window in the capture.

Modes:
  steps <pcap> <dev> <probe.log>       per step: message counts per topic/dir and
                                       which Cap'n Proto bytes change vs the
                                       "still" step (topics in DIFF_TOPICS)
  timeline <pcap> <dev> <probe.log>    every change of the topic-35 buttons/touch
                                       fields, plus analog ranges per step
  golden <pcap> <dev> <out.json>       labelled messages at known events (fixture)
  ripc <pcap> <dev> <out.json>         Meta's HapticsServer RuntimeIPC frames (fixture)
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


def _data(node):
    return struct.pack(f"<{len(node['s'])}Q", *node["s"])


def decode_input(buf):
    """Topic 35 DeviceInput -> (root fields, [controller entries]). Layout in docs/protocol/spec.md."""
    root, _ = decode(buf)
    d = _data(root)
    out = {"flags": d[0], "u8_1": d[1], "u16_2": struct.unpack_from("<H", d, 2)[0],
           "f32_4": struct.unpack_from("<f", d, 4)[0], "ipd": struct.unpack_from("<f", d, 8)[0]}
    entries = []
    for e in (root["p"][0] or {}).get("lc", []):
        b = _data(e)
        entries.append({
            "index": b[0], "battery": b[1], "u8_2": b[2], "u8_3": b[3],
            "buttons": struct.unpack_from("<I", b, 0x04)[0],
            "touches": struct.unpack_from("<I", b, 0x08)[0],
            "trigger": struct.unpack_from("<f", b, 0x0C)[0],
            "grip": struct.unpack_from("<f", b, 0x10)[0],
            "stick": struct.unpack_from("<2f", b, 0x14),
            "f32_1c": struct.unpack_from("<4f", b, 0x1C),
            "f32_2c": struct.unpack_from("<3f", b, 0x2C),
            "u8_38": b[0x38], "raw": b,
        })
    return out, entries


def decode_tracking(buf):
    """Topic 34 Pose2 controller list (root pointer 0) -> [(index, pose dict, status)]."""
    root, _ = decode(buf)
    out = []
    for e in (root["p"][0] or {}).get("lp", []) or (root["p"][0] or {}).get("lc", []):
        p = _data(e["p"][0]) if e["p"][0] else None
        pose = None if p is None else {
            "ang_vel": struct.unpack_from("<3f", p, 0x00), "lin_vel": struct.unpack_from("<3f", p, 0x0C),
            "ang_acc": struct.unpack_from("<3f", p, 0x18), "lin_acc": struct.unpack_from("<3f", p, 0x24),
            "quat": struct.unpack_from("<4f", p, 0x30), "pos": struct.unpack_from("<3f", p, 0x40),
            "time": struct.unpack_from("<Q", p, 0x50)[0]}
        status = e["p"][1]["s"][0] if e["p"][1] else None
        out.append((e["s"][0] & 0xFFFFFFFF, pose, status))
    return out


def timeline(path, device, log_path):
    """Print every change of the topic-35 digital fields, labelled with the probe step."""
    t0 = pcap_start(path)
    windows = [(n, b - t0, e - t0) for n, b, e in probe_steps(log_path)]
    last = {}
    analog = defaultdict(lambda: defaultdict(lambda: [9.0, -9.0]))
    for ts, to_q, topic, buf in capnp_messages(path, device, {35}):
        if to_q or buf is None:
            continue
        step = next((n for n, b, e in windows if b <= ts < e), "-")
        root, entries = decode_input(buf)
        key = ("root", root["flags"], root["u8_1"], root["u16_2"])
        if last.get("root") != key:
            print(f"{ts:9.3f} {step:16s} root flags={root['flags']:#x} u8_1={root['u8_1']} u16_2={root['u16_2']}"
                  f" f32_4={root['f32_4']:.4f}")
            last["root"] = key
        for e in entries:
            k = (e["index"], e["buttons"], e["touches"], e["u8_38"], e["battery"], e["u8_2"])
            if last.get(e["index"]) != k:
                print(f"{ts:9.3f} {step:16s} [{e['index']}] buttons={e['buttons']:#010x} touches={e['touches']:#06x}"
                      f" b38={e['u8_38']} batt={e['battery']} u8_2={e['u8_2']}")
                last[e["index"]] = k
            vals = [e["trigger"], e["grip"], *e["stick"], *e["f32_1c"], *e["f32_2c"]]
            for j, v in enumerate(vals):
                a = analog[(step, e["index"])][j]
                a[0], a[1] = min(a[0], v), max(a[1], v)
    names = ["trig", "grip", "sx", "sy", "1c", "20", "24", "28", "2c", "30", "34"]
    print("\nanalog ranges per step [index]:")
    for (step, idx), a in analog.items():
        print(f"  {step:16s} [{idx}] " + " ".join(f"{n}={a[j][0]:+.2f}..{a[j][1]:+.2f}"
                                               for j, n in enumerate(names) if a[j][1] - a[j][0] > 0.02))


HAPTIC_HASH = bytes.fromhex("6967dc2a")  # RIPC type hash of the haptic message on topic 31
BUTTON_EVENTS = {  # label -> (controller index, buttons value seen first)
    "a": (1, 0x1), "b": (1, 0x2), "r_stick_click": (1, 0x80000004),
    "x": (0, 0x100), "y": (0, 0x200), "l_stick_click": (0, 0x80000400), "menu": (0, 0x40000000),
}


def golden(path, device, out_path):
    """Write labelled topic 34/35/28/7/31 messages at known events for golden tests."""
    import json
    from scrub import Scrubber
    scrub = Scrubber.from_capture(path, device)
    picked = {}

    def keep(label, ts, topic, to_q, buf):
        if label not in picked:
            picked[label] = {"label": label, "t": round(ts, 6), "topic": topic,
                             "dir": "pc_to_quest" if to_q else "quest_to_pc", "capnp": scrub(buf).hex()}

    best = {}
    ripc = []
    for ts, to_q, topic, buf in capnp_messages(path, device, {7, 28, 34, 35}):
        if buf is None:
            continue
        if topic in (7, 28) and to_q:
            keep(f"topic{topic}_first", ts, topic, to_q, buf)
        elif topic == 34 and not to_q:
            st = [s for _, _, s in decode_tracking(buf)]
            if st == [15, 15]:
                keep("pose_both_tracked", ts, topic, to_q, buf)
            elif st and all(s == 0 for s in st):
                keep("pose_controllers_lost", ts, topic, to_q, buf)
        elif topic == 35 and not to_q:
            root, entries = decode_input(buf)
            if root["flags"] & 0x4 and root["u16_2"] == 0:
                keep("hands_mode", ts, topic, to_q, buf)
            for label, (idx, val) in BUTTON_EVENTS.items():
                if any(e["index"] == idx and e["buttons"] == val for e in entries):
                    keep(label, ts, topic, to_q, buf)
            for e in entries:
                side = "l" if e["index"] == 0 else "r"
                for name, v in (("trigger", e["trigger"]), ("grip", e["grip"]),
                                ("stick_x_max", e["stick"][0]), ("stick_x_min", -e["stick"][0]),
                                ("stick_y_max", e["stick"][1]), ("stick_y_min", -e["stick"][1])):
                    label = f"{side}_{name}"
                    if v > best.get(label, (0.5,))[0]:
                        best[label] = (v, ts, buf)
    for label, (v, ts, buf) in best.items():
        keep(label, ts, 35, False, buf)
    # Haptics ride RuntimeIPC (topic 31, raw frames, not Cap'n Proto): keep the first 0x23e group
    for m in messages(path, device):
        if m.topic == 31 and m.to_quest:
            b = m.body[:-m.body[-1]] if m.flags & 0x08 else m.body
            ripc.append(b)
            if len(ripc) >= 4 and len(ripc[-2]) == 32 and ripc[-2][22:26] == HAPTIC_HASH:
                picked["haptic_ripc"] = {"label": "haptic_ripc", "t": round(m.ts, 6), "topic": 31,
                                         "dir": "pc_to_quest", "parts": [scrub(x).hex() for x in ripc[-4:]]}
                break
            ripc = ripc[-3:]
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(sorted(picked.values(), key=lambda p: p["t"]), f, indent=1)
    print(f"{len(picked)} messages -> {out_path}: {sorted(picked)}")


RIPC_HEADER = struct.Struct("<IBIQBII")  # kind, u8, payload size, client id, u8, seq, tag; padded to 32 B
RIPC_LOOKUP, RIPC_CONNECT, RIPC_CALL = 0x43E, 0x13E, 0x23E


def ripc_header(b):
    """Packed 32-byte RuntimeIPC header (PC->Q) -> dict."""
    kind, _, size, client, _, seq, tag = RIPC_HEADER.unpack_from(b)
    return {"kind": kind, "size": size, "client": client, "seq": seq, "tag": tag}


def ripc_frame(kind, client, seq, tag, payload):
    """The 4 topic-31 bodies of one PC->Q RuntimeIPC frame (before XRSP padding)."""
    header = RIPC_HEADER.pack(kind, 0, len(payload), client, 0, seq, tag).ljust(32, b"\0")
    return [struct.pack("<II", 0, 2), struct.pack("<4I", 0, 1, len(header), len(payload)), header, payload]


def ripc_frames(path, device):
    """Yield (ts, [raw XRSP messages]) for every PC->Q RuntimeIPC frame (table, sizes, header, payload)."""
    cur = None
    for m in messages(path, device):
        if m.topic != 31 or not m.to_quest:
            continue
        b = m.body[:-m.body[-1]] if m.flags & 0x08 else m.body
        if b == struct.pack("<II", 0, 2):
            cur = [m.raw]
        elif cur is not None:
            cur.append(m.raw)
            if len(cur) == 4:
                yield m.ts, cur
                cur = None


def ripc_golden(path, device, out_path):
    """Write Meta's HapticsServer frames (lookup, connect, haptic calls) for byte-exact tests."""
    import json
    from scrub import Scrubber
    scrub = Scrubber.from_capture(path, device)
    frames = []
    for ts, raws in ripc_frames(path, device):
        body = raws[2][8:]
        if len(body) != 32:  # transport handshake frames have other parts
            continue
        frames.append((ts, ripc_header(body), raws))
        if body[22:26] == HAPTIC_HASH and sum(f[1]["tag"] == int.from_bytes(HAPTIC_HASH, "little")
                                              for f in frames) == 2:
            break
    haptic_client = frames[-1][1]["client"]
    connect = next(i for i, f in enumerate(frames) if f[1]["kind"] == RIPC_CONNECT and f[1]["client"] == haptic_client)
    lookup = max(i for i in range(connect) if frames[i][1]["kind"] == RIPC_LOOKUP)
    picked = [("lookup", frames[lookup]), ("connect_haptics", frames[connect])]
    picked += [(f"haptic_{n}", f) for n, f in enumerate(f for f in frames if f[1]["tag"] == int.from_bytes(HAPTIC_HASH, "little"))]
    out = [{"label": label, "t": round(ts, 6), "header": h, "raw": [scrub(r).hex() for r in raws]}
           for label, (ts, h, raws) in picked]
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print(f"{len(out)} frames -> {out_path}: {[p['label'] for p in out]}")


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
    elif mode == "timeline":
        timeline(args[1], int(args[2]), args[3])
    elif mode == "golden":
        golden(args[1], int(args[2]), args[3])
    elif mode == "ripc":
        ripc_golden(args[1], int(args[2]), args[3])
    elif mode == "dump":
        d = True if "--pc" in flags else False if "--quest" in flags else None
        dump(args[1], int(args[2]), int(args[3]), int(args[4]) if len(args) > 4 else 5,
             float(args[5]) if len(args) > 5 else 0.0, d)
    else:
        sys.exit(__doc__)
