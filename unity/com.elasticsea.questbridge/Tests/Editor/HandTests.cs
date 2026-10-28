// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using ElasticSea.QuestBridge.Native;
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestBridge.Tests
{
    public class HandTests
    {
        const float Eps = 1e-4f;

        // Unity-convention joints of a hand whose wrist frame is identity at the origin with
        // every bone at its bind rotation: palm down, fingers forward
        static QuestBridgeJointPose[] BindHand(QuestBridgeHand hand)
        {
            var local = new Quaternion[QuestBridgeHandSkeleton.BoneCount];
            for (var b = 0; b < local.Length; b++)
                local[b] = b == 0 || b == 1 || b >= 19 ? Quaternion.identity : QuestBridgeHandSkeleton.BindRotation(b);
            var bones = new Pose[QuestBridgeHandSkeleton.BoneCount];
            var root = new Pose(Vector3.zero, QuestBridgeHandSkeleton.RootFromWrist(hand, Quaternion.identity));
            QuestBridgeHandSkeleton.Bones(hand, root, local, 1f, bones);
            var joints = new QuestBridgeJointPose[QuestBridgeHandState.JointCount];
            QuestBridgeHandSkeleton.Joints(hand, bones, 1f, joints);
            QuestBridgeHandSkeleton.ToUnity(joints);
            return joints;
        }

        static QuestBridgeJointPose J(QuestBridgeJointPose[] joints, QuestBridgeHandJoint j) => joints[(int)j];

        [Test]
        public void BindHandPointsForwardWithThumbInward()
        {
            foreach (var hand in new[] { QuestBridgeHand.Left, QuestBridgeHand.Right })
            {
                var j = BindHand(hand);
                var wrist = J(j, QuestBridgeHandJoint.Wrist);
                Assert.AreEqual(1f, Mathf.Abs(Quaternion.Dot(wrist.rotation, Quaternion.identity)), Eps, $"{hand} wrist frame");
                // Fingers ahead (+Z), knuckles ~10 cm from the wrist
                var middle = J(j, QuestBridgeHandJoint.MiddleProximal).position;
                Assert.Greater(middle.z, 0.09f);
                Assert.Greater(J(j, QuestBridgeHandJoint.MiddleTip).position.z, 0.17f);
                // Palm down, thumb toward the body's midline: right of a left hand, left of a right one
                var side = hand == QuestBridgeHand.Left ? 1f : -1f;
                Assert.Greater(side * J(j, QuestBridgeHandJoint.ThumbTip).position.x, 0.03f, $"{hand} thumb side");
                Assert.Greater(side * (J(j, QuestBridgeHandJoint.IndexProximal).position.x - J(j, QuestBridgeHandJoint.LittleProximal).position.x), 0.03f);
            }
        }

        [Test]
        public void JointAxesFollowTheBones()
        {
            var mock = new MockQuestBridge();
            foreach (var t in new[] { 0f, 1.7f, 4.2f })
            foreach (var hand in new[] { QuestBridgeHand.Left, QuestBridgeHand.Right })
            {
                var s = new QuestBridgeHandState(hand);
                MockQuestBridge.HandAt(hand, t, s);
                // +Z of each joint points at the next joint along the finger
                foreach (var (a, b) in new[]
                         {
                             (QuestBridgeHandJoint.IndexProximal, QuestBridgeHandJoint.IndexIntermediate),
                             (QuestBridgeHandJoint.MiddleIntermediate, QuestBridgeHandJoint.MiddleDistal),
                             (QuestBridgeHandJoint.LittleProximal, QuestBridgeHandJoint.LittleIntermediate),
                             (QuestBridgeHandJoint.IndexMetacarpal, QuestBridgeHandJoint.IndexProximal),
                             (QuestBridgeHandJoint.ThumbProximal, QuestBridgeHandJoint.ThumbDistal),
                         })
                {
                    var dir = (s[b].position - s[a].position).normalized;
                    Assert.Greater(Vector3.Dot(s[a].rotation * Vector3.forward, dir), 0.99f, $"{hand} {a} at {t}");
                }
                // +Y out of the back of the hand: away from the palm side the fingers curl toward
                var wrist = s[QuestBridgeHandJoint.Wrist];
                var dorsal = wrist.rotation * Vector3.up;
                var palmToTip = s[QuestBridgeHandJoint.MiddleTip].position - s[QuestBridgeHandJoint.MiddleProximal].position;
                Assert.Less(Vector3.Dot(dorsal, palmToTip), 0.01f, $"{hand} fingers curl toward the palm");
                var palm = s[QuestBridgeHandJoint.Palm].position;
                var mm = s[QuestBridgeHandJoint.MiddleMetacarpal].position;
                var mp = s[QuestBridgeHandJoint.MiddleProximal].position;
                Assert.Less(Vector3.Distance(palm, (mm + mp) * 0.5f), Eps);
                foreach (var joint in s.joints)
                    Assert.That(joint.radius, Is.InRange(0.004f, 0.03f));
            }
            mock.Dispose();
        }

        [Test]
        public void MockHandsAnimate()
        {
            var mock = new MockQuestBridge { timeOverride = 1f };
            var left = new QuestBridgeHandState(QuestBridgeHand.Left);
            var right = new QuestBridgeHandState(QuestBridgeHand.Right);
            Assert.IsTrue(mock.TryGetHand(QuestBridgeHand.Left, left));
            Assert.IsTrue(mock.TryGetHand(QuestBridgeHand.Right, right));
            Assert.IsTrue(left.tracked && right.tracked);
            Assert.IsTrue(right.dominant && !left.dominant);
            Assert.Less(left.Root.position.x, right.Root.position.x);
            var tip = left[QuestBridgeHandJoint.IndexTip].position;
            var pinch = left.Pinch(QuestBridgeFinger.Index);
            mock.timeOverride = 2f;
            mock.TryGetHand(QuestBridgeHand.Left, left);
            Assert.Greater(Vector3.Distance(tip, left[QuestBridgeHandJoint.IndexTip].position), 0.005f);
            Assert.AreNotEqual(pinch, left.Pinch(QuestBridgeFinger.Index));
            Assert.That(left.pinch, Has.All.InRange(0f, 1f));
        }

        [Test]
        public void XRHandsIndexSwapsWristAndPalm()
        {
            Assert.AreEqual(0, QuestBridgeHandState.XRHandsIndex(QuestBridgeHandJoint.Wrist));
            Assert.AreEqual(1, QuestBridgeHandState.XRHandsIndex(QuestBridgeHandJoint.Palm));
            Assert.AreEqual(2, QuestBridgeHandState.XRHandsIndex(QuestBridgeHandJoint.ThumbMetacarpal));
            Assert.AreEqual(25, QuestBridgeHandState.XRHandsIndex(QuestBridgeHandJoint.LittleTip));
        }

        [Test]
        public void NativeHandLayoutMatchesHeader()
        {
            // questbridge.h: qb_hand 944 B with joints at 112; qb_hands 1896 B
            Assert.AreEqual(944, Marshal.SizeOf<QuestBridgeNative.qb_hand>());
            Assert.AreEqual(112, (int)Marshal.OffsetOf<QuestBridgeNative.qb_hand>("joints"));
            Assert.AreEqual(32, (int)Marshal.OffsetOf<QuestBridgeNative.qb_hand>("root"));
            Assert.AreEqual(1896, Marshal.SizeOf<QuestBridgeNative.qb_hands>());
        }

        [Test]
        public unsafe void NativeHandConvertsToUnity()
        {
            var h = new QuestBridgeNative.qb_hand
            {
                flags = QuestBridgeNative.HandTracked | QuestBridgeNative.HandDominant,
                confidence = 1f,
                scale = 1.04f,
                root = new QuestBridgeNative.qb_pose { qw = 1f, time_ns = 123 },
                aim = new QuestBridgeNative.qb_pose { qw = 1f, pz = -0.5f },
            };
            h.pinch[0] = 0.7f;
            var f = 8 * (int)QuestBridgeHandJoint.IndexTip;
            // 90 deg about Link +Y (turns -Z to -X), half a metre ahead
            h.joints[f + 1] = Mathf.Sqrt(0.5f);
            h.joints[f + 3] = Mathf.Sqrt(0.5f);
            h.joints[f + 6] = -0.5f;
            h.joints[f + 7] = 0.008f;
            var s = new QuestBridgeHandState(QuestBridgeHand.Right);
            NativeQuestBridge.FromNative(h, s);
            Assert.IsTrue(s.tracked && s.dominant && !s.aimValid);
            Assert.AreEqual(123L, s.timeNs);
            Assert.AreEqual(0.7f, s.Pinch(QuestBridgeFinger.Index), Eps);
            Assert.AreEqual(new Vector3(0, 0, 0.5f), s.aim.position);
            var tip = s[QuestBridgeHandJoint.IndexTip];
            Assert.AreEqual(0.5f, tip.position.z, Eps);
            Assert.AreEqual(0.008f, tip.radius, Eps);
            var fwd = tip.rotation * Vector3.forward;
            Assert.AreEqual(-1f, fwd.x, Eps);  // still turned to the left

            h.flags = 0;
            NativeQuestBridge.FromNative(h, s);
            Assert.IsFalse(s.tracked);
            Assert.AreEqual(Vector3.zero, s[QuestBridgeHandJoint.IndexTip].position);
        }
    }
}
