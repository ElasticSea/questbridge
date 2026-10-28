# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
import math
import unittest

from helpers import load
from decode_hands import PARENTS, joints, parse_hands, parse_skeleton, parse_skeleton_chunk


class Hands(unittest.TestCase):
    """Topic 8/9 from the hands-on bench dump (fixtures/hands/messages.json)."""

    def setUp(self):
        fx = load("hands", "messages")
        self.frames = [parse_hands(bytes.fromhex(h)[8:]) for h in fx["topic8"]]
        self.skel = {}
        for h in fx["topic9_first_chunks"]:
            hand, _, off, data = parse_skeleton_chunk(bytes.fromhex(h)[8:])
            self.assertEqual(off, 0)
            self.skel[hand] = parse_skeleton(data)

    def test_skeleton(self):
        left, right = self.skel[0], self.skel[1]
        self.assertEqual(left["parents"], PARENTS)
        self.assertEqual(len(left["capsules"]), 19)
        # index proximal bone 37.9 mm; right hand = left mirrored
        self.assertAlmostEqual(math.hypot(*left["bones"][7]["pos"]), 0.0379, 4)
        for a, b in zip(left["bones"], right["bones"]):
            for u, v in zip(a["pos"], b["pos"]):
                self.assertAlmostEqual(-u, v, 6)

    def test_hand_records(self):
        for header, hands in self.frames:
            self.assertEqual(header, 3)
            for h in hands:
                self.assertEqual(h["status"], 1)
                for q in [h["root_rot"], h["pointer_rot"], *h["bones"]]:
                    self.assertAlmostEqual(sum(v * v for v in q), 1, 3)
            self.assertLess(hands[0]["root_pos"][0], hands[1]["root_pos"][0])  # left of right
            self.assertTrue(hands[1]["input_status"] & 0x80)  # right = dominant

    def test_pinch_closes_thumb_and_index(self):
        _, hands = self.frames[1]
        self.assertGreater(hands[0]["pinch"][0], 0.6)
        j = joints(hands[0], self.skel[0])
        self.assertLess(math.dist(j[19][1], j[20][1]), 0.02)  # ThumbTip-IndexTip


if __name__ == "__main__":
    unittest.main()
