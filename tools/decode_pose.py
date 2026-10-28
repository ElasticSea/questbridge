"""Decode the Quest->PC pose stream (XRSP topics 34 and 35).

Both topics carry single-segment Cap'n Proto messages sent as two XRSP
messages: an 8-byte segment table, then the segment. Offsets below are byte
offsets inside the segment (= XRSP body of the second message).

Topic 34 (~543 Hz, 272 B segment). Root struct: 18 data words, 3 pointers.
  0x08 u64  vsync time (Quest ns, steps 13.889 ms = 72 Hz)
  0x10 ...  frame-timing block (frame indices + Quest-ns timestamps), zero
            until the video stream starts; see FRAME_FIELDS
  0x80 f32  unknown, changes ~1/s (in step with clock sync)
  0x88 i64  unknown ns offset (-0.17..-0.35 s), slews ~+47 us per update
  0x90 u64  send time (Quest ns; tracks USB capture time within ~3 ms)
  0x98 ptr  null in this capture (controllers? not seen)
  0xa0 ptr  -> pose state struct (11 data words, at 0xb8)
  0xa8 ptr  -> struct (1 word, at 0xb0): u64 = 0xf (tracking status bits?)
Pose state (offsets in segment):
  0xb8 3xf32 angular velocity, world frame, rad/s
  0xc4 3xf32 linear velocity, m/s
  0xd0 3xf32 angular acceleration, rad/s^2
  0xdc 3xf32 linear acceleration (gravity removed), m/s^2
  0xe8 4xf32 orientation quaternion x, y, z, w
  0xf8 3xf32 position, m
  0x104 u32  padding (0)
  0x108 u64  pose sample time (Quest ns, ~2.3 ms before send time)

Topic 35 (same rate, 32 B segment). Root struct: 2 data words, 1 pointer.
  0x08 u8 = 3, 0x09 u8 = 100, 0x0a u16 = 0   (constant)
  0x0c f32 = 1.1553                          (constant)
  0x10 f32 ~0.062-0.065 (IPD in m?)
  0x18 ptr  null

Usage: py decode_pose.py <capture.pcap> <device> [count] [--check]
"""
import math
import statistics
import struct
import sys

from xrsp import messages

FRAME_FIELDS = [  # (offset, fmt, name) -- topic 34 frame-timing block
    (0x10, "I", "frameA"),        # last completed frame index (gappy)
    (0x18, "Q", "frameA_t0"),
    (0x20, "Q", "frameA_t1"),
    (0x28, "I", "frameA_dur0"),   # ns-scale durations
    (0x30, "I", "frameA_dur1"),
    (0x38, "Q", "frameA_t2"),
    (0x40, "Q", "frameA_t3"),
    (0x48, "I", "stage"),         # 0, 2 or 7
    (0x50, "I", "frameC"),
    (0x58, "Q", "frameC_t"),
    (0x60, "I", "frameD"),        # increments once per vsync
    (0x68, "Q", "frameD_t"),
    (0x70, "Q", "frameA_tD"),     # = frameD_t from when frameD == frameA
]


def _struct_ptr(seg, at):
    """Resolve a Cap'n Proto struct pointer at byte offset `at`.
    Returns (data_offset, data_words, ptr_words) or None for null."""
    w = struct.unpack_from("<Q", seg, at)[0]
    if w == 0:
        return None
    if w & 3 != 0:
        raise ValueError(f"not a struct pointer at {at:#x}: {w:#018x}")
    off = (w & 0xFFFFFFFF) >> 2
    off = off - (1 << 30) if off & (1 << 29) else off
    return at + 8 + off * 8, (w >> 32) & 0xFFFF, w >> 48


def decode_34(seg):
    rdata, dw, pw = _struct_ptr(seg, 0)
    rptr = rdata + dw * 8
    d = {
        "vsync_t": struct.unpack_from("<Q", seg, rdata + 0x00)[0],
        "unk_f32": struct.unpack_from("<f", seg, rdata + 0x78)[0],
        "unk_ofs": struct.unpack_from("<q", seg, rdata + 0x80)[0],
        "send_t": struct.unpack_from("<Q", seg, rdata + 0x88)[0],
    }
    for off, fmt, name in FRAME_FIELDS:
        d[name] = struct.unpack_from("<" + fmt, seg, rdata + off - 0x08)[0]
    d["ptr0_null"] = _struct_ptr(seg, rptr) is None
    p = _struct_ptr(seg, rptr + 8)
    o = p[0]
    d["ang_vel"] = struct.unpack_from("<3f", seg, o + 0x00)
    d["lin_vel"] = struct.unpack_from("<3f", seg, o + 0x0C)
    d["ang_acc"] = struct.unpack_from("<3f", seg, o + 0x18)
    d["lin_acc"] = struct.unpack_from("<3f", seg, o + 0x24)
    d["quat"] = struct.unpack_from("<4f", seg, o + 0x30)
    d["pos"] = struct.unpack_from("<3f", seg, o + 0x40)
    d["pose_t"] = struct.unpack_from("<Q", seg, o + 0x50)[0]
    s = _struct_ptr(seg, rptr + 16)
    d["status"] = struct.unpack_from("<Q", seg, s[0])[0] if s else None
    return d


def decode_35(seg):
    rdata, dw, pw = _struct_ptr(seg, 0)
    a, b, c, f0, f1 = struct.unpack_from("<BBHff", seg, rdata)
    return {"u8_0": a, "u8_1": b, "u16": c, "f32_0": f0, "f32_1": f1}


