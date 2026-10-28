"""Decode the Quest->PC hand-tracking streams (topic 8 Hands, topic 9 Skeleton).

Both only flow after the PC sends topic 28 InputControl {u16 type 1, u16 1}.
Neither is Cap'n Proto: the bodies are packed little-endian C structs.
Quaternions are x, y, z, w; positions in metres; Quest tracking space.

Topic 8 (one XRSP message, flags 0x10, 1320 B body, 60/s). Hand data itself
updates at ~25-30 Hz (repeated between updates); the pointer pose every message.
  0x000 u32  3 (both entries present?)
  0x004      hand record, left   (656 B, HAND below)
  0x294      hand record, right
  0x524 u32  0
Hand record (offsets inside the record):
  0x000 u64  0
  0x008 u32  tracking status (1 = tracked, 0 = lost; then the record is zero)
  0x00c 4f   root (wrist) orientation
  0x01c 3f   root position
  0x028 3f   0 (velocity?)
  0x034 24x4f bone rotations, local to parent, OVR hand bone order (BONES)
  0x1b4 f64  sample time, s (Quest clock)
  0x1bc f64  same value again (requested time?)
  0x1c4 f32  hand confidence (1.0 = high)
  0x1c8 f32  hand scale (multiplies the bind skeleton)
  0x1cc 5f   finger confidences, thumb..pinky
  0x1e0 u32  0, 0x1e4 u32 0
  0x1e8 5x4f per-finger values, thumb..pinky (joint angles in rad? 0..1.35)
  0x238 6f   ~0.998 (continuous confidences?)
  0x250 u32  0
  0x254 4f   pinch strength index, middle, ring, pinky (0..1)
  0x264 4f   pointer (aim) orientation
  0x274 3f   pointer position
  0x280 3x u32 0
  0x28c u32  input status bits: 0x02 pointer valid, 0x80 dominant hand,
             0x1000/0x2000 seen (pinch-related?)

Topic 9 (46 messages, 4120 B body, once after hands are enabled): two 94208 B
blobs (left, right) in 4096 B chunks. Chunk: u32 hand (0 left, 1 right),
u32 0, f64 time (s), u32 byte offset into the blob, 4096 B data, u32 0.
Blob:
  0x0000 u32  skeleton version 0xdf000001, u32 bones 24, u32 capsules 19
  0x000c u32  mesh version 0xdf000001, u32 vertices 1360, u32 indices 6942
  0x0018 24 x (4f rotation, 3f translation) bind pose, local to parent
  0x02b8 24 x i16 parent bone (-1 for the root)
  0x02e8 19 x (u32 bone, 3f p0, 3f p1, f32 radius) collision capsules
  0x0548 440 B constants (pi/4, pi, ...; joint limits?), unknown
  0x0700 48 x i16 index map (-1 = none), unknown
  0x0760 V x 3f vertex positions (bind space)
  0x4720 I x u16 triangle indices
  then   V x 3f normals, V x 2f uv, V x 4 i16 blend bones, V x 4f blend weights
  0x1671c 2276 B unused tail
The right hand blob is the left one with positions negated.

Usage: py decode_hands.py <dump.txt> [--json out.json] [--skeleton out.json]
dump.txt is tools/pyhost/handshake.py's XRSP_DUMP=8,9 output: "<ns> <topic> <hex>" lines,
hex = whole XRSP message (8 B header + body).
"""
import json
import math
import struct
import sys

BONES = ["WristRoot", "ForearmStub",
         "Thumb0", "Thumb1", "Thumb2", "Thumb3",
         "Index1", "Index2", "Index3",
         "Middle1", "Middle2", "Middle3",
         "Ring1", "Ring2", "Ring3",
         "Pinky0", "Pinky1", "Pinky2", "Pinky3",
         "ThumbTip", "IndexTip", "MiddleTip", "RingTip", "PinkyTip"]
