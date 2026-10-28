# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Build the Quest side for the runtime's offline bench (QB_FAKE_QUEST=<file>).

The recorded Quest 3 handshake (tests/fixtures/handshake) with the HELLOs swapped for another
headset's, taken from a QB_LOG native log of a real session with it ("info   HELLO <hex>": the
phase 1 HELLO, then the Link session's). The last records are the recorded head pose (segment
table, segment), which the runtime replays with a moving yaw.

Record format: u8 kind (0 = Quest message, 1 = head pose template), u32 length LE, raw XRSP message.

Usage: py tools/make_fake_quest.py <native.log> <out.bin>
"""

import json
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def fixture(name):
    return json.loads((ROOT / "tests" / "fixtures" / "handshake" / f"{name}.json").read_text())


def hello_bodies(log_path):
    text = pathlib.Path(log_path).read_text(errors="replace")
    return [bytes.fromhex(h) for h in re.findall(r"info\s+HELLO ([0-9a-f]+)", text)]


def with_body(raw, body):
    """Same XRSP header (flags, topic, seq), new body and length."""
    if (8 + len(body)) % 4:
        raise ValueError("HELLO body not 4-byte aligned")
    header = bytearray(raw[:8])
    struct.pack_into("<H", header, 2, (8 + len(body) - 4) // 4)
    return bytes(header) + body


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    hellos = hello_bodies(sys.argv[1])
    if len(hellos) < 2:
        sys.exit("need the phase 1 and Link session HELLOs in the log")
    records = []
    n_hello = 0
    for e in fixture("control"):
        if e["dir"] != "quest_to_pc":
            continue
        raw = bytes.fromhex(e["raw"])
        if raw[8] == 0x81:  # HELLO: phase 1, then the Link session's
            raw = with_body(raw, hellos[min(n_hello, 1)])
            n_hello += 1
        records.append((0, raw))
    pose = next(g for g in fixture("streams") if g["topic"] == 34 and g["dir"] == "quest_to_pc")
    for s in pose["samples"]:
        records.append((1, bytes.fromhex(s["raw"])))
    with open(sys.argv[2], "wb") as f:
        for kind, raw in records:
            f.write(struct.pack("<BI", kind, len(raw)) + raw)
    print(f"{sys.argv[2]}: {len(records)} records, {n_hello} HELLOs replaced")


if __name__ == "__main__":
    main()
