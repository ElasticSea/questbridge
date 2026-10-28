// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using UnityEngine;

namespace ElasticSea.QuestBridge.BridgeTest
{
    // Editor Play bench scene (EditorPlay.unity): floor, cubes and a board with the round's steps
    // and yes/no questions (A/X = yes, B/Y = no). Nothing times out: each step waits for A/X, each
    // question for A/X or B/Y. Answers go to the Console as "[EditorPlay] ...". Press Play once: after round 1 the board
    // stops Play itself and Editor/EditorPlayBench.cs starts it again 3 s later for round 2.
    public class EditorPlayBoard : MonoBehaviour
    {
        public QuestBridgeRig rig;
        public QuestBridgeHands hands;
        public float standingHeight = 1.6f;

        public const string Key = "EditorPlayBench.";
        public static int Round => Get("round", 1);
        public static bool Done { get; private set; }

        TextMesh board;
        bool recentred;

        void Awake()
        {
            Done = false;
            var light = new GameObject("Sun").AddComponent<Light>();
            light.type = LightType.Directional;
            light.transform.rotation = Quaternion.Euler(50, -30, 0);
            FirstLightScene.Floor();
            FirstLightScene.Cube(new Vector3(0f, 0.25f, 1.5f), 0.5f, new Color(0.9f, 0.3f, 0.25f));
            FirstLightScene.Cube(new Vector3(-1.2f, 0.4f, 2.2f), 0.8f, new Color(0.25f, 0.7f, 0.35f));
            FirstLightScene.Cube(new Vector3(1.3f, 0.3f, 2.0f), 0.6f, new Color(0.25f, 0.45f, 0.9f));
            FirstLightScene.Cube(new Vector3(2.2f, 0.5f, -0.5f), 1.0f, new Color(0.8f, 0.4f, 0.85f));
            board = FirstLightScene.Text("", new Vector3(0f, 2.0f, 3.2f), 1.7f);
        }

        IEnumerator Start()
        {
            // Dry run of the bench, after the rig's OnEnable (unity/run-editorplay.ps1 -Mock)
            if (System.Environment.GetEnvironmentVariable("QB_BENCH_MOCK") == "1")
                rig.Initialize(new MockQuestBridge());
            var round = Round;
            var t0 = Time.realtimeSinceStartup;
            Debug.Log($"[EditorPlay] round {round}: Play, backend {rig.Backend?.Name}");
            while (rig.State != QuestBridgeState.Streaming)
                yield return null;
            Debug.Log($"[EditorPlay] round {round}: streaming {Time.realtimeSinceStartup - t0:F1} s after Play");

            if (round == 1)
            {
                yield return Step("UNITY EDITOR PLAY TEST\nRound 1 of 2\nLook around: cubes\nshould stay still.\nHold the controllers up.");
                yield return Ask("still1", "Did the cubes stay still\nas you looked around?");
                yield return Ask("blocks", "Did a small block follow\neach controller?");
                yield return Step("Unity now stops Play\nand starts it again.\nKeep the headset on.\nIf Meta's Link screen\nappears, press Launch.");
            }
            else
            {
                yield return Step("Round 2 of 2\nPlay started again.\nLook around.");
                yield return Ask("still2", "Did the cubes stay still\nas you looked around?");
                yield return Ask("nolaunch", "Did the picture come back\nWITHOUT pressing Launch?");
                board.text = "DONE\n\nTake the headset off.";
                yield return new WaitForSecondsRealtime(4f);
            }
            Debug.Log($"[EditorPlay] round {round}: done");
            Done = true;
#if UNITY_EDITOR
            // Under the EditorPlayBench test, the test stops and starts Play (UTF can't follow a
            // Play it didn't start)
            if (!UnityEditor.SessionState.GetBool(Key + "driven", false))
            {
                UnityEditor.SessionState.SetInt(Key + "round", round == 1 ? 2 : 1);
                UnityEditor.SessionState.SetFloat(Key + "replayAt", round == 1 ? (float)UnityEditor.EditorApplication.timeSinceStartup + 3f : -1f);
                UnityEditor.EditorApplication.ExitPlaymode();
            }
#endif
        }

        // Stopped by hand mid-run: the next Play starts over at round 1
        void OnDestroy()
        {
#if UNITY_EDITOR
            if (!Done && !UnityEditor.SessionState.GetBool(Key + "driven", false))
                UnityEditor.SessionState.SetInt(Key + "round", 1);
#endif
        }

        void Update()
        {
            foreach (var eye in new[] { rig.LeftEye, rig.RightEye })
                if (eye != null)
                {
                    eye.clearFlags = CameraClearFlags.SolidColor;
                    eye.backgroundColor = new Color(0.55f, 0.75f, 0.95f);
                }
            if (!recentred && rig.Backend != null && rig.Backend.TryGetHeadPose(out var head))
                Recentre(head);
        }

        // Current head yaw faces +Z, head over the origin (as FirstLightScene)
        void Recentre(QuestBridgePose head)
        {
            recentred = true;
            var yaw = Quaternion.Euler(0f, -head.rotation.eulerAngles.y, 0f);
            var p = yaw * head.position;
            var y = head.position.y < 0.8f ? standingHeight - head.position.y : 0f;
            rig.transform.SetPositionAndRotation(new Vector3(-p.x, y, -p.z), yaw);
        }

        // Waits for A/X, after every button is let go
        IEnumerator Step(string text)
        {
            yield return Press(text + "\n\npress A to continue", false, _ => { });
        }

        IEnumerator Ask(string key, string question)
        {
            var answer = "";
            yield return Press($"{question}\n\nA = YES    B = NO", true, a => answer = a);
            Debug.Log($"[EditorPlay] answer {key}={answer}");
            board.text = $"{question.Replace('\n', ' ')}\n{answer}\n\nlet go";
            while (Yes() || No())
                yield return null;
        }

        // Nothing pressed first, then the first press of A/X (YES) or, if allowed, B/Y (NO)
        IEnumerator Press(string text, bool allowNo, System.Action<string> done)
        {
            board.text = text;
            var armed = false;
            while (true)
            {
                bool yes = Yes(), no = allowNo && No();
                if (!yes && !no)
                    armed = true;
                else if (armed && yes != no)
                {
                    done(yes ? "YES" : "NO");
                    yield break;
                }
                yield return null;
            }
        }

        bool Yes() => Pad(QuestBridgeHand.Left).primaryButton || Pad(QuestBridgeHand.Right).primaryButton;
        bool No() => Pad(QuestBridgeHand.Left).secondaryButton || Pad(QuestBridgeHand.Right).secondaryButton;

        QuestBridgeControllerState Pad(QuestBridgeHand h) =>
            rig.Backend != null && rig.Backend.TryGetController(h, out var s) ? s : default;

        static int Get(string key, int fallback)
        {
#if UNITY_EDITOR
            return UnityEditor.SessionState.GetInt(Key + key, fallback);
#else
            return fallback;
#endif
        }
    }
}
