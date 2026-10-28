using System.Collections;
using System.Collections.Generic;
using UnityEngine;

namespace ElasticSea.QuestBridge.BridgeTest
{
    // "-sequence-recovery": the live recovery check (docs/testing.md), about one
    // minute, every step on the board. The player keeps running; tools/recovery_check.py reads
    // the "[Recovery] drop N <kind>" log lines and disturbs the headset over adb (sleep, Link
    // quit). Each drop must come back to streaming without restarting anything; the user presses
    // Launch when the Link dialog shows it. Every step waits for A to continue; the question at
    // the end waits for A (yes) or B (no), with no timeout.
    public class RecoveryCheck : MonoBehaviour
    {
        const float LossWait = 20f, BackWait = 60f;
        static readonly string[] Drops = { "sleep", "linkquit" };

        public QuestBridgeRig rig;
        public QuestBridgeConnectionStatus status;
        public TextMesh board;

        readonly List<string> results = new List<string>();

        IEnumerator Start()
        {
            board.text = "Waiting for Link...";
            while (rig.State != QuestBridgeState.Streaming)
                yield return null;
            Debug.Log("[Recovery] streaming");
            yield return Continue("RECOVERY TEST (1 min)\nKeep the headset on.\nThe picture goes away twice.\nWhen the Link dialog shows\nLaunch, press Launch.");
            for (var i = 0; i < Drops.Length; i++)
                yield return Drop(i + 1, Drops[i]);
            yield return Ask("came_back", "Did the picture come back\nevery time?");
            Debug.Log($"[Recovery] done: {string.Join(", ", results)}");
            // In a bench slot (tools/bench_slot.ps1) "-look-next" names the next part: hand over and quit
            var args = System.Environment.GetCommandLineArgs();
            var at = System.Array.IndexOf(args, "-look-next");
            if (at < 0 || at + 1 >= args.Length)
            {
                board.text = "DONE\n\nTake the headset off.";
                yield break;
            }
            var next = args[at + 1].Replace('|', '\n');
            yield return Continue($"Recovery test done.\nNEXT: {next}");
            board.text = next;
            yield return new WaitForSecondsRealtime(1f);
            if (rig.Backend is NativeQuestBridge)
                NativeQuestBridge.Shutdown();
            Application.Quit();
        }

        IEnumerator Drop(int n, string kind)
        {
            var hint = kind == "sleep" ? "The screen turns off,\nthen back on by itself." : "Link closes.\nPress Launch when you see it.";
            yield return Continue($"DROP {n} of {Drops.Length}\n{hint}\nIt starts when you press A.");
            var before = status.Status;
            var start = Time.realtimeSinceStartup;
            Debug.Log($"[Recovery] drop {n} {kind}");
            board.text = $"DROP {n}: now\n{hint}";
            while (status.Status.disconnects == before.disconnects && Time.realtimeSinceStartup - start < LossWait)
                yield return null;
            if (status.Status.disconnects == before.disconnects)
            {
                // Some drops leave the stream up (a short sleep): that's a pass too
                results.Add($"{kind}=kept");
                Debug.Log($"[Recovery] back {n} {kind}: stream kept, no loss in {LossWait:F0} s");
                yield return Continue($"DROP {n}: picture stayed.");
                yield break;
            }
            var lostAt = Time.realtimeSinceStartup;
            while (status.Status.state != QuestBridgeLinkState.Streaming && Time.realtimeSinceStartup - lostAt < BackWait)
                yield return null;
            var s = status.Status;
            var back = s.state == QuestBridgeLinkState.Streaming;
            var took = Time.realtimeSinceStartup - lostAt;
            results.Add($"{kind}={(back ? $"back_{took:F1}s" : "not_back")}");
            Debug.Log($"[Recovery] back {n} {kind}: {(back ? "streaming" : "NOT back")} {took:F1} s after the loss " +
                      $"({s.lastLoss}; connects {s.connects}, disconnects {s.disconnects}, resumes {s.resumes})");
            yield return Continue(back ? $"BACK after {took:F1} s.\nDrop {n} recovered." : $"Drop {n}: no picture\nafter {BackWait:F0} s.");
        }

        // A / X and B / Y on either controller
        void Buttons(out bool a, out bool b)
        {
            a = b = false;
            foreach (var h in new[] { QuestBridgeHand.Left, QuestBridgeHand.Right })
                if (rig.Backend != null && rig.Backend.TryGetController(h, out var c))
                {
                    a |= c.primaryButton;
                    b |= c.secondaryButton;
                }
        }

        // All buttons up first, so one press answers one step
        IEnumerator Released()
        {
            for (;;)
            {
                Buttons(out var a, out var b);
                if (!a && !b)
                    yield break;
                yield return null;
            }
        }

        // Shows text until A is pressed
        IEnumerator Continue(string text)
        {
            board.text = $"{text}\n\nPress A to continue.";
            yield return Released();
            for (;;)
            {
                Buttons(out var a, out _);
                if (a)
                    yield break;
                yield return null;
            }
        }

        // Waits for A (yes) or B (no)
        IEnumerator Ask(string key, string question)
        {
            board.text = $"{question}\nA / X = YES\nB / Y = NO";
            yield return Released();
            for (;;)
            {
                Buttons(out var yes, out var no);
                if (yes != no)
                {
                    var answer = yes ? "YES" : "NO";
                    results.Add($"{key}={answer}");
                    Debug.Log($"[Recovery] answer {key}={answer}");
                    yield return Continue($"{question.Replace('\n', ' ')}\n{answer}");
                    yield break;
                }
                yield return null;
            }
        }
    }
}
