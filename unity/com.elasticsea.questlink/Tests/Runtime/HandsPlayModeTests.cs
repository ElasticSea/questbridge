using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
#if QUESTLINK_XR_HANDS
using ElasticSea.QuestLink.XRHands;
using UnityEngine.XR.Hands;
#endif

namespace ElasticSea.QuestLink.Tests
{
    public class HandsPlayModeTests
    {
        [UnityTest]
        public IEnumerator MockHandsDriveJointSpheres()
        {
            var go = new GameObject("Rig");
            go.SetActive(false);
            go.transform.position = new Vector3(1f, 0f, 2f);
            var rig = go.AddComponent<QuestLinkRig>();
            rig.backend = QuestLinkRig.BackendMode.Mock;
            var hands = go.AddComponent<QuestLinkHands>();
            var vis = go.AddComponent<HandJointsVisualizer>();
#if QUESTLINK_XR_HANDS
            var xr = go.AddComponent<QuestLinkXRHands>();
#endif
            go.SetActive(true);

            for (var i = 0; i < 3; i++)
                yield return null;

            Assert.IsTrue(hands.HasData && hands.Left.tracked && hands.Right.tracked);
            Assert.AreSame(hands, QuestLinkHands.Active);
            var sphere = vis.JointSphere(QuestLinkHand.Right, QuestLinkHandJoint.IndexTip);
            Assert.IsTrue(sphere.gameObject.activeInHierarchy);
            var world = hands.WorldJoint(QuestLinkHand.Right, QuestLinkHandJoint.IndexTip);
            Assert.Less(Vector3.Distance(sphere.position, world.position), 1e-4f);
            Assert.Greater(world.position.z, 2f);  // in front of the rig origin

#if QUESTLINK_XR_HANDS
            Assert.IsNotNull(xr.Subsystem);
            Assert.IsTrue(xr.Subsystem.running);
            xr.Subsystem.TryUpdateHands(XRHandSubsystem.UpdateType.Dynamic);
            Assert.IsTrue(xr.Subsystem.leftHand.isTracked && xr.Subsystem.rightHand.isTracked);
            Assert.IsTrue(xr.Subsystem.rightHand.GetJoint(XRHandJointID.IndexTip).TryGetPose(out var tip));
            Assert.Less(Vector3.Distance(tip.position, hands.Right[QuestLinkHandJoint.IndexTip].position), 1e-4f);
            Assert.IsTrue(xr.Subsystem.leftHand.GetJoint(XRHandJointID.Palm).TryGetPose(out var palm));
            Assert.Less(Vector3.Distance(palm.position, hands.Left[QuestLinkHandJoint.Palm].position), 1e-4f);
#endif
            Object.Destroy(go);
            yield return null;
            Assert.IsNull(QuestLinkHands.Active);
        }
    }
}
