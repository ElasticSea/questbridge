using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
#if QUESTBRIDGE_XR_HANDS
using ElasticSea.QuestBridge.XRHands;
using UnityEngine.XR.Hands;
#endif

namespace ElasticSea.QuestBridge.Tests
{
    public class HandsPlayModeTests
    {
        [UnityTest]
        public IEnumerator MockHandsDriveJointSpheres()
        {
            var go = new GameObject("Rig");
            go.SetActive(false);
            go.transform.position = new Vector3(1f, 0f, 2f);
            var rig = go.AddComponent<QuestBridgeRig>();
            rig.backend = QuestBridgeRig.BackendMode.Mock;
            var hands = go.AddComponent<QuestBridgeHands>();
            var vis = go.AddComponent<HandJointsVisualizer>();
#if QUESTBRIDGE_XR_HANDS
            var xr = go.AddComponent<QuestBridgeXRHands>();
#endif
            go.SetActive(true);

            for (var i = 0; i < 3; i++)
                yield return null;

            Assert.IsTrue(hands.HasData && hands.Left.tracked && hands.Right.tracked);
            Assert.AreSame(hands, QuestBridgeHands.Active);
            var sphere = vis.JointSphere(QuestBridgeHand.Right, QuestBridgeHandJoint.IndexTip);
            Assert.IsTrue(sphere.gameObject.activeInHierarchy);
            var world = hands.WorldJoint(QuestBridgeHand.Right, QuestBridgeHandJoint.IndexTip);
            Assert.Less(Vector3.Distance(sphere.position, world.position), 1e-4f);
            Assert.Greater(world.position.z, 2f);  // in front of the rig origin

#if QUESTBRIDGE_XR_HANDS
            Assert.IsNotNull(xr.Subsystem);
            Assert.IsTrue(xr.Subsystem.running);
            xr.Subsystem.TryUpdateHands(XRHandSubsystem.UpdateType.Dynamic);
            Assert.IsTrue(xr.Subsystem.leftHand.isTracked && xr.Subsystem.rightHand.isTracked);
            Assert.IsTrue(xr.Subsystem.rightHand.GetJoint(XRHandJointID.IndexTip).TryGetPose(out var tip));
            Assert.Less(Vector3.Distance(tip.position, hands.Right[QuestBridgeHandJoint.IndexTip].position), 1e-4f);
            Assert.IsTrue(xr.Subsystem.leftHand.GetJoint(XRHandJointID.Palm).TryGetPose(out var palm));
            Assert.Less(Vector3.Distance(palm.position, hands.Left[QuestBridgeHandJoint.Palm].position), 1e-4f);
#endif
            Object.Destroy(go);
            yield return null;
            Assert.IsNull(QuestBridgeHands.Active);
        }
    }
}
