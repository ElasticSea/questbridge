using System;
using System.Collections;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.TestTools;

namespace ElasticSea.QuestLink.BridgeTest.Editor
{
    // Editor Play-mode bench, the tester's path: Assets/EditorPlay/EditorPlay.unity is a scene
    // with its Main Camera and the package's QuestLink Rig prefab dropped in. Press Play once:
    // round 1 runs until EditorPlayBoard is done, the board stops Play, Replay() starts it again
    // 3 s later (domain reload, same native runtime and headset session) for round 2.
    // Hands-off run: the TwoPlayRounds test in a real, minimized Editor on the hidden desktop
    // (unity/run-editorplay.ps1; a non-batchmode Editor started with -executeMethod hangs before
    // it logs anything, -runTests doesn't). QL_BENCH_MOCK=1: mock headset dry run.
    // Regenerate the scene: -batchmode -nographics -quit -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.EditorPlayBench.BuildScene
    [InitializeOnLoad]
    public class EditorPlayBench
    {
        const string ScenePath = "Assets/EditorPlay/EditorPlay.unity";
        const string Key = EditorPlayBoard.Key;
        const double TimeoutS = 3600, PauseS = 3;

        static EditorPlayBench() => EditorApplication.update += Replay;

        // Round 2 of a Play the user started: EditorPlayBoard stopped Play and set replayAt
        static void Replay()
        {
            var at = SessionState.GetFloat(Key + "replayAt", -1f);
            if (at < 0f || EditorApplication.isPlayingOrWillChangePlaymode || EditorApplication.timeSinceStartup < at)
                return;
            SessionState.SetFloat(Key + "replayAt", -1f);
            Debug.Log($"[EditorPlay] round {SessionState.GetInt(Key + "round", 1)}: Play again");
            EditorApplication.EnterPlaymode();
        }

        public static void BuildScene()
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.DefaultGameObjects, NewSceneMode.Single);
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>("Packages/com.elasticsea.questlink/Runtime/Prefabs/QuestLink Rig.prefab");
            var rigObject = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
            var bench = new GameObject("Editor Play Bench").AddComponent<EditorPlayBoard>();
            bench.rig = rigObject.GetComponent<QuestLinkRig>();
            bench.hands = rigObject.GetComponent<QuestLinkHands>();
            System.IO.Directory.CreateDirectory("Assets/EditorPlay");
            EditorSceneManager.SaveScene(scene, ScenePath);
            Debug.Log($"[EditorPlay] wrote {ScenePath}");
        }

        // The user's path, except that the test stops and restarts Play itself (UTF loses track
        // of a Play it didn't start); the scene's own Stop/Play is the same 3 s later Replay().
        [UnityTest, Explicit("live bench, unity/run-editorplay.ps1"), Timeout(4000000)]
        public IEnumerator TwoPlayRounds()
        {
            SessionState.SetFloat(Key + "start", (float)EditorApplication.timeSinceStartup);
            SessionState.SetInt(Key + "round", 1);
            SessionState.SetBool(Key + "driven", true);
            EditorSceneManager.OpenScene(ScenePath);
            Debug.Log($"[EditorPlay] bench start, graphics {SystemInfo.graphicsDeviceType}");
#if UNITY_EDITOR_WIN
            ShowWindow(GetActiveWindow(), 6);  // SW_MINIMIZE
#endif
            // Locals don't survive the domain reload of EnterPlayMode; state lives in SessionState
            for (; SessionState.GetInt(Key + "round", 1) <= 2; SessionState.SetInt(Key + "round", SessionState.GetInt(Key + "round", 1) + 1))
            {
                yield return new EnterPlayMode();
                while (!EditorPlayBoard.Done && !TimedOut())
                    yield return null;
                Debug.Log($"[EditorPlay] round {SessionState.GetInt(Key + "round", 1)}: Stop{(TimedOut() ? " (timeout)" : "")}");
                yield return new ExitPlayMode();
                SessionState.SetFloat(Key + "stopped", (float)EditorApplication.timeSinceStartup);
                while (EditorApplication.timeSinceStartup - SessionState.GetFloat(Key + "stopped", 0f) < PauseS)
                    yield return null;
                if (TimedOut())
                    break;
            }
            SessionState.SetBool(Key + "driven", false);
            SessionState.SetInt(Key + "round", 1);
            Debug.Log($"[EditorPlay] bench end{(TimedOut() ? " (timeout)" : "")}");
            Assert.IsFalse(TimedOut(), "bench timed out");
        }

        static bool TimedOut() => EditorApplication.timeSinceStartup - SessionState.GetFloat(Key + "start", 0f) > TimeoutS;

#if UNITY_EDITOR_WIN
        [DllImport("user32.dll")] static extern IntPtr GetActiveWindow();
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window, int command);
#endif
    }
}
