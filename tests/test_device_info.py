# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""HELLO device description (tools/device_info.py) against tests/fixtures/devices/*.json."""
import json
import os
import unittest

from helpers import FIXTURES
from device_info import parse_body

DEVICES = os.path.join(FIXTURES, "devices")


def device(name):
    with open(os.path.join(DEVICES, f"{name}.json"), encoding="utf-8") as f:
        return json.load(f)


class Golden(unittest.TestCase):
    def test_every_device_parses_to_expected(self):
        names = sorted(n[:-5] for n in os.listdir(DEVICES) if n.endswith(".json"))
        self.assertTrue({"quest3", "quest_pro"} <= set(names))
        for name in names:
            with self.subTest(name):
                d = device(name)
                self.assertEqual(parse_body(bytes.fromhex(d["hello"])), d["expected"])


class Quest3(unittest.TestCase):
    """Hand-checked values (Quest 3, firmware v207, HELLO v3)."""

    def setUp(self):
        self.d = parse_body(bytes.fromhex(device("quest3")["hello"]))

    def test_identity(self):
        self.assertEqual((self.d["product"], self.d["manufacturer"]), ("Meta Quest 3", "Oculus VR"))
        self.assertEqual(self.d["firmware"], "releases-oculus-14.0-v207")
        self.assertEqual(self.d["version"], 3)

    def test_display(self):
        self.assertEqual(self.d["panel"], [4128, 2208])
        self.assertEqual(self.d["encode"], [4096, 2240])
        self.assertEqual(self.d["default_refresh_hz"], 72.0)
        self.assertAlmostEqual(self.d["ipd_m"], 0.0635, places=6)

    def test_fov_per_eye(self):
        self.assertEqual(self.d["fov_deg"]["left"], [44.0, 55.0, 54.0, 40.0])   # up, down, left, right
        self.assertEqual(self.d["fov_deg"]["right"], [44.0, 55.0, 40.0, 54.0])  # mirrored nasal/temporal

    def test_rates_and_modes(self):
        self.assertEqual(self.d["refresh_rates"], [72, 80, 90, 120])
        self.assertEqual([m["hz"] for m in self.d["modes"]], [60, 72, 80, 90, 120])
        self.assertEqual(self.d["modes"][1]["bitrate"], 123_875_920)

    def test_codecs_and_features(self):
        self.assertEqual(self.d["codecs"], [])  # Quest 3 advertises no codec list
        self.assertEqual(self.d["features"], [1, 5, 6, 7, 9, 13, 14, 15, 19, 20])


class QuestPro(unittest.TestCase):
    """Hand-checked values (Quest Pro, firmware v207, HELLO v1 from the Mac host)."""

    def setUp(self):
        self.d = parse_body(bytes.fromhex(device("quest_pro")["hello"]))

    def test_identity(self):
        self.assertEqual(self.d["product"], "Meta Quest Pro")
        self.assertEqual(self.d["version"], 1)

    def test_display(self):
        self.assertEqual(self.d["panel"], [3600, 1920])
        self.assertEqual(self.d["encode"], [4096, 2160])  # 5 strips x 432 px = 27 MB rows
        self.assertEqual(self.d["slices"], 5)
        self.assertEqual(self.d["default_refresh_hz"], 72.0)

    def test_fov_per_eye(self):
        self.assertEqual(self.d["fov_deg"]["left"], [42.0, 54.0, 54.0, 40.0])
        self.assertEqual(self.d["fov_deg"]["right"], [42.0, 54.0, 40.0, 54.0])

    def test_rates_and_modes(self):
        self.assertEqual(self.d["refresh_rates"], [72, 80, 90])  # no 120 Hz
        self.assertEqual([m["hz"] for m in self.d["modes"]], [60, 72, 80, 90])
        self.assertEqual((self.d["modes"][2]["a"], self.d["modes"][2]["b"]), (0.7, 0.65))

    def test_v1_lists(self):
        self.assertEqual(self.d["codecs"], [])  # codec struct only in v3
        self.assertEqual(self.d["features"], [0, 4, 1, 2, 3, 8])  # phase-1 ids, as on Quest 3


if __name__ == "__main__":
    unittest.main()
