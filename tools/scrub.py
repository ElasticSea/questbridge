"""Replace personal identifiers in captured bytes with equal-length placeholders.

Fixture and replay writers run every message and transfer through a Scrubber
before writing, so new fixtures never carry the headset's identity. Equal
length keeps XRSP framing and Cap'n Proto layout (and byte-exact tests) valid.

Identifiers are learned from the capture itself, not hard-coded: each Quest
HELLO (topic 1, inner type 0x81) holds the device id (root data word 3), the
headset serial (text p0) and device/session UUIDs (any UUID text pointer). This PC's host
name is scrubbed too. Placeholders follow docs/PUBLIC_RELEASE_AUDIT.md.
"""
import os
import re
import socket
import struct
from dataclasses import replace

from capnp_codec import decode
from xrsp import messages

SERIAL = b"QUEST0SERIAL00"
UUID = re.compile(rb"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
HELLO_TYPE = 0x81


def _placeholder(base, n):
    return (base * (n // len(base) + 1))[:n]


class Scrubber:
    def __init__(self):
        self.pairs = {}  # old bytes -> new bytes, same length
        self.uuids = 0
        for name in {os.environ.get("COMPUTERNAME", ""), socket.gethostname()}:
            if len(name) >= 4:
                for v in (name, name.upper(), name.lower()):
                    self.add(v.encode(), _placeholder(b"HOSTNAME", len(name)))

    def add(self, old, new):
        if old and old not in self.pairs and old != new:
            assert len(old) == len(new)
            self.pairs[old] = new

    def learn_hello(self, body):
        """Learn identifiers from a topic-1 body (8-byte inner header + Cap'n Proto)."""
        if len(body) < 16 or body[0] != HELLO_TYPE:
            return
        try:
            root, _ = decode(body[8:])
        except ValueError:
            return
        if not root or "s" not in root:
            return
        if len(root["s"]) > 3 and root["s"][3]:
            self.add(struct.pack("<Q", root["s"][3]), struct.pack("<Q", 1))
        texts = [bytes.fromhex(p["b"]).rstrip(b"\0") if p and p.get("l") == 2 else None
                 for p in root["p"]]
        if texts and texts[0]:
            self.add(texts[0], _placeholder(SERIAL, len(texts[0])))
        for t in texts:
            if t and UUID.fullmatch(t) and t not in self.pairs:
                self.uuids += 1
                self.add(t, b"00000000-0000-4000-8000-%012d" % self.uuids)

    def __call__(self, data):
        for old, new in self.pairs.items():
            data = data.replace(old, new)
        return data

    def msg(self, m):
        return replace(m, body=self(m.body), raw=self(m.raw))

    @classmethod
    def from_capture(cls, path, device):
        s = cls()
        for m in messages(path, device):
            if m.topic == 1 and not m.to_quest:
                s.learn_hello(m.body)
        return s