PARENTS = [-1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18]
# OpenXR XR_EXT_hand_tracking joint -> OVR bone (None: synthesize, e.g. palm =
# midpoint of wrist and Middle1, other metacarpals between wrist and proximal)
OPENXR = {"PALM": None, "WRIST": 0,
          "THUMB_METACARPAL": 3, "THUMB_PROXIMAL": 4, "THUMB_DISTAL": 5, "THUMB_TIP": 19,
          "INDEX_METACARPAL": None, "INDEX_PROXIMAL": 6, "INDEX_INTERMEDIATE": 7,
          "INDEX_DISTAL": 8, "INDEX_TIP": 20,
          "MIDDLE_METACARPAL": None, "MIDDLE_PROXIMAL": 9, "MIDDLE_INTERMEDIATE": 10,
          "MIDDLE_DISTAL": 11, "MIDDLE_TIP": 21,
          "RING_METACARPAL": None, "RING_PROXIMAL": 12, "RING_INTERMEDIATE": 13,
          "RING_DISTAL": 14, "RING_TIP": 22,
          "LITTLE_METACARPAL": 15, "LITTLE_PROXIMAL": 16, "LITTLE_INTERMEDIATE": 17,
          "LITTLE_DISTAL": 18, "LITTLE_TIP": 23}
HANDS = ("left", "right")
HANDS_BODY = 1320
HAND_RECORD = 0x290
SKELETON_CHUNK = 4096
SKELETON_BLOB = 23 * SKELETON_CHUNK
HAND_VERSION = 0xDF000001


def parse_hands(body):
    """Topic 8 body -> (u32 header, [left, right] dicts)."""
    assert len(body) == HANDS_BODY, len(body)
    out = []
    for i in range(2):
        r = 4 + i * HAND_RECORD
        t = struct.unpack_from("<2d", body, r + 0x1b4)
        out.append({
            "status": struct.unpack_from("<I", body, r + 0x08)[0],
            "root_rot": struct.unpack_from("<4f", body, r + 0x0c),
            "root_pos": struct.unpack_from("<3f", body, r + 0x1c),
            "bones": [struct.unpack_from("<4f", body, r + 0x34 + 16 * b) for b in range(24)],
            "time": t[0], "time2": t[1],
            "confidence": struct.unpack_from("<f", body, r + 0x1c4)[0],
            "scale": struct.unpack_from("<f", body, r + 0x1c8)[0],
            "finger_confidence": struct.unpack_from("<5f", body, r + 0x1cc),
            "finger_values": [struct.unpack_from("<4f", body, r + 0x1e8 + 16 * f) for f in range(5)],
            "confidence2": struct.unpack_from("<6f", body, r + 0x238),
            "pinch": struct.unpack_from("<4f", body, r + 0x254),
            "pointer_rot": struct.unpack_from("<4f", body, r + 0x264),
            "pointer_pos": struct.unpack_from("<3f", body, r + 0x274),
            "input_status": struct.unpack_from("<I", body, r + 0x28c)[0],
        })
    return struct.unpack_from("<I", body, 0)[0], out


def parse_skeleton_chunk(body):
    """Topic 9 body -> (hand index, time s, blob offset, 4096 B data)."""
    hand, _, t, off = struct.unpack_from("<IIdI", body, 0)
    return hand, t, off, body[20:20 + SKELETON_CHUNK]


