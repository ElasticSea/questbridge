"""tools/recovery_check.py: which adb steps each player log line triggers, and the verdict"""
import unittest

import helpers  # noqa: F401 (puts tools/ on the path)
import recovery_check as rc


class RecoveryCheckTest(unittest.TestCase):
    def test_drop_lines_map_to_adb_steps(self):
        sleep = rc.actions_for("12.3 [Recovery] drop 1 sleep")
        self.assertEqual(sleep[0], ("shell", "input keyevent KEYCODE_SLEEP"))
        self.assertEqual(sleep[1], ("wait", 6))
        self.assertIn("prox_close", sleep[2][1])
        self.assertEqual(rc.actions_for("[Recovery] drop 2 linkquit"),
                         [("shell", "am force-stop com.oculus.xrstreamingclient")])
        self.assertEqual(rc.actions_for("[Recovery] back 2 linkquit: streaming 4.1 s"), [])
        self.assertEqual(rc.actions_for("[Recovery] drop 3 reboot"), [])  # never reboot the headset

    def test_verdict(self):
        ok, results = rc.verdict("sleep=kept, linkquit=back_4.2s, came_back=YES")
        self.assertTrue(ok)
        self.assertEqual(results["linkquit"], "back_4.2s")
        self.assertFalse(rc.verdict("sleep=back_2.0s, linkquit=not_back, came_back=YES")[0])
        self.assertFalse(rc.verdict("sleep=kept, linkquit=back_3.0s, came_back=NO")[0])
        self.assertFalse(rc.verdict("sleep=kept, linkquit=back_3.0s, came_back=none")[0])


if __name__ == "__main__":
    unittest.main()
