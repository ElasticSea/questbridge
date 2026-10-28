// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using System.Collections.Generic;
using ElasticSea.QuestBridge.Input;
using UnityEngine;

namespace ElasticSea.QuestBridge.BridgeTest
{
    // One guided pass once streaming starts, every step and question on the board, answers in
    // the log as "[Sequence] ..." lines. Controllers down first: answers are a raised hand
    // (left = yes, right = no) because controller input isn't decoded yet. Steps: volume up
    // with a looping tone, one AudioBench pass, questions, then either hands (full) or a
    // handover (audioOnly): the board says the video test comes next, and the native runtime
    // stops sending and logs the resume numbers so another host can attach (qb_shutdown).
    // controllersOnly: pick up the controllers, buttons, analog, motion, one buzz per controller,
    // answered with the controllers (A/X = yes, B/Y = no).
    public class BenchSequence : MonoBehaviour
    {
        const float AnswerTime = 20f, HoldTime = 1f, HandsTime = 30f, VolumeTime = 15f;

        public QuestBridgeRig rig;
        public QuestBridgeHands hands;
        public AudioBench bench;
        public QuestBridgeAudio output;
        public TextMesh board;
        public bool audioOnly;
        public bool controllersOnly;

        readonly List<string> answers = new List<string>();

        IEnumerator Start()
        {
            board.text = "Waiting for Link...";
            while (rig.State != QuestBridgeState.Streaming)
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
            if (rig.Backend is NativeQuestBridge)
                NativeQuestBridge.Shutdown();
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

        QuestBridgeControllerState Pad(QuestBridgeHand h) =>
            rig.Backend != null && rig.Backend.TryGetController(h, out var s) ? s : default;

        IEnumerator Controllers()
        {
            Debug.Log("[Sequence] controllers step");
            for (var end = Time.realtimeSinceStartup + PickUpTime; Time.realtimeSinceStartup < end;)
            {
                bool l = Pad(QuestBridgeHand.Left).connected, r = Pad(QuestBridgeHand.Right).connected;
                if (l && r)
                    break;
                board.text = $"CONTROLLER TEST ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nPick up both controllers.\nleft {(l ? "OK" : "--")}   right {(r ? "OK" : "--")}\nNot OK? Press its Meta button\nor pull its trigger.";
                yield return null;
            }
            bool hasL = Pad(QuestBridgeHand.Left).connected, hasR = Pad(QuestBridgeHand.Right).connected;
            Debug.Log($"[Sequence] controllers connected: left {hasL}, right {hasR}");
            answers.Add($"controllers={(hasL ? "L" : "")}{(hasR ? "R" : "")}");
            // A missing controller (dead battery, asleep) is skipped; the other one answers
            if (!hasL && !hasR)
            {
                yield return Show("No controller detected,\nskipping the controller test.", 4f);
                yield break;
            }
            if (!hasL || !hasR)
                yield return Show($"{(hasL ? "RIGHT" : "LEFT")} controller not detected,\nskipping it.", 4f);

            // Every button once; the list shrinks as they are pressed
            var pending = new List<string>();
            if (hasR) pending.AddRange(new[] { "A", "B", "R-stick" });
            if (hasL) pending.AddRange(new[] { "X", "Y", "L-stick", "Menu" });
            for (var end = Time.realtimeSinceStartup + ButtonsTime; Time.realtimeSinceStartup < end && pending.Count > 0;)
            {
                var l = Pad(QuestBridgeHand.Left);
                var r = Pad(QuestBridgeHand.Right);
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
                var l = Pad(QuestBridgeHand.Left);
                var r = Pad(QuestBridgeHand.Right);
                Track(max, 0, l);
                Track(max, 4, r);
                board.text = $"ANALOG ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nSqueeze triggers + grips fully,\ncircle both sticks.\nL {Analog(l)}\nR {Analog(r)}";
                yield return null;
            }
            Debug.Log($"[Sequence] analog max L trig {max[0]:F2} grip {max[1]:F2} sx {max[2]:F2} sy {max[3]:F2}, R trig {max[4]:F2} grip {max[5]:F2} sx {max[6]:F2} sy {max[7]:F2}");

            int frames = 0, tracked = 0;
            var lo = Vector3.one * 1e9f;
            var hi = -lo;
            var moving = hasR ? QuestBridgeHand.Right : QuestBridgeHand.Left;
            var movingName = hasR ? "RIGHT" : "LEFT";
            for (var end = Time.realtimeSinceStartup + MotionTime; Time.realtimeSinceStartup < end;)
            {
                frames++;
                var c = Pad(moving);
                if (c.positionTracked)
                {
                    tracked++;
                    lo = Vector3.Min(lo, c.pose.position);
                    hi = Vector3.Max(hi, c.pose.position);
                }
                var at = c.positionTracked ? c.pose.position.ToString("F2") : "not tracked";
                board.text = $"MOTION ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nWave the {movingName} controller\nup, down, left, right.\n{at}";
                yield return null;
            }
            Debug.Log($"[Sequence] motion: {tracked}/{frames} frames tracked, {movingName.ToLowerInvariant()} span {(tracked > 0 ? (hi - lo).ToString("F2") : "-")}");

            foreach (var hand in new[] { QuestBridgeHand.Left, QuestBridgeHand.Right })
            {
                var name = hand == QuestBridgeHand.Left ? "LEFT" : "RIGHT";
                if (!(hand == QuestBridgeHand.Left ? hasL : hasR))
                {
                    answers.Add($"buzz_{name.ToLowerInvariant()}=skipped");
                    Debug.Log($"[Sequence] haptic {name} skipped (not detected)");
                    continue;
                }
                // Through the Input System, as XRI sends it
                var device = hand == QuestBridgeHand.Left ? QuestBridgeInput.LeftController : QuestBridgeInput.RightController;
                Debug.Log($"[Sequence] haptic {name}");
                device?.SendImpulse(1f, 1f);
                yield return Show($"{name} controller\nbuzzes now (1 s)", 2f);
                // Answered on the other controller: the one under test may be the broken one
                var other = hand == QuestBridgeHand.Left ? QuestBridgeHand.Right : QuestBridgeHand.Left;
                yield return AskButtons($"buzz_{name.ToLowerInvariant()}", $"Did the {name}\ncontroller buzz?", other);
            }
        }

        static string Analog(in QuestBridgeControllerState s) =>
            $"{s.trigger:F2} {s.grip:F2} {s.thumbstick.x:+0.0;-0.0} {s.thumbstick.y:+0.0;-0.0}";

        static void Track(float[] max, int at, in QuestBridgeControllerState s)
        {
            max[at] = Mathf.Max(max[at], s.trigger);
            max[at + 1] = Mathf.Max(max[at + 1], s.grip);
            max[at + 2] = Mathf.Max(max[at + 2], Mathf.Abs(s.thumbstick.x));
            max[at + 3] = Mathf.Max(max[at + 3], Mathf.Abs(s.thumbstick.y));
        }

        // Answer on controller `with` (A/X = yes, B/Y = no; buttons released first) or, without
        // it, a raised hand held for HoldTime (left = yes, right = no)
        IEnumerator AskButtons(string key, string question, QuestBridgeHand with)
        {
            var armed = false;
            var held = 0f;
            bool? holding = null;
            var buttons = !Pad(with).connected ? "(other controller not detected)"
                : with == QuestBridgeHand.Right ? "RIGHT controller: A = YES, B = NO" : "LEFT controller: X = YES, Y = NO";
            for (var end = Time.realtimeSinceStartup + AnswerTime; Time.realtimeSinceStartup < end;)
            {
                var c = Pad(with);
                bool yes = c.primaryButton, no = c.secondaryButton;
                bool l = Raised(hands.Left), r = Raised(hands.Right);
                bool? raise = l != r ? l : (bool?)null;
                held = raise != null && raise == holding ? held + Time.unscaledDeltaTime : 0f;
                holding = raise;
                if (held >= HoldTime)
                {
                    yes = raise == true;
                    no = raise == false;
                }
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
                board.text = $"{question} ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\n{buttons}\nor raise a hand:\nLEFT = YES, RIGHT = NO";
                yield return null;
            }
            answers.Add($"{key}=none");
            Debug.Log($"[Sequence] answer {key}=none (timeout)");
        }

        static string Hand(QuestBridgeHandState h, float pinch) => h.tracked ? $"pinch {pinch:F2}" : "--";

        static float Pinch(QuestBridgeHandState h) => h.tracked ? h.Pinch(QuestBridgeFinger.Index) : 0f;

        // Wait for both hands down, then a raised hand held for HoldTime
        IEnumerator Ask(string key, string question)
        {
            var armed = false;
            var held = 0f;
            QuestBridgeHand? holding = null;
            for (var end = Time.realtimeSinceStartup + AnswerTime; Time.realtimeSinceStartup < end;)
            {
                var l = Raised(hands.Left);
                var r = Raised(hands.Right);
                if (!l && !r)
                    armed = true;
                QuestBridgeHand? now = armed && l != r ? (l ? QuestBridgeHand.Left : QuestBridgeHand.Right) : (QuestBridgeHand?)null;
                held = now != null && now == holding ? held + Time.unscaledDeltaTime : 0f;
                holding = now;
                if (held >= HoldTime)
                {
                    var answer = holding == QuestBridgeHand.Left ? "YES" : "NO";
                    answers.Add($"{key}={answer}");
                    Debug.Log($"[Sequence] answer {key}={answer}");
                    yield return Show($"{question.Replace('\n', ' ')}\n{answer}\n\nhands down", 2f);
                    yield break;
                }
                var hint = !armed ? "put both hands down first" : holding != null ? $"keep it up... {(holding == QuestBridgeHand.Left ? "YES" : "NO")}" : "";
                board.text = $"{question} ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nLEFT hand up = YES\nRIGHT hand up = NO\n{hint}";
                yield return null;
            }
            answers.Add($"{key}=none");
            Debug.Log($"[Sequence] answer {key}=none (timeout)");
        }

        // Wrist above the head (both in tracking space)
        bool Raised(QuestBridgeHandState h) =>
            h.tracked && rig.Head != null && h[QuestBridgeHandJoint.Wrist].position.y > rig.Head.localPosition.y;
    }
}
