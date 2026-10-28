"""Schema-less Cap'n Proto message pretty-printer.

Walks the pointer tree of a serialized message (standard segment table) and
prints data words plus decoded pointers: structs, lists, and text-looking
byte lists.
"""
import struct


def _signed30(v):
    v &= 0x3FFFFFFF
    return v - (1 << 30) if v & (1 << 29) else v


class Message:
    def __init__(self, buf):
        nseg = struct.unpack_from("<I", buf, 0)[0] + 1
        sizes = struct.unpack_from(f"<{nseg}I", buf, 4)
        off = 4 + 4 * nseg
        off += (8 - off % 8) % 8
        self.segs = []
        for s in sizes:
            self.segs.append(buf[off:off + s * 8])
            off += s * 8
        self.consumed = off

    def word(self, seg, i):
        return struct.unpack_from("<Q", self.segs[seg], i * 8)[0]


def _fmt_word(w):
    lo, hi = w & 0xFFFFFFFF, w >> 32
    f_lo = struct.unpack("<f", struct.pack("<I", lo))[0]
    f_hi = struct.unpack("<f", struct.pack("<I", hi))[0]
    d = struct.unpack("<d", struct.pack("<Q", w))[0]
    hints = []
    if 1e-6 < abs(d) < 1e9:
        hints.append(f"f64={d:g}")
    for f in (f_lo, f_hi):
        if 1e-4 < abs(f) < 1e6:
            hints.append(f"f32={f:g}")
    return f"0x{w:016x}  u32=({lo}, {hi})" + (f"  [{', '.join(hints)}]" if hints else "")


def dump(msg, out, seg=0, idx=0, indent=0, depth=0):
    pad = "  " * indent
    if depth > 12:
        out.append(f"{pad}...")
        return
    w = msg.word(seg, idx)
    if w == 0:
        out.append(f"{pad}null")
        return
    kind = w & 3
    if kind == 2:  # far pointer
        landing = (w & 0xFFFFFFFF) >> 3
        tseg = w >> 32
        out.append(f"{pad}far -> seg {tseg} word {landing}")
        dump(msg, out, tseg, landing, indent, depth + 1)
        return
    off = _signed30((w & 0xFFFFFFFF) >> 2)
    target = idx + 1 + off
    if kind == 0:
        dsz, psz = (w >> 32) & 0xFFFF, (w >> 48) & 0xFFFF
        out.append(f"{pad}struct (data={dsz}, ptrs={psz})")
        _struct(msg, out, seg, target, dsz, psz, indent + 1, depth)
    elif kind == 1:
        esz, count = (w >> 32) & 7, w >> 35
        if esz == 7:
            tag = msg.word(seg, target)
            n = _signed30((tag & 0xFFFFFFFF) >> 2)
            dsz, psz = (tag >> 32) & 0xFFFF, (tag >> 48) & 0xFFFF
            out.append(f"{pad}list<struct(data={dsz}, ptrs={psz})>[{n}]")
            for i in range(min(n, 16)):
                out.append(f"{pad}  [{i}]")
                _struct(msg, out, seg, target + 1 + i * (dsz + psz), dsz, psz, indent + 2, depth)
            if n > 16:
                out.append(f"{pad}  ... {n - 16} more")
        elif esz == 2:
            raw = msg.segs[seg][target * 8:target * 8 + count]
            txt = raw.rstrip(b"\0")
            if txt and all(32 <= c < 127 for c in txt):
                out.append(f'{pad}text "{txt.decode()}"')
            else:
                out.append(f"{pad}bytes[{count}] {raw[:48].hex(' ')}{' ...' if count > 48 else ''}")
        elif esz == 6:
            out.append(f"{pad}list<ptr>[{count}]")
            for i in range(min(count, 16)):
                dump(msg, out, seg, target + i, indent + 1, depth + 1)
        else:
            bits = {0: 0, 1: 1, 3: 16, 4: 32, 5: 64}[esz]
            nbytes = (count * bits + 7) // 8
            raw = msg.segs[seg][target * 8:target * 8 + nbytes]
            out.append(f"{pad}list<{bits}bit>[{count}] {raw[:64].hex(' ')}{' ...' if nbytes > 64 else ''}")
    else:
        out.append(f"{pad}capability/other 0x{w:016x}")


def _struct(msg, out, seg, start, dsz, psz, indent, depth):
    pad = "  " * indent
    for i in range(dsz):
        out.append(f"{pad}d{i}: {_fmt_word(msg.word(seg, start + i))}")
    for i in range(psz):
        out.append(f"{pad}p{i}:")
        dump(msg, out, seg, start + dsz + i, indent + 1, depth + 1)


def pretty(buf):
    msg = Message(buf)
    out = []
    dump(msg, out)
    return "\n".join(out), msg.consumed
