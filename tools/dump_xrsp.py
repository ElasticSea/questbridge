# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Hex-dump the first N XRSP bulk transfers for a device.

Usage: python dump_xrsp.py <capture.pcap> <device> [count] [bytes-per-transfer]
"""
import sys

from usbpcap import read


def main():
    path, dev = sys.argv[1], int(sys.argv[2])
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    width = int(sys.argv[4]) if len(sys.argv) > 4 else 96
    n = 0
    for p in read(path):
        if p.device != dev or p.transfer != 3 or not p.data or (p.endpoint & 0x7F) != 1:
            continue
        d = "Q->PC" if p.endpoint & 0x80 else "PC->Q"
        print(f"#{p.index} t={p.ts:.6f} {d} len={len(p.data)}")
        chunk = p.data[:width]
        for i in range(0, len(chunk), 16):
            row = chunk[i:i + 16]
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
            print(f"  {i:04x}: {row.hex(' '):<48} {asc}")
        n += 1
        if n >= count:
            break


if __name__ == "__main__":
    main()
