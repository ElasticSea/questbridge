using System.Collections;
using System.Collections.Generic;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // "-sequence-recovery": the live recovery check (docs/RECOVERY_LIVE_CHECK.md), about one
    // minute, every step on the board. The player keeps running; tools/recovery_check.py reads
    // the "[Recovery] drop N <kind>" log lines and disturbs the headset over adb (sleep, Link
    // quit). Each drop must come back to streaming without restarting anything; the user only
    // presses Launch when the Link dialog shows it. One question at the end (A yes / B no).
    public class RecoveryCheck : MonoBehaviour
    {
        const float LossWait = 20f, BackWait = 60f, AnswerTime = 20f;
        static readonly string[] Drops = { "sleep", "linkquit" };

        public QuestLinkRig rig;
        public QuestLinkConnectionStatus status;
        public TextMesh board;

        readonly List<string> results = new List<string>();

        IEnumerator Start()
        {
            board.text = "Waiting for Link...";
            while (rig.State != QuestLinkState.Streaming)
                yield return null;
            Debug.Log("[Recovery] streaming");
            yield return Show("RECOVERY TEST (1 min)\nKeep the headset on.\nThe picture goes away twice.\nWhen the Link dialog shows\nLaunch, press Launch.", 8f);
            for (var i = 0; i < Drops.Length; i++)
                yield return Drop(i + 1, Drops[i]);
            yield return Ask("came_back", "Did the picture come back\nevery time?");
            board.text = "DONE\n\nTake the headset off.";
            Debug.Log($"[Recovery] done: {string.Join(", ", results)}");
        }

        IEnumerator Drop(int n, string kind)
        {
            var hint = kind == "sleep" ? "The screen turns off,\nthen back on by itself." : "Link closes.\nPress Launch when you see it.";
            yield return Show($"DROP {n} of {Drops.Length}\n{hint}\nStarting in", 4f);
            var before = status.Status;
            var start = Time.realtimeSinceStartup;
            Debug.Log($"[Recovery] drop {n} {kind}");
            board.text = $"DROP {n}: now\n{hint}";
            while (status.Status.disconnects == before.disconnects && Time.realtimeSinceStartup - start < LossWait)
                yield return null;
            var lost = status.Status.disconnects > before.disconnects;
            if (!lost)
            {
                // Some drops leave the stream up (a short sleep): that's a pass too
                results.Add($"{kind}=kept");
                Debug.Log($"[Recovery] back {n} {kind}: stream kept, no loss in {LossWait:F0} s");
                yield return Show($"DROP {n}: picture stayed.", 3f);
                yield break;
            }
            var lostAt = Time.realtimeSinceStartup;
            while (status.Status.state != QuestLinkLinkState.Streaming && Time.realtimeSinceStartup - lostAt < BackWait)
                yield return null;
            var s = status.Status;
            var back = s.state == QuestLinkLinkState.Streaming;
            var took = Time.realtimeSinceStartup - lostAt;
            results.Add($"{kind}={(back ? $"back_{took:F1}s" : "not_back")}");
            Debug.Log($"[Recovery] back {n} {kind}: {(back ? "streaming" : "NOT back")} {took:F1} s after the loss " +
                      $"({s.lastLoss}; connects {s.connects}, disconnects {s.disconnects}, resumes {s.resumes})");
            yield return Show(back ? $"BACK after {took:F1} s.\nDrop {n} recovered." : $"Drop {n}: no picture\nafter {BackWait:F0} s.", 3f);
        }

        IEnumerator Show(string text, float seconds)
        {
            for (var end = Time.realtimeSinceStartup + seconds; Time.realtimeSinceStartup < end;)
            {
                board.text = $"{text}\n({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})";
                yield return null;
            }
        }

        // A = yes, B = no on either controller (X / Y on the left)
        IEnumerator Ask(string key, string question)
        {
            for (var end = Time.realtimeSinceStartup + AnswerTime; Time.realtimeSinceStartup < end;)
            {
                bool yes = false, no = false;
                foreach (var h in new[] { QuestLinkHand.Left, QuestLinkHand.Right })
                    if (rig.Backend != null && rig.Backend.TryGetController(h, out var c))
                    {
                        yes |= c.primaryButton;
                        no |= c.secondaryButton;
                    }
                if (yes != no)
                {
                    var answer = yes ? "YES" : "NO";
                    results.Add($"{key}={answer}");
                    Debug.Log($"[Recovery] answer {key}={answer}");
                    yield return Show($"{question.Replace('\n', ' ')}\n{answer}", 2f);
                    yield break;
                }
                board.text = $"{question} ({Mathf.CeilToInt(end - Time.realtimeSinceStartup)})\nA / X = YES\nB / Y = NO";
                yield return null;
            }
            results.Add($"{key}=none");
            Debug.Log($"[Recovery] answer {key}=none (timeout)");
        }
    }
}
