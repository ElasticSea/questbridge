# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Cut a raw capture into small, language-neutral test fixtures.

Writes tests/fixtures/<scenario>/:
  control.json    every topic-1 message: raw bytes, inner header, decoded
                  Cap'n Proto tree + allocation order (clock sync summarized)
  streams.json    a few sample messages per (topic, direction): first, smallest,
                  largest, and one per distinct flags value
  transfers.json  whole USB transfers that hold complete messages + padding
  transcript.json normalized message sequence (no ids/timestamps) for
                  comparing our host's session against Meta's
Personal identifiers are replaced with equal-length placeholders (scrub.py).

Usage: py extract_fixtures.py <capture.pcap> <device> <scenario> [until-seconds]
"""
import json
import os
import struct
import sys
from collections import defaultdict

from capnp_codec import allocation_order, decode, strip_offsets
from scrub import Scrubber
from xrsp import messages, transfers

CLOCK_TYPES = (0x06, 0x16)
MAX_TRANSFERS = 24
MAX_STREAM_BYTES = 20000


def direction(to_quest):
    return "pc_to_quest" if to_quest else "quest_to_pc"


def control_entry(m):
    typ, flags, a, b = struct.unpack_from("<BBHI", m.body)
    entry = {"t": round(m.ts, 6), "dir": direction(m.to_quest), "seq": m.seq,
             "xrsp_flags": m.flags, "raw": m.raw.hex(),
             "inner": {"type": typ, "flags": flags, "a": a, "b": b}}
    if flags == 0x80 and typ in CLOCK_TYPES:
        entry["clock"] = list(struct.unpack_from("<4Q", m.body, 8))
        return entry
    tree, words = decode(m.body[8:])
    entry["capnp"] = {"words": words, "order": allocation_order(tree), "tree": strip_offsets(tree)}
    return entry


def pick_stream_samples(msgs):
    """Keep a handful of representative messages instead of thousands of near-copies."""
    keep = {0}
    keep.add(min(range(len(msgs)), key=lambda i: len(msgs[i].raw)))
    small_enough = [i for i in range(len(msgs)) if len(msgs[i].raw) <= MAX_STREAM_BYTES]
    if small_enough:
        keep.add(max(small_enough, key=lambda i: len(msgs[i].raw)))
    seen_flags = set()
    for i, m in enumerate(msgs):
        if m.flags not in seen_flags:
            seen_flags.add(m.flags)
            keep.add(i)
    return [msgs[i] for i in sorted(keep) if len(msgs[i].raw) <= MAX_STREAM_BYTES]


def complete_transfer(data):
    """True if the transfer starts on a message boundary and ends exactly on one."""
    i = 0
    while i + 8 <= len(data):
        words = struct.unpack_from("<H", data, i + 2)[0]
        size = 4 + words * 4
        if size < 8 or i + size > len(data):
            return False
        i += size
    return i == len(data)


def main():
    path, dev, scenario = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    until = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
    out = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures", scenario)
    os.makedirs(out, exist_ok=True)

    scrub = Scrubber.from_capture(path, dev)
    control, streams, transcript = [], defaultdict(list), []
    for m in messages(path, dev):
        if m.ts > until:
            break
        m = scrub.msg(m)
        if m.topic == 1:
            e = control_entry(m)
            control.append(e)
            kind = "clock" if "clock" in e else f"0x{e['inner']['type']:02x}"
            transcript.append({"dir": e["dir"], "topic": 1, "kind": kind})
        else:
            key = (m.topic, m.to_quest)
            if not streams[key]:
                transcript.append({"dir": direction(m.to_quest), "topic": m.topic, "kind": "stream_start"})
            streams[key].append(m)

    stream_out = []
    for (topic, to_quest), msgs in sorted(streams.items()):
        stream_out.append({
            "topic": topic, "dir": direction(to_quest), "count": len(msgs),
            "samples": [{"t": round(m.ts, 6), "seq": m.seq, "flags": m.flags, "raw": m.raw.hex()}
                        for m in pick_stream_samples(msgs)],
        })

    xfers = []
    for p, to_quest in transfers(path, dev):
        if p.ts > until or len(xfers) >= MAX_TRANSFERS:
            break
        if not p.truncated and len(p.data) <= 4096 and complete_transfer(p.data):
            xfers.append({"t": round(p.ts, 6), "dir": direction(to_quest), "raw": scrub(p.data).hex()})

    # Collapse repeated clock-sync exchanges in the transcript
    compact = []
    for e in transcript:
        if compact and e["kind"] == "clock" and compact[-1]["kind"] == "clock":
            continue
        compact.append(e)

    for name, obj in (("control", control), ("streams", stream_out),
                      ("transfers", xfers), ("transcript", compact)):
        with open(os.path.join(out, f"{name}.json"), "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=1)
    sizes = {n: os.path.getsize(os.path.join(out, f"{n}.json")) for n in ("control", "streams", "transfers", "transcript")}
    print(f"{scenario}: {len(control)} control, {len(stream_out)} stream groups, {len(xfers)} transfers, "
          f"{len(compact)} transcript steps; bytes {sizes}")


if __name__ == "__main__":
    main()
