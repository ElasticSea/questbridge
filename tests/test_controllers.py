"""Golden facts about controllers and haptics over Meta's Link (Quest 3 + Touch Plus,
SensorProbe guided run, runtime 1.208). Fixture: tests/fixtures/sensors/controllers.json,
labelled messages written by `decode_controllers.py golden`."""
import math
import struct
import unittest

from helpers import load  # noqa: I001 (sets up the tools/ import path)

from capnp_codec import decode
from decode_controllers import RIPC_CALL, RIPC_CONNECT, RIPC_LOOKUP, decode_input, decode_tracking, ripc_frame

LEFT, RIGHT = 0, 1


def msgs():
    return {m["label"]: m for m in load("sensors", "controllers")}


def entry(label, index):
    _, entries = decode_input(bytes.fromhex(msgs()[label]["capnp"]))
    return next(e for e in entries if e["index"] == index)


class DeviceInput(unittest.TestCase):
    """Topic 35: root {u8 flags, u8, u16 controller mask, f32, f32 ipd} + list of 8-word entries."""

    def test_buttons_word(self):
        cases = {"a": (RIGHT, 0x1), "b": (RIGHT, 0x2), "r_stick_click": (RIGHT, 0x80000004),
                 "x": (LEFT, 0x100), "y": (LEFT, 0x200), "l_stick_click": (LEFT, 0x80000400),
                 "menu": (LEFT, 0x40000000)}
        for label, (idx, val) in cases.items():
            with self.subTest(label):
                self.assertEqual(entry(label, idx)["buttons"], val)

    def test_entry_layout(self):
        m = msgs()["a"]
        root, _ = decode(bytes.fromhex(m["capnp"]))
        lst = root["p"][0]
        self.assertEqual(len(lst["lc"]), 2)
        self.assertEqual((lst["d"], lst["q"]), (8, 0))      # 8 data words, no pointers
        raw = struct.pack("<8Q", *lst["lc"][1]["s"])
        self.assertEqual(raw[0], RIGHT)                     # index: 0 left, 1 right
        self.assertEqual(raw[1], 100)                       # controller battery %
        self.assertEqual(struct.unpack_from("<I", raw, 4)[0], 0x1)  # buttons: A

    def test_analog_axes(self):
        for side, idx in (("l", LEFT), ("r", RIGHT)):
            with self.subTest(side):
                self.assertGreater(entry(f"{side}_trigger", idx)["trigger"], 0.99)  # f32 @0x0c
                self.assertGreater(entry(f"{side}_grip", idx)["grip"], 0.99)        # f32 @0x10
                self.assertGreater(entry(f"{side}_stick_x_max", idx)["stick"][0], 0.9)   # f32 @0x14
                self.assertLess(entry(f"{side}_stick_x_min", idx)["stick"][0], -0.9)
                self.assertGreater(entry(f"{side}_stick_y_max", idx)["stick"][1], 0.9)   # f32 @0x18
                self.assertLess(entry(f"{side}_stick_y_min", idx)["stick"][1], -0.9)

    def test_controller_mask_and_hands_mode(self):
        root, entries = decode_input(bytes.fromhex(msgs()["a"]["capnp"]))
        self.assertEqual((root["flags"], root["u16_2"]), (0x3, 0x3))   # both controllers
        root, entries = decode_input(bytes.fromhex(msgs()["hands_mode"]["capnp"]))
        self.assertEqual((root["flags"], root["u16_2"], entries), (0x7, 0, []))
        self.assertAlmostEqual(root["ipd"], 0.062, delta=0.004)


class ControllerPose(unittest.TestCase):
    """Topic 34 root pointer 0: list of {u32 index, pose (head-pose layout), status u64}."""

    def test_both_tracked(self):
        entries = decode_tracking(bytes.fromhex(msgs()["pose_both_tracked"]["capnp"]))
        self.assertEqual([(i, s) for i, _, s in entries], [(LEFT, 0xF), (RIGHT, 0xF)])
        for _, pose, _ in entries:
            self.assertAlmostEqual(math.sqrt(sum(q * q for q in pose["quat"])), 1.0, places=4)
            self.assertGreater(pose["time"], 0)

    def test_lost_when_put_down(self):
        entries = decode_tracking(bytes.fromhex(msgs()["pose_controllers_lost"]["capnp"]))
        self.assertEqual([s for _, _, s in entries], [0, 0])


