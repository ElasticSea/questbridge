using System.Collections;
using System.Collections.Generic;
using ElasticSea.QuestLink.Input;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // One guided pass once streaming starts, every step and question on the board, answers in
    // the log as "[Sequence] ..." lines. Controllers down first: answers are a raised hand
    // (left = yes, right = no) because controller input isn't decoded yet. Steps: volume up
    // with a looping tone, one AudioBench pass, questions, then either hands (full) or a
    // handover (audioOnly): the board says the video test comes next, and the native runtime
    // stops sending and logs the resume numbers so another host can attach (ql_shutdown).
    // controllersOnly: pick up the controllers, buttons, analog, motion, one buzz per controller,
    // answered with the controllers (A/X = yes, B/Y = no).
    public class BenchSequence : MonoBehaviour
    {
        const float AnswerTime = 20f, HoldTime = 1f, HandsTime = 30f, VolumeTime = 15f;

        public QuestLinkRig rig;
        public QuestLinkHands hands;
        public AudioBench bench;
        public QuestLinkAudio output;
        public TextMesh board;
        public bool audioOnly;
        public bool controllersOnly;

        readonly List<string> answers = new List<string>();

        IEnumerator Start()
        {
            board.text = "Waiting for Link...";
            while (rig.State != QuestLinkState.Streaming)
                yield return null;
            Debug.Log($"[Sequence] streaming{(audioOnly ? ", audio only" : controllersOnly ? ", controllers only" : "")}");
            if (controllersOnly)
            {
                yield return Controllers();
                board.text = "DONE\n\nTake the headset off.";
                Debug.Log($"[Sequence] done: {string.Join(", ", answers)}");
                yield break;
            }
            yield return Show("Put the controllers down.\nAnswer with your hands:\nraise LEFT hand = YES\nraise RIGHT hand = NO", 8f);
            yield return Volume();
            Debug.Log("[Sequence] audio step");
            bench.Begin();
            board.text = "AUDIO TEST\nListen, then speak when\nthe board below says so.";
            while (!bench.Done)
                yield return null;
            yield return Ask("beeps", "Did you hear\nthe beeps?");
            yield return Ask("voice", "Did you hear your own\nvoice played back?");
            if (audioOnly)
            {
                Debug.Log($"[Sequence] done: {string.Join(", ", answers)}");
                yield return Show("Audio done.\nVideo test next:\njust watch, keep the\nheadset on.", 3f);
                Handover();
                yield break;
            }
            yield return Hands();
            yield return Ask("spheres", "Did the spheres follow\nyour fingers?");
            board.text = "DONE\n\nTake the headset off.";
            Debug.Log($"[Sequence] done: {string.Join(", ", answers)}");
        }

        // Leave the headset's session to the next host (mac-m3's attach runs), then quit
        void Handover()
        {
            board.text = "Video test next:\njust watch, keep the\nheadset on.";
            if (rig.Backend is NativeQuestLink)
                NativeQuestLink.Shutdown();
            Debug.Log("[Sequence] handover: native sending stopped, quitting");
            Application.Quit();
        }

        IEnumerator Show(string text, float seconds)
        {
            for (var end = Time.realtimeSinceStartup + seconds; Time.realtimeSinceStartup < end;)
            {
                board.text = $"{text}\n({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})";
                yield return null;
            }
        }

        // Looping tone while the user turns the headset volume up; the bar is what we send
        IEnumerator Volume()
        {
            Debug.Log("[Sequence] volume step");
            var peak = 0f;
            bench.Tone(true);
            for (var end = Time.realtimeSinceStartup + VolumeTime; Time.realtimeSinceStartup < end;)
            {
                var level = output != null ? output.TakePeak() : 0f;
                peak = Mathf.Max(peak, level);
                board.text = $"VOLUME UP NOW ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nPress the volume + button\n(headset underside, right)\nuntil you hear the tone.\nsending {AudioBench.Bar(level)}";
                yield return null;
            }
            bench.Tone(false);
            Debug.Log($"[Sequence] volume step: send peak {peak:F3}");
        }

        IEnumerator Hands()
        {
            Debug.Log("[Sequence] hands step");
            int frames = 0, left = 0, right = 0;
            float maxL = 0f, maxR = 0f;
            for (var end = Time.realtimeSinceStartup + HandsTime; Time.realtimeSinceStartup < end;)
            {
                frames++;
                if (hands.Left.tracked) left++;
                if (hands.Right.tracked) right++;
                var pl = Pinch(hands.Left);
                var pr = Pinch(hands.Right);
                maxL = Mathf.Max(maxL, pl);
                maxR = Mathf.Max(maxR, pr);
                board.text = $"HANDS ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nHands up, open and close.\nPinch FIRMLY (thumb on index):\n" +
                             $"left {Hand(hands.Left, pl)}   right {Hand(hands.Right, pr)}";
                yield return null;
            }
            Debug.Log($"[Sequence] hands: {frames} frames, left tracked {left}, right tracked {right}, max pinch L {maxL:F2} R {maxR:F2}");
        }

        const float PickUpTime = 15f, ButtonsTime = 20f, AnalogTime = 12f, MotionTime = 8f;

        QuestLinkControllerState Pad(QuestLinkHand h) =>
            rig.Backend != null && rig.Backend.TryGetController(h, out var s) ? s : default;

        IEnumerator Controllers()
        {
            Debug.Log("[Sequence] controllers step");
            for (var end = Time.realtimeSinceStartup + PickUpTime; Time.realtimeSinceStartup < end;)
            {
                bool l = Pad(QuestLinkHand.Left).connected, r = Pad(QuestLinkHand.Right).connected;
                if (l && r)
                    break;
                board.text = $"CONTROLLER TEST ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nPick up both controllers.\nleft {(l ? "OK" : "--")}   right {(r ? "OK" : "--")}";
                yield return null;
            }
            Debug.Log($"[Sequence] controllers connected: left {Pad(QuestLinkHand.Left).connected}, right {Pad(QuestLinkHand.Right).connected}");

            // Every button once; the list shrinks as they are pressed
            var pending = new List<string> { "A", "B", "X", "Y", "L-stick", "R-stick", "Menu" };
            for (var end = Time.realtimeSinceStartup + ButtonsTime; Time.realtimeSinceStartup < end && pending.Count > 0;)
            {
                var l = Pad(QuestLinkHand.Left);
                var r = Pad(QuestLinkHand.Right);
                if (r.primaryButton) pending.Remove("A");
                if (r.secondaryButton) pending.Remove("B");
                if (l.primaryButton) pending.Remove("X");
                if (l.secondaryButton) pending.Remove("Y");
                if (l.thumbstickClick) pending.Remove("L-stick");
                if (r.thumbstickClick) pending.Remove("R-stick");
                if (l.menu) pending.Remove("Menu");
                board.text = $"BUTTONS ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nPress each once:\n{string.Join("  ", pending)}";
                yield return null;
            }
            var missing = pending.Count == 0 ? "none" : string.Join("+", pending);
            answers.Add($"buttons_missing={missing}");
            Debug.Log($"[Sequence] buttons not seen: {missing}");

            var max = new float[8];  // trigger, grip, |stick x|, |stick y| per hand
            for (var end = Time.realtimeSinceStartup + AnalogTime; Time.realtimeSinceStartup < end;)
            {
                var l = Pad(QuestLinkHand.Left);
                var r = Pad(QuestLinkHand.Right);
                Track(max, 0, l);
                Track(max, 4, r);
                board.text = $"ANALOG ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nSqueeze triggers + grips fully,\ncircle both sticks.\nL {Analog(l)}\nR {Analog(r)}";
                yield return null;
            }
            Debug.Log($"[Sequence] analog max L trig {max[0]:F2} grip {max[1]:F2} sx {max[2]:F2} sy {max[3]:F2}, R trig {max[4]:F2} grip {max[5]:F2} sx {max[6]:F2} sy {max[7]:F2}");

            int frames = 0, tracked = 0;
            var lo = Vector3.one * 1e9f;
            var hi = -lo;
            for (var end = Time.realtimeSinceStartup + MotionTime; Time.realtimeSinceStartup < end;)
            {
                frames++;
                var r = Pad(QuestLinkHand.Right);
                if (r.positionTracked)
                {
                    tracked++;
                    lo = Vector3.Min(lo, r.pose.position);
                    hi = Vector3.Max(hi, r.pose.position);
                }
                var at = r.positionTracked ? r.pose.position.ToString("F2") : "not tracked";
                board.text = $"MOTION ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nWave the RIGHT controller\nup, down, left, right.\n{at}";
                yield return null;
            }
            Debug.Log($"[Sequence] motion: {tracked}/{frames} frames tracked, right span {(tracked > 0 ? (hi - lo).ToString("F2") : "-")}");

            foreach (var hand in new[] { QuestLinkHand.Left, QuestLinkHand.Right })
            {
                var name = hand == QuestLinkHand.Left ? "LEFT" : "RIGHT";
                // Through the Input System, as XRI sends it
                var device = hand == QuestLinkHand.Left ? QuestLinkInput.LeftController : QuestLinkInput.RightController;
                Debug.Log($"[Sequence] haptic {name}");
                device?.SendImpulse(1f, 1f);
                yield return Show($"{name} controller\nbuzzes now (1 s)", 2f);
                yield return AskButtons($"buzz_{name.ToLowerInvariant()}", $"Did the {name}\ncontroller buzz?");
            }
        }

        static string Analog(in QuestLinkControllerState s) =>
            $"{s.trigger:F2} {s.grip:F2} {s.thumbstick.x:+0.0;-0.0} {s.thumbstick.y:+0.0;-0.0}";

        static void Track(float[] max, int at, in QuestLinkControllerState s)
        {
            max[at] = Mathf.Max(max[at], s.trigger);
            max[at + 1] = Mathf.Max(max[at + 1], s.grip);
            max[at + 2] = Mathf.Max(max[at + 2], Mathf.Abs(s.thumbstick.x));
            max[at + 3] = Mathf.Max(max[at + 3], Mathf.Abs(s.thumbstick.y));
        }

        // A or X = yes, B or Y = no; buttons must be released first
        IEnumerator AskButtons(string key, string question)
        {
            var armed = false;
            for (var end = Time.realtimeSinceStartup + AnswerTime; Time.realtimeSinceStartup < end;)
            {
                var l = Pad(QuestLinkHand.Left);
                var r = Pad(QuestLinkHand.Right);
                bool yes = l.primaryButton || r.primaryButton, no = l.secondaryButton || r.secondaryButton;
                if (!yes && !no)
                    armed = true;
                else if (armed && yes != no)
                {
                    var answer = yes ? "YES" : "NO";
                    answers.Add($"{key}={answer}");
                    Debug.Log($"[Sequence] answer {key}={answer}");
                    yield return Show($"{question.Replace('\n', ' ')}\n{answer}", 1.5f);
                    yield break;
                }
                board.text = $"{question} ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nA or X = YES\nB or Y = NO";
                yield return null;
            }
            answers.Add($"{key}=none");
            Debug.Log($"[Sequence] answer {key}=none (timeout)");
        }

        static string Hand(QuestLinkHandState h, float pinch) => h.tracked ? $"pinch {pinch:F2}" : "--";

        static float Pinch(QuestLinkHandState h) => h.tracked ? h.Pinch(QuestLinkFinger.Index) : 0f;

        // Wait for both hands down, then a raised hand held for HoldTime
        IEnumerator Ask(string key, string question)
        {
            var armed = false;
            var held = 0f;
            QuestLinkHand? holding = null;
            for (var end = Time.realtimeSinceStartup + AnswerTime; Time.realtimeSinceStartup < end;)
            {
                var l = Raised(hands.Left);
                var r = Raised(hands.Right);
                if (!l && !r)
                    armed = true;
                QuestLinkHand? now = armed && l != r ? (l ? QuestLinkHand.Left : QuestLinkHand.Right) : (QuestLinkHand?)null;
                held = now != null && now == holding ? held + Time.unscaledDeltaTime : 0f;
                holding = now;
                if (held >= HoldTime)
                {
                    var answer = holding == QuestLinkHand.Left ? "YES" : "NO";
                    answers.Add($"{key}={answer}");
                    Debug.Log($"[Sequence] answer {key}={answer}");
                    yield return Show($"{question.Replace('\n', ' ')}\n{answer}\n\nhands down", 2f);
                    yield break;
                }
                var hint = !armed ? "put both hands down first" : holding != null ? $"keep it up... {(holding == QuestLinkHand.Left ? "YES" : "NO")}" : "";
                board.text = $"{question} ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nLEFT hand up = YES\nRIGHT hand up = NO\n{hint}";
                yield return null;
            }
            answers.Add($"{key}=none");
            Debug.Log($"[Sequence] answer {key}=none (timeout)");
        }

        // Wrist above the head (both in tracking space)
        bool Raised(QuestLinkHandState h) =>
            h.tracked && rig.Head != null && h[QuestLinkHandJoint.Wrist].position.y > rig.Head.localPosition.y;
    }
}
