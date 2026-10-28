"""Decode the device description in a Quest HELLO (topic 1, inner type 0x81).

Layout: docs/protocol/spec.md §8. The same parse lives in native/src/session.cpp
(parse_device_info); both are checked against tests/fixtures/devices/*.json.

Input (first argument):
  control.json          a fixture's control messages (the last Quest HELLO wins)
  <file>.txt | <hex>    hex of one HELLO: the topic-1 body (starts 81) or the whole
                        XRSP message; qlhost / QL_LOG=1 print it as "info HELLO <hex>"
  <file>.pcap <device>  a USBPcap capture

Usage:
  py tools/device_info.py <input> [device]               print the decoded description
  py tools/device_info.py <input> [device] --save <name> write tests/fixtures/devices/<name>.json
                                                         (identifiers scrubbed)
"""
import json
import os
import struct
import sys

from capnp_codec import decode
from scrub import Scrubber

HELLO = 0x81
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _u32(n, word, half):
    s = n["s"] if n else []
    return (s[word] >> (32 * half)) & 0xFFFFFFFF if word < len(s) else 0


def _f32(n, word, half):
    return struct.unpack("<f", struct.pack("<I", _u32(n, word, half)))[0]


def _f64(n, word):
    s = n["s"] if n else []
    return struct.unpack("<d", struct.pack("<Q", s[word] if word < len(s) else 0))[0]


def _ptr(n, i):
    return n["p"][i] if n and "p" in n and i < len(n["p"]) else None


def _text(n):
    return bytes.fromhex(n["b"]).rstrip(b"\0").decode() if n and "b" in n else ""


def _list(n, fmt):
    if not n or "b" not in n:
        return []
    raw = bytes.fromhex(n["b"])
    size = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, raw, i)[0] for i in range(0, len(raw) - size + 1, size)]


def parse(root):
    """HELLO root struct -> device description dict (None if it has none)."""
    d = _ptr(_ptr(root, 3), 0)
    if not d or "s" not in d:
        return None
    fov = {}
    for eye, p in (("left", 3), ("right", 4)):
        e = _ptr(d, p)
        fov[eye] = [_f32(e, 0, 0), _f32(e, 0, 1), _f32(e, 1, 0), _f32(e, 1, 1)]  # up, down, left, right
    modes = []
    for m in (_ptr(d, 5) or {}).get("lc", []):
        scale = _ptr(m, 0)
        modes.append({"hz": _u32(m, 0, 0), "a": _f64(scale, 0), "b": _f64(scale, 1),
                      "bitrate": (_ptr(m, 1) or {"s": [0]})["s"][0]})
    return {
        "version": root["s"][0] & 0xFFFF,
        "product": _text(_ptr(d, 0)),
        "manufacturer": _text(_ptr(d, 1)),
        "firmware": _text(_ptr(root, 5)),
        "panel": [_u32(d, 0, 1), _u32(d, 1, 0)],
        "encode": [_u32(d, 5, 0), _u32(d, 5, 1)],
        "default_refresh_hz": _f32(d, 1, 1),
        "ipd_m": _f32(d, 2, 0),
        "fov_deg": fov,
        "slices": _u32(d, 6, 0),
        "refresh_rates": sorted(_list(_ptr(root, 4), "<I")),
        "modes": modes,
        "codecs": _list(_ptr(_ptr(d, 6), 0), "<H"),
        "features": _list(_ptr(root, 6), "<H"),
    }


def hello_body(data):
    """Topic-1 body (inner header + Cap'n Proto) from a body or a whole XRSP message."""
    if data and data[0] == HELLO:
        return data
    if len(data) > 16 and data[1] == 1 and data[8] == HELLO:
        return data[8:]
    raise ValueError("not a HELLO")


def parse_body(body):
    root, _ = decode(hello_body(body)[8:])
    return parse(root)


def _read_input(args):
    src = args[0]
    if src.endswith(".pcap"):
        from xrsp import messages
        bodies = [m.body for m in messages(src, int(args[1])) if m.topic == 1 and not m.to_quest
                  and m.body[:1] == bytes([HELLO])]
    elif src.endswith(".json"):
        with open(src, encoding="utf-8") as f:
            bodies = [bytes.fromhex(e["raw"])[8:] for e in json.load(f)
                      if e["dir"] == "quest_to_pc" and e.get("inner", {}).get("type") == HELLO]
    else:
        text = open(src, encoding="utf-8").read() if os.path.exists(src) else src
        bodies = [bytes.fromhex("".join(text.split()[-1:]))]  # last word: "... HELLO <hex>"
    if not bodies:
        raise SystemExit("no HELLO found")
    return bodies[-1]


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    save = argv[argv.index("--save") + 1] if "--save" in argv else None
    args = [a for i, a in enumerate(argv) if a != "--save" and (i == 0 or argv[i - 1] != "--save")]
    body = hello_body(_read_input(args))
    info = parse_body(body)
    print(json.dumps(info, indent=1))
    if save:
        s = Scrubber()
        s.learn_hello(body)
        out = os.path.join(ROOT, "tests", "fixtures", "devices", f"{save}.json")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"hello": s(body).hex(), "expected": parse_body(s(body))}, f, indent=1)
            f.write("\n")
        print("wrote", out)


if __name__ == "__main__":
    main(sys.argv[1:])