def segments(path, device, topics=(34, 35)):
    """Yield (ts, topic, seg) for Quest->PC Cap'n Proto segments, joining the
    separately-sent 8-byte segment table with the following segment."""
    pending = {}
    for m in messages(path, device):
        if m.to_quest or m.topic not in topics:
            continue
        b = m.body
        if len(b) == 8 and struct.unpack_from("<I", b)[0] == 0:
            pending[m.topic] = struct.unpack_from("<I", b, 4)[0] * 8
            continue
        size = pending.pop(m.topic, None)
        if size is None and len(b) >= 8 and struct.unpack_from("<I", b)[0] == 0:
            size = struct.unpack_from("<I", b, 4)[0] * 8  # inline table
            b = b[8:]
        if size is not None and len(b) >= size:
            yield m.ts, m.topic, b[:size]


def _v(v, p=4):
    return "(" + ", ".join(f"{x:+.{p}f}" for x in v) + ")"


def show(path, device, count):
    n = {34: 0, 35: 0}
    for ts, topic, seg in segments(path, device):
        if n[topic] >= count:
            if all(v >= count for v in n.values()):
                break
            continue
        n[topic] += 1
        if topic == 34:
            d = decode_34(seg)
            print(f"t={ts:.6f} T34 pose_t={d['pose_t']:#x} send_t={d['send_t']:#x} "
                  f"vsync_t={d['vsync_t']:#x} status={d['status']:#x}")
            print(f"    quat={_v(d['quat'], 5)} |q|={math.sqrt(sum(x * x for x in d['quat'])):.6f} "
                  f"pos={_v(d['pos'])}")
            print(f"    angvel={_v(d['ang_vel'])} linvel={_v(d['lin_vel'])} "
                  f"angacc={_v(d['ang_acc'], 3)} linacc={_v(d['lin_acc'], 3)}")
            print(f"    frameA={d['frameA']} frameC={d['frameC']} frameD={d['frameD']} "
                  f"stage={d['stage']} unk_f32={d['unk_f32']:.4g} unk_ofs={d['unk_ofs'] / 1e6:.3f}ms")
        else:
            d = decode_35(seg)
            print(f"t={ts:.6f} T35 {d['u8_0']} {d['u8_1']} {d['u16']} "
                  f"f32_0={d['f32_0']:.5f} f32_1={d['f32_1']:.5f}")


def _corr(xs, ys):
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy), sxy / sxx


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def check(path, device, k=8):
    """Statistical checks of the topic-34 layout over the whole capture."""
    P = [(ts, decode_34(seg)) for ts, topic, seg in segments(path, device, (34,))]
    D = [d for _, d in P]
    print(f"topic 34 segments: {len(D)} over {P[-1][0] - P[0][0]:.2f} s")
    norms = [math.sqrt(sum(x * x for x in d["quat"])) for d in D]
    print(f"|quat|: min {min(norms):.7f} max {max(norms):.7f}")
    for f in ("pose_t", "send_t", "vsync_t"):
        st = [b[f] - a[f] for a, b in zip(D, D[1:])]
        nz = [s for s in st if s]
        print(f"{f}: monotonic={min(st) >= 0} repeats={len(st) - len(nz)} "
              f"median step {statistics.median(nz) / 1e6:.3f} ms")
    lag = [(d["send_t"] - d["pose_t"]) / 1e6 for d in D]
    print(f"send_t - pose_t: {min(lag):.3f}..{max(lag):.3f} ms")
    jit = [d["send_t"] / 1e9 - ts for ts, d in P]
    print(f"send_t vs capture clock: spread {(max(jit) - min(jit)) * 1e3:.2f} ms")
    T = [d["pose_t"] / 1e9 for d in D]

    def deriv(key):
        out = []
        for i in range(k, len(D) - k):
            dt = T[i + k] - T[i - k]
            out.append(None if dt <= 0 else
                       tuple((D[i + k][key][j] - D[i - k][key][j]) / dt for j in range(3)))
        return out

    def report(name, est, key):
        pairs = [(e, D[i + k][key]) for i, e in enumerate(est) if e is not None]
        r = [_corr([p[0][j] for p in pairs], [p[1][j] for p in pairs]) for j in range(3)]
        print(f"{name:28s} r=" + " ".join(f"{c:.3f}" for c, _ in r)
              + "  slope=" + " ".join(f"{s:.3f}" for _, s in r))

    report("d(pos)/dt ~ lin_vel", deriv("pos"), "lin_vel")
    report("d(lin_vel)/dt ~ lin_acc", deriv("lin_vel"), "lin_acc")
    report("d(ang_vel)/dt ~ ang_acc", deriv("ang_vel"), "ang_acc")
    w = []
    for i in range(k, len(D) - k):
        dt = T[i + k] - T[i - k]
        a, b, q = D[i - k]["quat"], D[i + k]["quat"], D[i]["quat"]
        if dt <= 0:
            w.append(None)
            continue
        if sum(x * y for x, y in zip(a, b)) < 0:
            b = tuple(-x for x in b)
        dq = tuple((y - x) / dt for x, y in zip(a, b))
        w.append(tuple(2 * x for x in _qmul(dq, (-q[0], -q[1], -q[2], q[3]))[:3]))
    report("2*dq/dt*q^-1 ~ ang_vel", w, "ang_vel")
    print(f"status values: {sorted({d['status'] for d in D})}, ptr0 always null: "
          f"{all(d['ptr0_null'] for d in D)}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path, dev = args[0], int(args[1])
    if "--check" in sys.argv:
        check(path, dev)
    else:
        show(path, dev, int(args[2]) if len(args) > 2 else 10)
