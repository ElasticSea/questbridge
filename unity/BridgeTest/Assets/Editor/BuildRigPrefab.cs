// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using ElasticSea.QuestBridge.Input;
using UnityEditor;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestBridge.BridgeTest.Editor
{
    // Regenerates the package's drop-in "QuestBridge Rig" prefab: rig + hands + link status banner,
    // Head with the AudioListener and QuestBridgeAudio, two controllers on stock TrackedPoseDrivers.
    // tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestBridge.BridgeTest.Editor.BuildRigPrefab.Build -logFile prefab.log"
    public static class BuildRigPrefab
    {
        public const string Path = "Packages/com.elasticsea.questbridge/Runtime/Prefabs/QuestBridge Rig.prefab";

        public static void Build()
        {
            var root = new GameObject("QuestBridge Rig");
            try
            {
                // Inactive while building, so OnEnable doesn't start a backend in the Editor
                root.SetActive(false);
                root.AddComponent<QuestBridgeRig>();
                root.AddComponent<QuestBridgeHands>();
                root.AddComponent<HandJointsVisualizer>();
                root.AddComponent<QuestBridgeConnectionStatus>();

                var head = new GameObject("Head");
                head.transform.SetParent(root.transform, false);
                head.AddComponent<AudioListener>();
                head.AddComponent<QuestBridgeAudio>().rig = root.GetComponent<QuestBridgeRig>();

                Controller(root.transform, QuestBridgeHand.Left);
                Controller(root.transform, QuestBridgeHand.Right);

                root.SetActive(true);
                System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(Path)));
                PrefabUtility.SaveAsPrefabAsset(root, Path, out var ok);
                if (!ok)
                    throw new System.Exception($"Saving {Path} failed");
                Debug.Log($"[QuestBridge] wrote {Path}");
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        static void Controller(Transform origin, QuestBridgeHand hand)
        {
            var usage = hand == QuestBridgeHand.Left ? "LeftHand" : "RightHand";
            var go = new GameObject(hand == QuestBridgeHand.Left ? "Left Controller" : "Right Controller");
            go.transform.SetParent(origin, false);
            var driver = go.AddComponent<TrackedPoseDriver>();
            driver.positionInput = new InputActionProperty(new InputAction("Position", binding: $"<XRController>{{{usage}}}/devicePosition"));
            driver.rotationInput = new InputActionProperty(new InputAction("Rotation", binding: $"<XRController>{{{usage}}}/deviceRotation"));
            driver.trackingStateInput = new InputActionProperty(new InputAction("Tracking State", binding: $"<XRController>{{{usage}}}/trackingState"));
            // Small block at the controller, hidden while untracked
            go.AddComponent<QuestBridgeTrackedVisual>().hand = hand;
        }
    }
}
