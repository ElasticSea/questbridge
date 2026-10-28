"""Decode the PC->Quest video stream (XRSP topics 10-14) from a USB capture.

Layout (see PROTOCOL_NOTES.md):
  topic 10+i carries slice i of every frame. Per slice, in sequence order:
    1. 8-byte Cap'n Proto segment table (flags 0x10)
    2. segment: video.capnp:EncodedSlice (flags 0x10)
    3. csd message, only when csdLength > 0 (Annex-B SPS + PPS)
    4. video message: Annex-B H.264 slice NAL(s), videoLength bytes
  Flag bit 0x08 = body padded: last body byte is the pad count (fill 0xDE).

Usage: py decode_video.py <capture.pcap> <device> [--frame N] [--out PATH] [--limit N]
Prints a per-slice summary and writes one reassembled frame (csd + slices 0..4)
as an H.264 elementary stream (default: first complete intra frame).
"""
import argparse
import os
import struct
from dataclasses import dataclass, field

from xrsp import messages

VIDEO_TOPICS = range(10, 15)
NAL_NAMES = {1: "slice", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}


def unpad(m):
    return m.body[:len(m.body) - m.body[-1]] if m.flags & 0x08 and m.body else m.body


@dataclass
class Slice:
    topic: int
    ts: float
    hdr: dict
    csd: bytes = b""
    video: bytes = b""
    complete: bool = False
    payload_msgs: int = 0
    flags: list = field(default_factory=list)


def parse_header(seg):
    """EncodedSlice root struct (18 data words, 2 ptrs) from a single segment."""
    w = struct.unpack_from("<Q", seg, 0)[0]
    if w & 3 or (w >> 32) & 0xFFFF < 18:
        raise ValueError("not an EncodedSlice root")
    d = 8 + 8 * ((w & 0xFFFFFFFF) >> 2)  # data section start
    u32 = lambda o: struct.unpack_from("<I", seg, d + 4 * o)[0]
    u64 = lambda o: struct.unpack_from("<Q", seg, d + 8 * o)[0]
    f32 = lambda o: struct.unpack_from("<f", seg, d + 4 * o)[0]
    bits = seg[d + 49]

    def fov(i):
        p = d + 8 * 18 + 8 * i
        pw = struct.unpack_from("<Q", seg, p)[0]
        if not pw:
            return None
        t = p + 8 + 8 * ((pw & 0xFFFFFFFF) >> 2)
        return struct.unpack_from("<4f", seg, t)  # left, right, up, down tan

    return {
        "frame": u64(0), "mesh": u32(2),
        "quat": (f32(3), f32(4), f32(5), f32(6)), "pos": (f32(7), f32(8), f32(9)),
        "pose_ns": u64(5),
        "index": seg[d + 48], "intra": bits & 1, "last": (bits >> 1) & 1,
        "blend": struct.unpack_from("<H", seg, d + 50)[0],
        "y": u32(13), "h_mb": u32(14), "csd_len": u32(15), "video_len": u32(16),
        "present_ns": u64(9), "present_to_tx_ns": u64(10), "vsync_ns": u64(11),
        "deadline_ns": u64(12), "gpu_end_ns": u64(13), "present_index": u64(15),
        "fov": (fov(0), fov(1)),
    }


def slices(path, device):
    """Yield Slice objects per topic; incomplete ones (capture loss) have complete=False."""
    state = {}  # topic -> (phase, expected_seq, data)
    for m in messages(path, device):
        if not m.to_quest or m.topic not in VIDEO_TOPICS:
            continue
        st = state.get(m.topic)
        if st and m.seq != st[1]:
            if st[0] == "payload":
                yield st[2]
            st = None
        body = unpad(m)
        if st is None or st[0] == "idle":
            if len(m.body) == 8 and struct.unpack_from("<I", m.body)[0] == 0:
                words = struct.unpack_from("<I", m.body, 4)[0]
                st = ("segment", m.seq + 1, words)
            else:
                st = None
        elif st[0] == "segment":
            try:
                hdr = parse_header(m.body[:st[2] * 8])
                st = ("payload", m.seq + 1, Slice(m.topic, m.ts, hdr))
            except (ValueError, struct.error):
                st = None
        elif st[0] == "payload":
            s = st[2]
            s.payload_msgs += 1
            s.flags.append(m.flags)
            need_csd = s.hdr["csd_len"] - len(s.csd)
            if need_csd > 0:
                s.csd += body[:need_csd]
                body = body[need_csd:]
            s.video += body
            if len(s.video) >= s.hdr["video_len"]:
                s.complete = len(s.video) == s.hdr["video_len"]
                yield s
                st = ("idle", m.seq + 1, None)
            else:
                st = ("payload", m.seq + 1, s)
        if st is not None:
            state[m.topic] = (st[0], (st[1]) & 0xFFFF, st[2])
        else:
            state.pop(m.topic, None)
    for st in state.values():
        if st[0] == "payload":
            yield st[2]


# ---- H.264 bitstream helpers (ITU-T H.264 7.3) ----

def nal_units(es):
    """Split an Annex-B stream into (offset, nal bytes)."""
    starts, i = [], 0
    while True:
        j = es.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(es)
        nal = es[s:e]
        if k + 1 < len(starts) and nal.endswith(b"\x00"):
            nal = nal[:-1]  # 4-byte start code of the next NAL
        out.append((s, nal))
    return out