class Haptics(unittest.TestCase):
    def test_app_haptics_go_over_runtime_ipc(self):
        """OpenXR xrApplyHapticFeedback (amplitude 1, 1 s, freq 0) -> RIPC frame on topic 31."""
        table, sizes, header, payload = (bytes.fromhex(p) for p in msgs()["haptic_ripc"]["parts"])
        self.assertEqual(struct.unpack("<II", table), (0, 2))
        self.assertEqual(struct.unpack("<4I", sizes), (0, 1, 32, 22))
        self.assertEqual(struct.unpack_from("<I", header, 0)[0], 0x23E)
        # Packed header: u32 kind @0, u8 @4, u32 payload size @5, u64 time @9, u8 @17, u32 seq @18, u32 hash @22
        self.assertEqual(struct.unpack_from("<I", header, 5)[0], len(payload))
        self.assertEqual(header[22:26], payload[2:6])           # type hash, also in the payload
        self.assertEqual(payload[2:6].hex(), "6967dc2a")
        self.assertIn(payload[8], (1, 2))                       # controller (1 left?, 2 right?)
        self.assertEqual(struct.unpack_from("<Q", payload, 10)[0], 1_000_000_000)  # duration ns
        self.assertEqual(struct.unpack_from("<f", payload, 18)[0], 1.0)            # amplitude

    def test_session_start_haptic_topic7(self):
        root, _ = decode(bytes.fromhex(msgs()["topic7_first"]["capnp"]))
        self.assertEqual((len(root["s"]), len(root["p"])), (3, 1))
        self.assertEqual(root["s"][1], 1)       # device id 1 (then 2, 3)
        self.assertEqual(root["s"][2], 0)       # amplitude/value 0: stop
        self.assertIsNone(root["p"][0])


def haptic_frames():
    return {f["label"]: f for f in load("sensors", "haptics")}


def bodies(frame):
    """XRSP bodies of a frame's raw messages, XRSP padding removed."""
    out = []
    for r in (bytes.fromhex(x) for x in frame["raw"]):
        b = r[8:]
        out.append(b[:-b[-1]] if r[0] & 0x08 else b)
    return out


class HapticsServer(unittest.TestCase):
    """Meta's PC opens a RuntimeIPC connection to the Quest's HapticsServer (lookup, connect with
    a PC-chosen client id) and sends each haptic as a call on it (tests/fixtures/sensors/haptics.json)."""

    def test_frames_rebuild_byte_exact(self):
        for label, f in haptic_frames().items():
            with self.subTest(label):
                b = bodies(f)
                h = f["header"]
                self.assertEqual(ripc_frame(h["kind"], h["client"], h["seq"], h["tag"], b[3]), b)

    def test_connect_targets_haptics_server(self):
        f = haptic_frames()
        self.assertEqual(f["lookup"]["header"]["kind"], RIPC_LOOKUP)
        self.assertIn(b"com.oculus.vrruntimeservice.VrRuntimeService", bodies(f["lookup"])[3])
        c = f["connect_haptics"]["header"]
        self.assertEqual(c["kind"], RIPC_CONNECT)
        self.assertIn(b"HapticsServer", bodies(f["connect_haptics"])[3])
        for n, side in ((0, 1), (1, 2)):   # SensorProbe pulsed left, then right
            h = f[f"haptic_{n}"]["header"]
            self.assertEqual((h["kind"], h["client"]), (RIPC_CALL, c["client"]))
            self.assertGreater(h["seq"], c["seq"])                               # one seq per PC request
            payload = bodies(f[f"haptic_{n}"])[3]
            self.assertEqual(payload[8], side)                                   # 1 left, 2 right
            self.assertEqual(struct.unpack_from("<Qf", payload, 10), (1_000_000_000, 1.0))


class HandsToggle(unittest.TestCase):
    def test_pc_turns_hands_on_for_hand_tracking_app(self):
        root, _ = decode(bytes.fromhex(msgs()["topic28_first"]["capnp"]))
        self.assertEqual(root["s"][0] & 0xFFFFFFFF, 0x00010001)   # type 1 (hands), value 1 (on)


if __name__ == "__main__":
    unittest.main()
