using System.Collections;
using System.Collections.Generic;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // One guided pass once streaming starts, every step and question on the board, answers in
    // the log as "[Sequence] ..." lines. Controllers down first: answers are a raised hand
    // (left = yes, right = no) because controller input isn't decoded yet. Steps: volume up
    // with a looping tone, one AudioBench pass, questions, then either hands (full) or a
    // handover (audioOnly): the board says the video test comes next, and the native runtime
    // stops sending and logs the resume numbers so another host can attach (ql_shutdown).
    public class BenchSequence : MonoBehaviour
    {
        const float AnswerTime = 20f, HoldTime = 1f, HandsTime = 30f, VolumeTime = 15f;

        public QuestLinkRig rig;
        public QuestLinkHands hands;
        public AudioBench bench;
        public QuestLinkAudio output;
        public TextMesh board;
        public bool audioOnly;

        readonly List<string> answers = new List<string>();

        IEnumerator Start()
        {
            board.text = "Waiting for Link...";
            while (rig.State != QuestLinkState.Streaming)
                yield return null;
            Debug.Log($"[Sequence] streaming{(audioOnly ? ", audio only" : "")}");
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
