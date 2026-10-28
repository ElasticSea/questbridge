"""Schema-less Cap'n Proto codec for single-segment messages.

decode(buf) -> (tree, seg_words); encode(tree, seg_words) -> bytes.

Tree nodes:
  None                                   null pointer
  {"s": [data words], "p": [children]}   struct
  {"l": esz, "n": count, "b": hex}       primitive list (esz 0..5; 2 = bytes/text)
  {"lp": [children]}                     list of pointers
  {"lc": [structs], "d": dsz, "q": psz}  composite (struct) list
Every pointer-target node may carry "o": its word offset in the segment,
so encode() can place objects exactly where the original builder did.
"""
import struct

BITS = {0: 0, 1: 1, 2: 8, 3: 16, 4: 32, 5: 64}


def _s30(v):
    v &= 0x3FFFFFFF
    return v - (1 << 30) if v & (1 << 29) else v


class _Reader:
    def __init__(self, seg):
        self.seg = seg

    def word(self, i):
        return struct.unpack_from("<Q", self.seg, i * 8)[0]

    def ptr(self, at):
        w = self.word(at)
        if w == 0:
            return None
        kind = w & 3
        if kind == 2:
            raise ValueError("far pointers unsupported (multi-segment)")
        target = at + 1 + _s30((w & 0xFFFFFFFF) >> 2)
        if kind == 0:
            dsz, psz = (w >> 32) & 0xFFFF, (w >> 48) & 0xFFFF
            node = self.struct(target, dsz, psz)
        elif kind == 1:
            esz, n = (w >> 32) & 7, w >> 35
            if esz == 7:
                tag = self.word(target)
                count = _s30((tag & 0xFFFFFFFF) >> 2)
                dsz, psz = (tag >> 32) & 0xFFFF, (tag >> 48) & 0xFFFF
                step = dsz + psz
                node = {"lc": [self.struct(target + 1 + i * step, dsz, psz) for i in range(count)],
                        "d": dsz, "q": psz, "w": n}
            elif esz == 6:
                node = {"lp": [self.ptr(target + i) for i in range(n)]}
            else:
                nbytes = (n * BITS[esz] + 7) // 8
                node = {"l": esz, "n": n, "b": self.seg[target * 8:target * 8 + nbytes].hex()}
        else:
            raise ValueError("capability pointers unsupported")
        node["o"] = target
        return node

    def struct(self, at, dsz, psz):
        return {"s": [self.word(at + i) for i in range(dsz)],
                "p": [self.ptr(at + dsz + i) for i in range(psz)]}


def decode(buf):
    nseg = struct.unpack_from("<I", buf, 0)[0] + 1
    if nseg != 1:
        raise ValueError(f"{nseg} segments unsupported")
    words = struct.unpack_from("<I", buf, 4)[0]
    seg = buf[8:8 + words * 8]
    if len(seg) != words * 8:
        raise ValueError("truncated segment")
    return _Reader(seg).ptr(0), words


def _size_words(node):
    if "s" in node:
        return len(node["s"]) + len(node["p"])
    if "lc" in node:
        return 1 + len(node["lc"]) * (node["d"] + node["q"])
    if "lp" in node:
        return len(node["lp"])
    return (node["n"] * BITS[node["l"]] + 63) // 64


class _Writer:
    def __init__(self, words):
        self.seg = bytearray(words * 8)
        self.next = 1

    def put(self, i, w):
        struct.pack_into("<Q", self.seg, i * 8, w)

    def alloc(self, node):
        size = _size_words(node)
        at = node.get("o", self.next)
        self.next = max(self.next, at + size)
        return at

    def ptr(self, at, node):
        if node is None:
            return
        target = self.alloc(node)
        off = (target - at - 1) & 0x3FFFFFFF
        if "s" in node:
            self.put(at, (off << 2) | (len(node["s"]) << 32) | (len(node["p"]) << 48))
            self.struct(target, node)
        elif "lc" in node:
            body = len(node["lc"]) * (node["d"] + node["q"])
            self.put(at, (off << 2) | 1 | (7 << 32) | (node.get("w", body) << 35))
            self.put(target, (len(node["lc"]) << 2) | (node["d"] << 32) | (node["q"] << 48))
            for i, s in enumerate(node["lc"]):
                self.struct(target + 1 + i * (node["d"] + node["q"]), s)
        elif "lp" in node:
            self.put(at, (off << 2) | 1 | (6 << 32) | (len(node["lp"]) << 35))
            for i, c in enumerate(node["lp"]):
                self.ptr(target + i, c)
        else:
            self.put(at, (off << 2) | 1 | (node["l"] << 32) | (node["n"] << 35))
            raw = bytes.fromhex(node["b"])
            self.seg[target * 8:target * 8 + len(raw)] = raw

    def struct(self, at, node):
        for i, w in enumerate(node["s"]):
            self.put(at + i, w)
        base = at + len(node["s"])
        for i, c in enumerate(node["p"]):
            self.ptr(base + i, c)


def encode(tree, seg_words=None, order=None):
    """Serialize a tree. Objects are placed back to back in `order` (a list of
    node paths, see walk()); unlisted objects follow depth-first. Nodes that
    still carry "o" offsets keep them when no order is given."""
    if order is not None:
        tree = layout(tree, order)
    if seg_words is None:
        seg_words = 1 + _total(tree)
    w = _Writer(seg_words)
    w.ptr(0, tree)
    return struct.pack("<II", 0, seg_words) + bytes(w.seg)


def walk(node, path="r"):
    """Yield (path, node) for every pointer target, depth-first."""
    if node is None:
        return
    yield path, node
    for k in ("p", "lp"):
        for i, c in enumerate(node.get(k, [])):
            yield from walk(c, f"{path}.{k}{i}")
    for j, s in enumerate(node.get("lc", [])):
        for i, c in enumerate(s["p"]):
            yield from walk(c, f"{path}.lc{j}.p{i}")


def allocation_order(tree):
    """Paths sorted by their recorded offsets: the order Meta's builder allocated them.
    Zeroed holes (objects Meta overwrote, which Cap'n Proto zero-fills) appear as "gap:N"."""
    order, at = [], 1
    for p, n in sorted(walk(tree), key=lambda pn: pn[1]["o"]):
        if n["o"] > at:
            order.append(f"gap:{n['o'] - at}")
        order.append(p)
        at = max(at, n["o"] + _size_words(n))
    return order


def layout(tree, order):
    """Return a copy of tree with "o" offsets assigned by sequential allocation."""
    tree = strip_offsets(tree)
    nodes = dict(walk(tree))
    listed = set(order)
    at = 1
    for p in list(order) + [p for p in nodes if p not in listed]:
        if p.startswith("gap:"):
            at += int(p[4:])
        elif p in nodes:
            nodes[p]["o"] = at
            at += _size_words(nodes[p])
    return tree


def strip_offsets(node):
    """Drop layout hints so encode() falls back to its own allocation order."""
    if isinstance(node, dict):
        node = {k: v for k, v in node.items() if k != "o"}
        for k in ("p", "lp", "lc"):
            if k in node:
                node[k] = [strip_offsets(c) for c in node[k]]
    return node


def _total(node):
    if node is None:
        return 0
    n = _size_words(node)
    for k in ("p", "lp"):
        for c in node.get(k, []):
            n += _total(c)
    for s in node.get("lc", []):
        for c in s["p"]:
            n += _total(c)
    return n
