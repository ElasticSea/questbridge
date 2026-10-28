# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""No fixture may carry a headset's or a PC's identity.

Every HELLO in the fixtures is run through the Scrubber: it must find nothing but placeholders
(serial, device id, UUIDs). Fixture text and every hex string inside the JSON are also checked for
private network addresses. A second test proves the Scrubber removes real-looking identifiers.
"""
import json
import os
import re
import struct
import unittest

from helpers import FIXTURES
from scrub import SERIAL, Scrubber

HEX = re.compile(r"^(?:[0-9a-f]{2})+$")
PRIVATE_IP = re.compile(rb"\b(?:10|192\.168|172\.(?:1[6-9]|2\d|3[01]))(?:\.\d{1,3}){2,3}\b")
PLACEHOLDER_UUID = re.compile(rb"00000000-0000-4000-8000-\d{12}")
HELLO_TYPE = 0x81


def _is_placeholder(old):
    return (old == SERIAL or PLACEHOLDER_UUID.fullmatch(old) is not None
            or old == struct.pack("<Q", 1) or set(old) <= set(b"HOSTNAME"))


def _hex_strings(obj):
    if isinstance(obj, str):
        if len(obj) >= 16 and HEX.match(obj):
            yield bytes.fromhex(obj)
    elif isinstance(obj, dict):
        for v in obj.values():
            yield from _hex_strings(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from _hex_strings(v)


def fixture_files():
    for root, _, names in os.walk(FIXTURES):
        for name in sorted(names):
            if name.endswith(".json"):
                path = os.path.join(root, name)
                with open(path, "rb") as f:
                    yield os.path.relpath(path, FIXTURES), f.read()


def hello_bodies(blobs):
    """Topic-1 HELLO bodies (inner header + Cap'n Proto) found anywhere in the fixture blobs."""
    for b in blobs:
        for i in (0, 8):  # a body on its own, or a raw message with its 8-byte XRSP header
            if len(b) > i + 16 and b[i] == HELLO_TYPE:
                yield b[i:]


class FixturePrivacy(unittest.TestCase):
    def test_hellos_hold_only_placeholders(self):
        for rel, text in fixture_files():
            with self.subTest(fixture=rel):
                scrub = Scrubber()
                scrub.pairs.clear()  # judge the fixture alone, not this machine's name
                for body in hello_bodies(_hex_strings(json.loads(text))):
                    scrub.learn_hello(body)
                self.assertEqual([o for o in scrub.pairs if not _is_placeholder(o)], [])

    def test_no_private_addresses(self):
        for rel, text in fixture_files():
            with self.subTest(fixture=rel):
                blobs = [text, *_hex_strings(json.loads(text))]
                self.assertEqual([m.group() for b in blobs for m in PRIVATE_IP.finditer(b)], [])

    def test_scrubber_cleans_hello(self):
        with open(os.path.join(FIXTURES, "devices", "quest3.json"), encoding="utf-8") as f:
            clean = bytes.fromhex(json.load(f)["hello"])
        real = (clean.replace(SERIAL, b"1WMHH000ABCDEF")
                .replace(b"00000000-0000-4000-8000-000000000001", b"3f2504e0-4f89-41d3-9a0c-0305e82c3301"))
        self.assertNotEqual(real, clean)
        scrub = Scrubber()
        scrub.learn_hello(real)
        out = scrub(real)
        self.assertEqual(len(out), len(real))
        self.assertNotIn(b"1WMHH000ABCDEF", out)
        self.assertNotIn(b"3f2504e0-4f89-41d3-9a0c-0305e82c3301", out)
        self.assertIn(SERIAL, out)


if __name__ == "__main__":
    unittest.main()
