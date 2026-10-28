using ElasticSea.QuestLink.Input;
using UnityEditor;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestLink.BridgeTest.Editor
{
    // Regenerates the package's drop-in "QuestLink Rig" prefab: rig + hands, Head with the
    // AudioListener and QuestLinkAudio, two controllers on stock TrackedPoseDrivers.
    // tools\unity_hidden.ps1 -ArgLine "-batchmode -nographics -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildRigPrefab.Build -logFile prefab.log"
    public static class BuildRigPrefab
    {
        public const string Path = "Packages/com.elasticsea.questlink/Runtime/Prefabs/QuestLink Rig.prefab";

        public static void Build()
        {
            var root = new GameObject("QuestLink Rig");
            try
            {
                // Inactive while building, so OnEnable doesn't start a backend in the Editor
                root.SetActive(false);
                root.AddComponent<QuestLinkRig>();
                root.AddComponent<QuestLinkHands>();
                root.AddComponent<HandJointsVisualizer>();

                var head = new GameObject("Head");
                head.transform.SetParent(root.transform, false);
                head.AddComponent<AudioListener>();
                head.AddComponent<QuestLinkAudio>().rig = root.GetComponent<QuestLinkRig>();

                Controller(root.transform, QuestLinkHand.Left);
                Controller(root.transform, QuestLinkHand.Right);

                root.SetActive(true);
                System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(Path)));
                PrefabUtility.SaveAsPrefabAsset(root, Path, out var ok);
                if (!ok)
                    throw new System.Exception($"Saving {Path} failed");
                Debug.Log($"[QuestLink] wrote {Path}");
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        static void Controller(Transform origin, QuestLinkHand hand)
        {
            var usage = hand == QuestLinkHand.Left ? "LeftHand" : "RightHand";
            var go = new GameObject(hand == QuestLinkHand.Left ? "Left Controller" : "Right Controller");
            go.transform.SetParent(origin, false);
            var driver = go.AddComponent<TrackedPoseDriver>();
            driver.positionInput = new InputActionProperty(new InputAction("Position", binding: $"<XRController>{{{usage}}}/devicePosition"));
            driver.rotationInput = new InputActionProperty(new InputAction("Rotation", binding: $"<XRController>{{{usage}}}/deviceRotation"));
            driver.trackingStateInput = new InputActionProperty(new InputAction("Tracking State", binding: $"<XRController>{{{usage}}}/trackingState"));
            // Small block at the controller, hidden while untracked
            go.AddComponent<QuestLinkTrackedVisual>().hand = hand;
        }
    }
}
