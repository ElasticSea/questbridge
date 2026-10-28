using System.Collections;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // "-sequence-look": one bench part with a single question about the picture (launcher focus
    // check, encoder fallbacks; tools/bench_slot.ps1). "-look-label <text>" names the part and
    // "-look-next <text>" says what comes after it ("|" = new line). Every step waits for A, the
    // question for A (yes) or B (no), no timeouts. Then the native session is shut down and the
    // player quits, so the next part's host can take over.
    public class LookCheck : MonoBehaviour
    {
        public QuestLinkRig rig;
        public TextMesh board;

        IEnumerator Start()
        {
            var label = Arg("-look-label", "PICTURE CHECK");
            var next = Arg("-look-next", "Take the headset off.");
            board.text = "Waiting for Link...";
            while (rig.State != QuestLinkState.Streaming)
                yield return null;
            Debug.Log($"[Look] streaming: {label.Replace('\n', ' ')}");
            yield return Continue($"{label}\nLook around for a few seconds:\nfloor grid and cubes.");
            var ok = false;
            yield return Ask("Does the picture look OK?\n(sharp, steady, no glitches)", a => ok = a);
            Debug.Log($"[Look] answer ok={(ok ? "YES" : "NO")}");
            yield return Continue($"Thanks.\nNEXT: {next}");
            board.text = next;
            yield return new WaitForSecondsRealtime(1f);
            if (rig.Backend is NativeQuestLink)
                NativeQuestLink.Shutdown();
            Debug.Log("[Look] done, quitting");
            Application.Quit();
        }

        static string Arg(string name, string fallback)
        {
            var args = System.Environment.GetCommandLineArgs();
            var i = System.Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1].Replace('|', '\n') : fallback;
        }

        // A / X and B / Y on either controller
        void Buttons(out bool a, out bool b)
        {
            a = b = false;
            foreach (var h in new[] { QuestLinkHand.Left, QuestLinkHand.Right })
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
        IEnumerator Ask(string question, System.Action<bool> answer)
        {
            board.text = $"{question}\nA / X = YES\nB / Y = NO";
            yield return Released();
            for (;;)
            {
                Buttons(out var yes, out var no);
                if (yes != no)
                {
                    answer(yes);
                    yield break;
                }
                yield return null;
            }
        }
    }
}
