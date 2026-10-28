"""XRSP message splitter.

Observed framing (little endian), messages packed back to back in each bulk
transfer, transfers padded with topic-0 messages:

  u8  flags      (0x10 so far)
  u8  topic      (0 = padding, 1 = builtin/control, others = streams)
  u16 len_words  (message is 4 + len_words*4 bytes)
  u16 seq        (per topic, per direction)
  u16 reserved
  ... body

Usage: python xrsp.py <capture.pcap> <device> [stats|dump] [topic] [count]
"""
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass

from usbpcap import read


@dataclass
class Msg:
    ts: float
    to_quest: bool
    flags: int
    topic: int
    seq: int
    body: bytes


def messages(path, device):
    # Messages can span consecutive transfers, so keep a carry buffer per direction.
    carry = {True: b"", False: b""}
    for p in read(path):
        if p.device != device or p.transfer != 3 or not p.data or (p.endpoint & 0x7F) != 1:
            continue
        to_quest = not (p.endpoint & 0x80)
        # OUT data is real on submission, IN data on completion; skip the echoes
        if p.inbound == to_quest:
            continue
        d = carry[to_quest] + p.data
        i = 0
        while i + 8 <= len(d):
            flags, topic, words = struct.unpack_from("<BBH", d, i)
            size = 4 + words * 4
            if size < 8:
                i = len(d)
                break
            if i + size > len(d):
                break
            seq = struct.unpack_from("<H", d, i + 4)[0]
            if topic != 0:
                yield Msg(p.ts, to_quest, flags, topic, seq, d[i + 8:i + size])
            i += size
        # A snaplen-truncated transfer loses its tail; resync at the next transfer
        carry[to_quest] = b"" if p.truncated else d[i:]


def stats(path, device):
    s = defaultdict(lambda: [0, 0, None, None, set()])
    for m in messages(path, device):
        k = (m.topic, "PC->Q" if m.to_quest else "Q->PC")
        e = s[k]
        e[0] += 1
        e[1] += len(m.body)
        e[2] = m.ts if e[2] is None else e[2]
        e[3] = m.ts
        e[4].add(m.flags)
    print(f"{'topic':>5} {'dir':>6} {'msgs':>8} {'bytes':>12} {'first':>8} {'last':>8} {'rate/s':>8} flags")
    for (t, d), (n, b, f, l, fl) in sorted(s.items()):
        rate = n / (l - f) if l > f else 0
        print(f"{t:>5} {d:>6} {n:>8} {b:>12} {f:>8.3f} {l:>8.3f} {rate:>8.1f} {','.join(hex(x) for x in sorted(fl))}")


def dump(path, device, topic, count):
    n = 0
    for m in messages(path, device):
        if m.topic != topic:
            continue
        print(f"t={m.ts:.6f} {'PC->Q' if m.to_quest else 'Q->PC'} topic={m.topic} seq={m.seq} len={len(m.body)}")
        b = m.body[:128]
        for i in range(0, len(b), 16):
            print(f"  {i:04x}: {b[i:i+16].hex(' ')}")
        n += 1
        if n >= count:
            break


if __name__ == "__main__":
    path, dev = sys.argv[1], int(sys.argv[2])
    mode = sys.argv[3] if len(sys.argv) > 3 else "stats"
    if mode == "stats":
        stats(path, dev)
    else:
        dump(path, dev, int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 20)
