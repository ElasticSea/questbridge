"""Golden facts about Meta's handshake (Quest 3, runtime 1.208, firmware v207)."""
import unittest

from helpers import f32, load, text, u16_list, u32

HELLO, CONFIG, ACK = 0x81, 0x82, 0x83


def control(kind_type, direction, version):
    for e in load("handshake", "control"):
        if e["inner"]["type"] == kind_type and e["dir"] == direction and "capnp" in e:
            if (e["capnp"]["tree"]["s"][0] & 0xFFFF) == version:
                return e["capnp"]["tree"]
    raise AssertionError(f"no 0x{kind_type:02x} v{version} from {direction}")


class QuestHello(unittest.TestCase):
    def setUp(self):
        self.t = control(HELLO, "quest_to_pc", 3)

    def test_identity(self):
        self.assertEqual(text(self.t, "p0"), "QUEST0SERIAL00")
        self.assertEqual(text(self.t, "p5"), "releases-oculus-14.0-v207")
        self.assertEqual(text(self.t, "p3.p0.p0"), "Meta Quest 3")
        self.assertEqual(text(self.t, "p3.p0.p1"), "Oculus VR")

    def test_display(self):
        d = "p3.p0"
        self.assertEqual(u32(self.t, d, 0, 1), 4128)        # panel width (both eyes)
        self.assertEqual(u32(self.t, d, 1, 0), 2208)        # panel height
        self.assertEqual(f32(self.t, d, 1, 1), 72.0)        # default refresh
        self.assertAlmostEqual(f32(self.t, d, 2, 0), 0.0635, places=4)  # IPD metres
        self.assertEqual(u32(self.t, d, 5, 0), 4096)        # encode width
        self.assertEqual(u32(self.t, d, 5, 1), 2240)        # encode height

    def test_eye_fov_degrees(self):
        left = [f32(self.t, "p3.p0.p3", 0, 0), f32(self.t, "p3.p0.p3", 0, 1),
                f32(self.t, "p3.p0.p3", 1, 0), f32(self.t, "p3.p0.p3", 1, 1)]
        self.assertEqual(left, [44.0, 55.0, 54.0, 40.0])

    def test_refresh_rates(self):
        raw = bytes.fromhex(self.t["p"][4]["b"])
        self.assertEqual(sorted(int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)),
                         [72, 80, 90, 120])


class PcConfig(unittest.TestCase):
    def setUp(self):
        self.t = control(CONFIG, "pc_to_quest", 3)

    def test_stream_settings(self):
        self.assertEqual(text(self.t, "p2"), "USB3")
        self.assertEqual(u32(self.t, "", 1, 0), 5)    # slices
        self.assertEqual(u32(self.t, "", 2, 0), 72)   # refresh Hz
        self.assertEqual(u16_list(self.t, "p1"), [1, 3, 2, 19, 7, 14, 15])


class QuestAck(unittest.TestCase):
    def test_selected_mode(self):
        t = control(ACK, "quest_to_pc", 3)
        self.assertEqual(u32(t, "p0", 0, 0), 72)
        self.assertEqual(u32(t, "p0.p1", 0, 0), 123_875_920)


class Transcript(unittest.TestCase):
    def test_handshake_order(self):
        steps = [(s["dir"][0], s["kind"]) for s in load("handshake", "transcript") if s["topic"] == 1]
        q, p = "q", "p"
        expected = [(q, "0x81"), (p, "0x82"), (q, "0x83"), (p, "0x89"), (q, "0x8a"), (p, "0x87"), (q, "0x80"),
                    (q, "0x85"),
                    (q, "0x81"), (p, "0x82"), (q, "0x83"), (p, "0x89"), (q, "0x8a"), (p, "0x87"), (q, "0x80")]
        self.assertEqual(steps[:len(expected)], expected)
        self.assertIn((p, "clock"), steps[len(expected):len(expected) + 3])


if __name__ == "__main__":
    unittest.main()
