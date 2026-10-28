# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Per-device/endpoint traffic summary plus Quest descriptors found during enumeration.

Usage: python -I pcap_summary.py <capture.pcap>
"""
import struct
import sys
from collections import defaultdict

from usbpcap import TRANSFER, read

QUEST_VID = 0x2833


def parse_config_descriptor(d):
    out = []
    i = 0
    while i + 2 <= len(d):
        ln, typ = d[i], d[i + 1]
        if ln == 0:
            break
        if typ == 4 and ln >= 9:
            out.append(f"  intf {d[i+2]} alt {d[i+3]} eps={d[i+4]} class={d[i+5]:02x}/{d[i+6]:02x}/{d[i+7]:02x}")
        elif typ == 5 and ln >= 7:
            ep, attr, mps = d[i + 2], d[i + 3], struct.unpack_from("<H", d, i + 4)[0]
            out.append(f"    ep 0x{ep:02x} {['ctrl', 'iso', 'bulk', 'int'][attr & 3]} maxPacket={mps}")
        i += ln
    return out


def main():
    stats = defaultdict(lambda: [0, 0, None, None])  # count, bytes, first, last
    quest_devices = {}
    pending_desc = {}
    for p in read(sys.argv[1]):
        if p.transfer == 2:
            if not p.inbound and len(p.data) >= 8 and p.data[0] == 0x80 and p.data[1] == 6:
                pending_desc[(p.device, p.irp)] = p.data[3]
            elif p.inbound and (p.device, p.irp) in pending_desc:
                dtype = pending_desc.pop((p.device, p.irp))
                if dtype == 1 and len(p.data) >= 12:
                    vid, pid = struct.unpack_from("<HH", p.data, 8)
                    if vid == QUEST_VID:
                        quest_devices.setdefault(p.device, {"pid": pid, "t": p.ts, "cfg": []})
                elif dtype == 2 and p.device in quest_devices and len(p.data) > 9:
                    quest_devices[p.device]["cfg"] = parse_config_descriptor(p.data)
            continue
        if not p.data:
            continue
        key = (p.device, p.endpoint, TRANSFER.get(p.transfer, "?"))
        s = stats[key]
        s[0] += 1
        s[1] += len(p.data)
        s[2] = p.ts if s[2] is None else s[2]
        s[3] = p.ts

    print("Quest enumerations:")
    for dev, info in quest_devices.items():
        print(f" device {dev} pid=0x{info['pid']:04x} at t={info['t']:.3f}s")
        print("\n".join(info["cfg"]))
    print()
    print(f"{'dev':>4} {'ep':>5} {'type':>5} {'pkts':>9} {'bytes':>13} {'first':>9} {'last':>9}")
    for (dev, ep, t), (n, b, first, last) in sorted(stats.items(), key=lambda kv: -kv[1][1]):
        mark = " <- quest" if dev in quest_devices else ""
        print(f"{dev:>4} 0x{ep:02x} {t:>5} {n:>9} {b:>13} {first:>9.3f} {last:>9.3f}{mark}")


if __name__ == "__main__":
    main()