def rbsp(nal):
    out, zeros = bytearray(), 0
    for b in nal[1:]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, data):
        self.d, self.p = data, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)

    def se(self):
        v = self.ue()
        return (v + 1) // 2 if v & 1 else -(v // 2)


def parse_sps(nal):
    b = Bits(rbsp(nal))
    sps = {"profile": b.u(8), "constraints": b.u(8), "level": b.u(8), "id": b.ue()}
    if sps["profile"] in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cf = b.ue()
        if cf == 3:
            b.u(1)
        b.ue(); b.ue(); b.u(1)
        if b.u(1):
            raise ValueError("scaling matrices not handled")
    sps["log2_max_frame_num"] = b.ue() + 4
    sps["poc_type"] = b.ue()
    if sps["poc_type"] == 0:
        sps["log2_max_poc_lsb"] = b.ue() + 4
    elif sps["poc_type"] == 1:
        b.u(1); b.se(); b.se()
        for _ in range(b.ue()):
            b.se()
    sps["ref_frames"] = b.ue()
    b.u(1)
    w_mb, h_mu = b.ue() + 1, b.ue() + 1
    frame_mbs_only = b.u(1)
    if not frame_mbs_only:
        b.u(1)
    b.u(1)
    crop = (b.ue(), b.ue(), b.ue(), b.ue()) if b.u(1) else (0, 0, 0, 0)
    sps["vui"] = b.u(1)
    h_mb = h_mu * (2 - frame_mbs_only)
    sps["mbs"] = (w_mb, h_mb)
    sps["width"] = w_mb * 16 - 2 * (crop[0] + crop[1])
    sps["height"] = h_mb * 16 - 2 * (2 - frame_mbs_only) * (crop[2] + crop[3])
    return sps


def slice_header(nal, sps):
    b = Bits(rbsp(nal[:64]))
    first_mb, stype, pps = b.ue(), b.ue(), b.ue()
    fn = b.u(sps["log2_max_frame_num"]) if sps else None
    return first_mb, stype % 5, fn


def describe(es, sps=None):
    parts = []
    for _, nal in nal_units(es):
        t = nal[0] & 0x1F
        s = f"{NAL_NAMES.get(t, t)}"
        if t in (1, 5):
            fm, st, fn = slice_header(nal, sps)
            s += f"(mb={fm},{'PBI'[st] if st < 3 else st},fn={fn})"
        parts.append(s)
    return " ".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("device", type=int)
    ap.add_argument("--frame", type=int, help="frame index to dump (default: first complete intra frame)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "capture", "runs", "frame.h264"))
    ap.add_argument("--limit", type=int, default=40, help="slices to print (0 = all)")
    a = ap.parse_args()

    frames, sps, printed = {}, None, 0
    total = complete = 0
    print(f"{'ts':>8} {'top':>3} {'frame':>6} sl {'y':>5} {'hMB':>3} I L {'csd':>4} {'video':>7} msgs flags ok  NALs")
    for s in slices(a.pcap, a.device):
        total += 1
        h = s.hdr
        if s.complete:
            complete += 1
            frames.setdefault(h["frame"], {})[h["index"]] = s
        if s.csd and sps is None:
            for _, nal in nal_units(s.csd):
                if nal[0] & 0x1F == 7:
                    sps = parse_sps(nal)
        if a.limit == 0 or printed < a.limit:
            nals = describe(s.csd + s.video, sps) if s.complete else ""
            print(f"{s.ts:8.3f} {s.topic:>3} {h['frame']:>6} {h['index']:>2} {h['y']:>5} {h['h_mb']:>3} "
                  f"{h['intra']} {h['last']} {h['csd_len']:>4} {h['video_len']:>7} {s.payload_msgs:>4} "
                  f"{','.join(hex(f) for f in s.flags):9} {'Y' if s.complete else '-'}   {nals}")
            printed += 1

    full = sorted(f for f, sl in frames.items() if len(sl) == 5)
    print(f"\nslices: {total} parsed, {complete} complete; frames with all 5 slices: {len(full)} of {len(frames)}")
    if sps:
        print(f"SPS: H.264 profile {sps['profile']} constraints 0x{sps['constraints']:02x} level {sps['level'] / 10}, "
              f"{sps['width']}x{sps['height']} ({sps['mbs'][0]}x{sps['mbs'][1]} MBs), "
              f"poc_type {sps['poc_type']}, ref_frames {sps['ref_frames']}")

    pick = a.frame
    if pick is None:
        intra = [f for f in full if frames[f][0].hdr["intra"]]
        pick = (intra or full or [None])[0]
    if pick is None or pick not in frames or len(frames[pick]) != 5:
        print(f"no complete frame {pick} to dump")
        return
    sl = frames[pick]
    csd = next((sl[i].csd for i in range(5) if sl[i].csd), b"")
    es = csd + b"".join(sl[i].video for i in range(5))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(es)
    print(f"frame {pick} (intra={sl[0].hdr['intra']}): {len(es)} bytes -> {os.path.abspath(a.out)}")
    print(f"  {describe(es, sps)}")
    if not sl[0].hdr["intra"]:
        print("  note: P frame, needs its reference frames to decode")


if __name__ == "__main__":
    main()
