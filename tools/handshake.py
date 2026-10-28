"""Print every topic-1 (control) message in order, decoded as Cap'n Proto.

Body layout: 8-byte inner header [type u8][flags u8][u16 a][u32 b], then a
Cap'n Proto message. Clock-sync messages (type 0x06/0x16, flags 0x80) are
raw u64 nanosecond timestamps and are summarized on one line.

Usage: py handshake.py <capture.pcap> <device> [until-seconds]
"""
import struct
import sys

from capnp_dump import pretty
from xrsp import messages


def main():
    path, dev = sys.argv[1], int(sys.argv[2])
    until = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    for m in messages(path, dev):
        if m.ts > until:
            break
        if m.topic != 1:
            continue
        d = "PC->Q" if m.to_quest else "Q->PC"
        typ, flags, a, b = struct.unpack_from("<BBHI", m.body)
        head = f"t={m.ts:.4f} {d} seq={m.seq} type=0x{typ:02x} flags=0x{flags:02x} a={a} b={b} len={len(m.body)}"
        if flags == 0x80 and typ in (0x06, 0x16):
            ts = struct.unpack_from("<4Q", m.body, 8)
            print(f"{head}  CLOCK {[hex(x) for x in ts]}")
            continue
        print(head)
        try:
            text, used = pretty(m.body[8:])
            print("    " + text.replace("\n", "\n    "))
            rest = m.body[8 + used:]
            if rest.strip(b"\0"):
                print(f"    trailing: {rest[:64].hex(' ')}")
        except Exception as e:  # noqa: BLE001 - keep dumping on malformed bodies
            print(f"    (not capnp: {e}) {m.body[8:72].hex(' ')}")


if __name__ == "__main__":
    main()
