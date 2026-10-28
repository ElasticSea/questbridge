// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections;
using System.Diagnostics;
using ElasticSea.QuestBridge.Editor;
using ElasticSea.QuestBridge.Input;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.InputSystem.XR;
using UnityEngine.Rendering;
using UnityEngine.TestTools;
using Debug = UnityEngine.Debug;
using Object = UnityEngine.Object;

namespace ElasticSea.QuestBridge.Tests
{
    // Editor Play mode with the real questbridge.dll: Play, Stop, Play again (with and without a
    // domain reload) on the one native runtime the Editor keeps loaded. Needs QB_USB=0 in the
    // Editor's environment (tools\test_all.ps1 sets it), so the library never touches the
    // headset that stays plugged into the bench PC, and a graphics device (no -nographics).
    public class EditorPlayTests
    {
        static void RequireOfflineNative()
        {
            if (Environment.GetEnvironmentVariable("QB_USB") != "0")
                Assert.Ignore("QB_USB=0 not set: the native library would open the headset's USB interface");
            var mac = Application.platform == RuntimePlatform.OSXEditor;
            if (!QuestBridgeSetup.PluginInstalled(mac))
                Assert.Ignore(mac ? "libquestbridge.dylib not installed (unity/install-native.sh)" : "questbridge.dll not installed (unity\\install-native.ps1)");
            var api = mac ? GraphicsDeviceType.Metal : GraphicsDeviceType.Direct3D11;
            if (SystemInfo.graphicsDeviceType != api)
                Assert.Ignore($"needs {api}, Editor runs {SystemInfo.graphicsDeviceType} (drop -nographics)");
        }

        // Locals don't survive the domain reload of EnterPlayMode, so state goes through SessionState
        const string Key = "QuestBridgeTests.";

        [UnityTest]
        public IEnumerator NativeRigAcrossPlayStopPlay() => PlayStopPlay(reload: true);

        [UnityTest]
        public IEnumerator NativeRigAcrossPlayStopPlayWithoutDomainReload() => PlayStopPlay(reload: false);

        IEnumerator PlayStopPlay(bool reload)
        {
            RequireOfflineNative();
            SessionState.SetInt(Key + "rate", Application.targetFrameRate);
            SessionState.SetBool(Key + "options", EditorSettings.enterPlayModeOptionsEnabled);
            SessionState.SetInt(Key + "flags", (int)EditorSettings.enterPlayModeOptions);
            EditorSettings.enterPlayModeOptionsEnabled = !reload;
            EditorSettings.enterPlayModeOptions = reload ? EnterPlayModeOptions.None : EnterPlayModeOptions.DisableDomainReload;
            try
            {
                for (SessionState.SetInt(Key + "round", 0); SessionState.GetInt(Key + "round", 0) < 2; SessionState.SetInt(Key + "round", SessionState.GetInt(Key + "round", 0) + 1))
                {
                    yield return new EnterPlayMode(expectDomainReload: reload);
                    var go = new GameObject("Rig");
                    go.SetActive(false);
                    var rig = go.AddComponent<QuestBridgeRig>();
                    rig.backend = QuestBridgeRig.BackendMode.Native;
                    var watch = Stopwatch.StartNew();
                    go.SetActive(true);
                    watch.Stop();
                    Assert.IsNotNull(rig.Backend, "native backend didn't load");
                    Assert.AreEqual("Native", rig.Backend.Name);
                    Assert.Less(watch.ElapsedMilliseconds, 200, "starting the rig stalls the Editor");

                    var worst = 0.0;
                    for (var i = 0; i < 30; i++)
                    {
                        var t = Time.realtimeSinceStartupAsDouble;
                        yield return null;
                        // Batchmode doesn't render; drive the submit path by hand
                        if (Application.isBatchMode)
                        {
                            rig.LatchPose();
                            rig.LeftEye.Render();
                            rig.RightEye.Render();
                        }
                        if (i > 2)
                            worst = Math.Max(worst, Time.realtimeSinceStartupAsDouble - t);
                    }
                    Assert.AreEqual(QuestBridgeState.Idle, rig.State, "no headset (QB_USB=0) must read as idle");
                    Debug.Log($"[QuestBridge] round {SessionState.GetInt(Key + "round", 0)}: worst frame {worst * 1000:F0} ms");
                    yield return new ExitPlayMode();
                    Assert.AreEqual(SessionState.GetInt(Key + "rate", 0), Application.targetFrameRate, "frame rate restored after Stop");
                }
            }
            finally
            {
                EditorSettings.enterPlayModeOptionsEnabled = SessionState.GetBool(Key + "options", false);
                EditorSettings.enterPlayModeOptions = (EnterPlayModeOptions)SessionState.GetInt(Key + "flags", 0);
            }
        }

        [Test]
        public void RigPrefabIsReadyToDrop()
        {
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(QuestBridgeSetup.RigPrefabPath);
            Assert.IsNotNull(prefab, QuestBridgeSetup.RigPrefabPath);
            var rig = prefab.GetComponent<QuestBridgeRig>();
            Assert.IsNotNull(rig);
            Assert.AreEqual(QuestBridgeRig.BackendMode.Auto, rig.backend);
            Assert.IsNotNull(prefab.GetComponent<QuestBridgeHands>());
            Assert.IsNotNull(prefab.GetComponent<HandJointsVisualizer>());

            var head = prefab.transform.Find("Head");
            Assert.IsNotNull(head.GetComponent<AudioListener>());
            Assert.AreSame(rig, head.GetComponent<QuestBridgeAudio>().rig);
            Assert.IsNull(prefab.GetComponentInChildren<Camera>(true), "eyes are created at Play; a camera in the prefab would render in Edit mode");

            foreach (var (name, usage, hand) in new[] { ("Left Controller", "LeftHand", QuestBridgeHand.Left), ("Right Controller", "RightHand", QuestBridgeHand.Right) })
            {
                var c = prefab.transform.Find(name);
                var driver = c.GetComponent<TrackedPoseDriver>();
                Assert.AreEqual($"<XRController>{{{usage}}}/devicePosition", driver.positionInput.action.bindings[0].path);
                Assert.AreEqual($"<XRController>{{{usage}}}/deviceRotation", driver.rotationInput.action.bindings[0].path);
                Assert.AreEqual(hand, c.GetComponent<QuestBridgeTrackedVisual>().hand);
            }
        }

        [Test]
        public void SetupChecksRun()
        {
            foreach (var p in QuestBridgeSetup.Check())
                Debug.Log($"[QuestBridge] setup: {p.type} {p.message}");
        }
    }
}
