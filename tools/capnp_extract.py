# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Recover Cap'n Proto schemas embedded in a compiled binary.

Generated C++ embeds each schema node as a single-segment message whose root
is a schema.capnp `Node` struct (5 data words, 6 pointers). We scan for that
root pointer, decode each node, and print reconstructed .capnp text.

Usage: python -I capnp_extract.py <binary> [name-filter]
"""
import struct
import sys
from collections import defaultdict

NODE_ROOT = bytes([0, 0, 0, 0, 5, 0, 6, 0])

PRIM = {0: "Void", 1: "Bool", 2: "Int8", 3: "Int16", 4: "Int32", 5: "Int64",
        6: "UInt8", 7: "UInt16", 8: "UInt32", 9: "UInt64", 10: "Float32",
        11: "Float64", 12: "Text", 13: "Data", 18: "AnyPointer"}


class Seg:
    def __init__(self, buf, base):
        self.buf, self.base = buf, base

    def word(self, i):
        o = self.base + i * 8
        if o < 0 or o + 8 > len(self.buf):
            raise ValueError("out of bounds")
        return struct.unpack_from("<Q", self.buf, o)[0]


class Struct:
    def __init__(self, seg, data, dsize, psize):
        self.seg, self.data, self.dsize, self.psize = seg, data, dsize, psize

    def _bits(self, bit, width):
        w, b = divmod(bit, 64)
        if w >= self.dsize:
            return 0
        return (self.seg.word(self.data + w) >> b) & ((1 << width) - 1)

    def u16(self, off16): return self._bits(off16 * 16, 16)
    def u32(self, off32): return self._bits(off32 * 32, 32)
    def u64(self, off64): return self._bits(off64 * 64, 64)
    def bool(self, bit): return self._bits(bit, 1) == 1

    def ptr(self, i):
        if i >= self.psize:
            return None
        return self.data + self.dsize + i

    def struct(self, i):
        p = self.ptr(i)
        return read_struct(self.seg, p) if p is not None else None

    def list(self, i):
        p = self.ptr(i)
        return read_list(self.seg, p) if p is not None else []

    def text(self, i):
        p = self.ptr(i)
        return read_text(self.seg, p) if p is not None else ""


def _signed30(v):
    v &= 0x3FFFFFFF
    return v - (1 << 30) if v & (1 << 29) else v


def read_struct(seg, pidx):
    w = seg.word(pidx)
    if w == 0:
        return None
    if w & 3 != 0:
        raise ValueError("not a struct pointer")
    off = _signed30((w & 0xFFFFFFFF) >> 2)
    return Struct(seg, pidx + 1 + off, (w >> 32) & 0xFFFF, (w >> 48) & 0xFFFF)


def read_list(seg, pidx):
    w = seg.word(pidx)
    if w == 0:
        return []
    if w & 3 != 1:
        raise ValueError("not a list pointer")
    off = _signed30((w & 0xFFFFFFFF) >> 2)
    start = pidx + 1 + off
    esize = (w >> 32) & 7
    if esize != 7:
        raise ValueError("expected composite list")
    tag = seg.word(start)
    count = _signed30((tag & 0xFFFFFFFF) >> 2)
    dsize, psize = (tag >> 32) & 0xFFFF, (tag >> 48) & 0xFFFF
    step = dsize + psize
    return [Struct(seg, start + 1 + i * step, dsize, psize) for i in range(count)]


def read_text(seg, pidx):
    w = seg.word(pidx)
    if w == 0:
        return ""
    if w & 3 != 1 or (w >> 32) & 7 != 2:
        raise ValueError("not a text pointer")
    off = _signed30((w & 0xFFFFFFFF) >> 2)
    n = w >> 35
    o = seg.base + (pidx + 1 + off) * 8
    raw = seg.buf[o:o + n]
    return raw.rstrip(b"\0").decode("utf-8", "replace")


def decode_type(t):
    which = t.u16(0)
    if which in PRIM:
        return PRIM[which], None
    if which == 14:
        inner, ref = decode_type(t.struct(0))
        return f"List({inner})", ref
    if which in (15, 16, 17):
        return None, t.u64(1)
    return f"?type{which}", None


def decode_node(buf, base):
    seg = Seg(buf, base)
    n = read_struct(seg, 0)
    node = {
        "id": n.u64(0),
        "displayName": n.text(0),
        "prefixLen": n.u32(2),
        "which": n.u16(6),
        "scopeId": n.u64(2),
        "nested": [(x.text(0), x.u64(0)) for x in n.list(1)],
    }
    w = node["which"]
    if w == 1:  # struct
        node["dataWords"] = n.u16(7)
        node["ptrCount"] = n.u16(12)
        node["isGroup"] = n.bool(224)
        node["discCount"] = n.u16(15)
        node["discOffset"] = n.u32(8)
        fields = []
        for f in n.list(3):
            fd = {
                "name": f.text(0),
                "codeOrder": f.u16(0),
                "disc": f.u16(1) ^ 0xFFFF,
                "kind": f.u16(4),
                "ordinal": f.u16(6) if f.u16(5) == 1 else None,
            }
            if fd["kind"] == 0:
                fd["offset"] = f.u32(1)
                fd["type"] = decode_type(f.struct(2))
            else:
                fd["groupId"] = f.u64(2)
            fields.append(fd)
        node["fields"] = fields
    elif w == 2:  # enum
        node["enumerants"] = [(e.text(0), e.u16(0)) for e in n.list(3)]
    elif w == 3:  # interface
        node["methods"] = [(m.text(0), m.u64(1), m.u64(2)) for m in n.list(3)]
    return node


def scan(buf):
    nodes = {}
    i = buf.find(NODE_ROOT)
    while i != -1:
        if i % 8 == 0:
            try:
                node = decode_node(buf, i)
                name = node["displayName"]
                if node["id"] and ".capnp" in name and name.isprintable():
                    nodes[node["id"]] = node
            except (ValueError, struct.error, IndexError):
                pass
        i = buf.find(NODE_ROOT, i + 1)
    return nodes


def short(nodes, nid):
    n = nodes.get(nid)
    if not n:
        return f"@0x{nid:016x}"
    return n["displayName"][n["prefixLen"]:]


def render(nodes, node, indent=0):
    pad = "  " * indent
    name = node["displayName"][node["prefixLen"]:]
    out = []
    w = node["which"]
    if w == 1 and not node["isGroup"]:
        out.append(f"{pad}struct {name} @0x{node['id']:016x} {{  # {node['dataWords']} data words, {node['ptrCount']} ptrs")
        out += render_fields(nodes, node, indent + 1)
        for nn, nid in node["nested"]:
            if nid in nodes:
                out += render(nodes, nodes[nid], indent + 1)
        out.append(f"{pad}}}")
    elif w == 2:
        out.append(f"{pad}enum {name} @0x{node['id']:016x} {{")
        for en, co in node["enumerants"]:
            out.append(f"{pad}  {en} @{co};")
        out.append(f"{pad}}}")
    elif w == 3:
        out.append(f"{pad}interface {name} @0x{node['id']:016x} {{")
        for mn, p, r in node["methods"]:
            out.append(f"{pad}  {mn} ({short(nodes, p)}) -> ({short(nodes, r)});")
        out.append(f"{pad}}}")
    elif w == 0:
        out.append(f"{pad}# file {node['displayName']} @0x{node['id']:016x}")
        for nn, nid in node["nested"]:
            if nid in nodes:
                out += render(nodes, nodes[nid], indent)
    return out


def render_fields(nodes, node, indent):
    pad = "  " * indent
    out = []
    in_union = node["discCount"] > 0
    if in_union:
        out.append(f"{pad}# union discriminant at u16 offset {node['discOffset']}")
    for f in sorted(node["fields"], key=lambda x: x["ordinal"] if x["ordinal"] is not None else 1 << 20):
        ordinal = f"@{f['ordinal']}" if f["ordinal"] is not None else ""
        tag = f"  # union tag {f['disc']}" if f["disc"] != 0xFFFF else ""
        if f["kind"] == 0:
            tname, ref = f["type"]
            if ref is not None:
                tname = short(nodes, ref)
            out.append(f"{pad}{f['name']} {ordinal} :{tname};  # offset {f['offset']}{tag}")
        else:
            g = nodes.get(f["groupId"])
            out.append(f"{pad}{f['name']} :group {{{tag}")
            if g:
                out += render_fields(nodes, g, indent + 1)
            out.append(f"{pad}}}")
    return out


def main():
    path = sys.argv[1]
    flt = sys.argv[2] if len(sys.argv) > 2 else None
    buf = open(path, "rb").read()
    nodes = scan(buf)
    by_file = defaultdict(list)
    for n in nodes.values():
        by_file[n["displayName"].split(":")[0]].append(n)
    for fname in sorted(by_file):
        if flt and flt not in fname:
            continue
        print(f"######## {fname}  ({len(by_file[fname])} nodes)")
        ids = {n["id"] for n in by_file[fname]}
        nested_ids = {nid for n in by_file[fname] for _, nid in n["nested"]}
        group_ids = {f.get("groupId") for n in by_file[fname] for f in n.get("fields", [])}
        for n in sorted(by_file[fname], key=lambda x: x["displayName"]):
            if n["id"] in nested_ids or n["id"] in group_ids:
                continue
            print("\n".join(render(nodes, n)))
            print()


if __name__ == "__main__":
    main()
