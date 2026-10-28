import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
FIXTURES = os.path.join(ROOT, "tests", "fixtures")


def load(scenario, name):
    with open(os.path.join(FIXTURES, scenario, f"{name}.json"), encoding="utf-8") as f:
        return json.load(f)


def split_messages(data):
    """Split a USB transfer into raw XRSP messages (padding included)."""
    out, i = [], 0
    while i + 8 <= len(data):
        size = 4 + struct.unpack_from("<H", data, i + 2)[0] * 4
        out.append(data[i:i + size])
        i += size
    return out


def node(tree, path):
    """Follow a path like 'p3.p0.p2' or 'p5.lc1.p0' from the root struct."""
    n = tree
    for step in path.split(".") if path else []:
        if step.startswith("lc"):
            n = n["lc"][int(step[2:])]
        elif step.startswith("lp"):
            n = n["lp"][int(step[2:])]
        else:
            n = n["p"][int(step[1:])]
    return n


def text(tree, path):
    return bytes.fromhex(node(tree, path)["b"]).rstrip(b"\0").decode()


def u32(tree, path, word, half):
    return (node(tree, path)["s"][word] >> (32 * half)) & 0xFFFFFFFF


def f32(tree, path, word, half):
    return struct.unpack("<f", struct.pack("<I", u32(tree, path, word, half)))[0]


def u16_list(tree, path):
    raw = bytes.fromhex(node(tree, path)["b"])
    return list(struct.unpack(f"<{len(raw) // 2}H", raw))
