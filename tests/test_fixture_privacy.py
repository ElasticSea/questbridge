"""No known personal identifier may appear in test fixtures.

Values are stored as BLAKE2b digests (with their byte length) so this file
doesn't leak them itself. Fixture bytes are scanned as file text and as the
decoded form of every hex string inside the JSON.
"""
import hashlib
import json
import os
import re
import unittest

from helpers import FIXTURES, load
from scrub import Scrubber

# (length, blake2b-128 hex): serial, two device/session UUIDs, device id (u64 LE), LAN IP
PERSONAL = [
    (14, "8405029eadd8f69c84025a232452f20b"),
    (36, "8adee2f714c724c0674d6c4c2ff2a347"),
    (36, "b5c89412b67c33350588ebbbc5adfb63"),
    (8, "9db94422121b21a563e1866418a0aaf2"),
    (13, "1b2d9ed496018304dcf4654807884754"),
]
# Extracted before scrubbing existed; the public-release history rewrite cleans these.
LEGACY = {"handshake"}
HEX = re.compile(r"^(?:[0-9a-f]{2})+$")


def _digest(b):
    return hashlib.blake2b(b, digest_size=16).hexdigest()


def personal_hits(blobs):
    by_len = {}
    for n, d in PERSONAL:
        by_len.setdefault(n, set()).add(d)
    hits = set()
    for blob in blobs:
        for n, digests in by_len.items():
            for i in range(len(blob) - n + 1):
                if _digest(blob[i:i + n]) in digests:
                    hits.add((n, _digest(blob[i:i + n])))
    return hits


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


def fixture_blobs(scenario):
    d = os.path.join(FIXTURES, scenario)
    for name in sorted(os.listdir(d)):
        with open(os.path.join(d, name), "rb") as f:
            text = f.read()
        yield text
        if name.endswith(".json"):
            yield from _hex_strings(json.loads(text))


class FixturePrivacy(unittest.TestCase):
    def test_no_personal_values_in_fixtures(self):
        for sc in sorted(os.listdir(FIXTURES)):
            if sc in LEGACY or not os.path.isdir(os.path.join(FIXTURES, sc)):
                continue
            with self.subTest(scenario=sc):
                self.assertEqual(personal_hits(fixture_blobs(sc)), set())

    def test_scrubber_cleans_hello(self):
        """The legacy handshake still holds the real ids, so it exercises the scrubber."""
        control = load("handshake", "control")
        scrub = Scrubber()
        for e in control:
            if e["dir"] == "quest_to_pc":
                scrub.learn_hello(bytes.fromhex(e["raw"])[8:])
        raws = [bytes.fromhex(e["raw"]) for e in control]
        self.assertTrue(personal_hits(raws))
        clean = [scrub(r) for r in raws]
        self.assertEqual([len(r) for r in clean], [len(r) for r in raws])
        self.assertEqual(personal_hits(clean), set())


if __name__ == "__main__":
    unittest.main()
