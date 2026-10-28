"""Byte-exact round trips against Meta's own traffic."""
import struct
import unittest

from helpers import load, split_messages
from capnp_codec import encode
from xrsp import encode_message, pack_transfer

SCENARIOS = ["handshake"]


def parse_header(raw):
    flags, topic, _, seq, reserved = struct.unpack_from("<BBHHH", raw)
    return flags, topic, seq, reserved, raw[8:]


class XrspFraming(unittest.TestCase):
    def test_control_messages_reencode(self):
        for sc in SCENARIOS:
            for e in load(sc, "control"):
                raw = bytes.fromhex(e["raw"])
                with self.subTest(sc=sc, t=e["t"]):
                    self.assertEqual(encode_message(*parse_header(raw)[:3], parse_header(raw)[4],
                                                    parse_header(raw)[3]), raw)

    def test_stream_samples_reencode(self):
        for sc in SCENARIOS:
            for group in load(sc, "streams"):
                for s in group["samples"]:
                    raw = bytes.fromhex(s["raw"])
                    flags, topic, seq, reserved, body = parse_header(raw)
                    with self.subTest(sc=sc, topic=group["topic"], t=s["t"]):
                        self.assertEqual(encode_message(flags, topic, seq, body, reserved), raw)

    def test_transfers_repack(self):
        for sc in SCENARIOS:
            xfers = load(sc, "transfers")
            self.assertTrue(xfers)
            for x in xfers:
                raw = bytes.fromhex(x["raw"])
                msgs = [m for m in split_messages(raw) if m[1] != 0]
                with self.subTest(sc=sc, t=x["t"]):
                    self.assertEqual(pack_transfer(msgs), raw)


class ControlCapnp(unittest.TestCase):
    def test_capnp_bodies_reencode(self):
        n = 0
        for sc in SCENARIOS:
            for e in load(sc, "control"):
                if "capnp" not in e:
                    continue
                raw = bytes.fromhex(e["raw"])
                c = e["capnp"]
                with self.subTest(sc=sc, t=e["t"], type=hex(e["inner"]["type"])):
                    capnp = encode(c["tree"], c["words"], c["order"])
                    self.assertEqual(capnp, raw[16:16 + len(capnp)])
                    # Remaining bytes after the Cap'n Proto message are zero padding
                    self.assertFalse(raw[16 + len(capnp):].strip(b"\0"))
                    n += 1
        self.assertGreater(n, 0)

    def test_inner_header_size_field(self):
        """inner.a is the Cap'n Proto size in 16-byte units, rounded up."""
        for sc in SCENARIOS:
            for e in load(sc, "control"):
                if "capnp" not in e:
                    continue
                capnp_bytes = 8 + e["capnp"]["words"] * 8
                with self.subTest(sc=sc, t=e["t"]):
                    self.assertEqual(e["inner"]["a"], -(-capnp_bytes // 16))

    def test_inner_header_reencode(self):
        for sc in SCENARIOS:
            for e in load(sc, "control"):
                raw = bytes.fromhex(e["raw"])
                i = e["inner"]
                with self.subTest(sc=sc, t=e["t"]):
                    self.assertEqual(struct.pack("<BBHI", i["type"], i["flags"], i["a"], i["b"]), raw[8:16])


if __name__ == "__main__":
    unittest.main()