def parse_skeleton(blob):
    """Bind skeleton from a topic 9 blob; mesh too when the blob is complete."""
    sv, nb, nc, mv, nv, ni = struct.unpack_from("<6I", blob, 0)
    assert sv == mv == HAND_VERSION and nb == 24, (hex(sv), nb)
    sk = {"bones": [], "parents": list(struct.unpack_from(f"<{nb}h", blob, 0x2b8)), "capsules": []}
    for b in range(nb):
        v = struct.unpack_from("<7f", blob, 0x18 + 28 * b)
        sk["bones"].append({"rot": v[:4], "pos": v[4:]})
    for c in range(nc):
        v = struct.unpack_from("<I7f", blob, 0x2e8 + 32 * c)
        sk["capsules"].append({"bone": v[0], "p0": v[1:4], "p1": v[4:7], "radius": v[7]})
    if len(blob) >= SKELETON_BLOB:
        o = 0x760
        mesh = {"positions": [struct.unpack_from("<3f", blob, o + 12 * i) for i in range(nv)]}
        o += 12 * nv
        mesh["indices"] = list(struct.unpack_from(f"<{ni}H", blob, o))
        o += 2 * ni
        mesh["normals"] = [struct.unpack_from("<3f", blob, o + 12 * i) for i in range(nv)]
        o += 12 * nv
        mesh["uvs"] = [struct.unpack_from("<2f", blob, o + 8 * i) for i in range(nv)]
        o += 8 * nv
        mesh["blend_bones"] = [struct.unpack_from("<4h", blob, o + 8 * i) for i in range(nv)]
        o += 8 * nv
        mesh["blend_weights"] = [struct.unpack_from("<4f", blob, o + 16 * i) for i in range(nv)]
        sk["mesh"] = mesh
    return sk


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = qmul(qmul(q, (*v, 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def joints(hand, skeleton):
    """World joint positions/rotations: root pose, then per bone the bind
    translation (times hand scale) and the streamed local rotation."""
    s = hand["scale"]
    world = []
    for b in range(24):
        p = skeleton["parents"][b]
        prot, ppos = (hand["root_rot"], hand["root_pos"]) if p < 0 else world[p]
        t = qrot(prot, tuple(c * s for c in skeleton["bones"][b]["pos"]))
        world.append((qmul(prot, hand["bones"][b]), tuple(a + c for a, c in zip(ppos, t))))
    return world


def read_dump(path):
    hands, chunks = [], {0: {}, 1: {}}
    for line in open(path):
        ns, topic, hexmsg = line.split()
        body = bytes.fromhex(hexmsg)[8:]
        if topic == "8":
            hands.append((int(ns), *parse_hands(body)))
        elif topic == "9":
            h, _, off, data = parse_skeleton_chunk(body)
            chunks[h][off] = data
    blobs = [b"".join(c[k] for k in sorted(c)) for c in chunks.values()]
    return hands, [parse_skeleton(b) if len(b) >= 0x548 else None for b in blobs]


def main(argv):
    hands, skeletons = read_dump(argv[0])
    for i, sk in enumerate(skeletons):
        if sk:
            print(f"{HANDS[i]} skeleton: bone lengths (mm)",
                  [round(1000 * math.hypot(*b["pos"]), 1) for b in sk["bones"]],
                  "mesh" if "mesh" in sk else "(no mesh)")
    frames = []
    for ns, _, recs in hands:
        frame = {"ns": ns}
        for i, h in enumerate(recs):
            if h["status"] and skeletons[i]:
                h["joints"] = [{"bone": BONES[b], "rot": r, "pos": p}
                               for b, (r, p) in enumerate(joints(h, skeletons[i]))]
            frame[HANDS[i]] = h
        frames.append(frame)
    last = next((f for f in reversed(frames) if f["left"]["status"] or f["right"]["status"]), frames[-1])
    for name in HANDS:
        h = last[name]
        print(f"{name}: status {h['status']} t {h['time']:.4f} conf {h['confidence']} scale {h['scale']:.4f} "
              f"pinch {[round(v, 2) for v in h['pinch']]} input 0x{h['input_status']:x}")
        for j in h.get("joints", []):
            print(f"  {j['bone']:12s} pos {' '.join(f'{v:+.3f}' for v in j['pos'])}")
    print(f"{len(frames)} hand messages")
    for flag, out in (("--json", frames), ("--skeleton", skeletons)):
        if flag in argv:
            with open(argv[argv.index(flag) + 1], "w") as f:
                json.dump(out, f)


if __name__ == "__main__":
    main(sys.argv[1:])
