using System.Runtime.InteropServices;
using ElasticSea.QuestLink.Native;
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestLink.Tests
{
    public class HandTests
    {
        const float Eps = 1e-4f;

        // Unity-convention joints of a hand whose wrist frame is identity at the origin with
        // every bone at its bind rotation: palm down, fingers forward
        static QuestLinkJointPose[] BindHand(QuestLinkHand hand)
        {
            var local = new Quaternion[QuestLinkHandSkeleton.BoneCount];
            for (var b = 0; b < local.Length; b++)
                local[b] = b == 0 || b == 1 || b >= 19 ? Quaternion.identity : QuestLinkHandSkeleton.BindRotation(b);
            var bones = new Pose[QuestLinkHandSkeleton.BoneCount];
            var root = new Pose(Vector3.zero, QuestLinkHandSkeleton.RootFromWrist(hand, Quaternion.identity));
            QuestLinkHandSkeleton.Bones(hand, root, local, 1f, bones);
            var joints = new QuestLinkJointPose[QuestLinkHandState.JointCount];
            QuestLinkHandSkeleton.Joints(hand, bones, 1f, joints);
            QuestLinkHandSkeleton.ToUnity(joints);
            return joints;
        }

        static QuestLinkJointPose J(QuestLinkJointPose[] joints, QuestLinkHandJoint j) => joints[(int)j];

        [Test]
        public void BindHandPointsForwardWithThumbInward()
        {
            foreach (var hand in new[] { QuestLinkHand.Left, QuestLinkHand.Right })
            {
                var j = BindHand(hand);
                var wrist = J(j, QuestLinkHandJoint.Wrist);
                Assert.AreEqual(1f, Mathf.Abs(Quaternion.Dot(wrist.rotation, Quaternion.identity)), Eps, $"{hand} wrist frame");
                // Fingers ahead (+Z), knuckles ~10 cm from the wrist
                var middle = J(j, QuestLinkHandJoint.MiddleProximal).position;
                Assert.Greater(middle.z, 0.09f);
                Assert.Greater(J(j, QuestLinkHandJoint.MiddleTip).position.z, 0.17f);
                // Palm down, thumb toward the body's midline: right of a left hand, left of a right one
                var side = hand == QuestLinkHand.Left ? 1f : -1f;
                Assert.Greater(side * J(j, QuestLinkHandJoint.ThumbTip).position.x, 0.03f, $"{hand} thumb side");
                Assert.Greater(side * (J(j, QuestLinkHandJoint.IndexProximal).position.x - J(j, QuestLinkHandJoint.LittleProximal).position.x), 0.03f);
            }
        }

        [Test]
        public void JointAxesFollowTheBones()
        {
            var mock = new MockQuestLink();
            foreach (var t in new[] { 0f, 1.7f, 4.2f })
            foreach (var hand in new[] { QuestLinkHand.Left, QuestLinkHand.Right })
            {
                var s = new QuestLinkHandState(hand);
                MockQuestLink.HandAt(hand, t, s);
                // +Z of each joint points at the next joint along the finger
                foreach (var (a, b) in new[]
                         {
                             (QuestLinkHandJoint.IndexProximal, QuestLinkHandJoint.IndexIntermediate),
                             (QuestLinkHandJoint.MiddleIntermediate, QuestLinkHandJoint.MiddleDistal),
                             (QuestLinkHandJoint.LittleProximal, QuestLinkHandJoint.LittleIntermediate),
                             (QuestLinkHandJoint.IndexMetacarpal, QuestLinkHandJoint.IndexProximal),
                             (QuestLinkHandJoint.ThumbProximal, QuestLinkHandJoint.ThumbDistal),
                         })
                {
                    var dir = (s[b].position - s[a].position).normalized;
                    Assert.Greater(Vector3.Dot(s[a].rotation * Vector3.forward, dir), 0.99f, $"{hand} {a} at {t}");
                }
                // +Y out of the back of the hand: away from the palm side the fingers curl toward
                var wrist = s[QuestLinkHandJoint.Wrist];
                var dorsal = wrist.rotation * Vector3.up;
                var palmToTip = s[QuestLinkHandJoint.MiddleTip].position - s[QuestLinkHandJoint.MiddleProximal].position;
                Assert.Less(Vector3.Dot(dorsal, palmToTip), 0.01f, $"{hand} fingers curl toward the palm");
                var palm = s[QuestLinkHandJoint.Palm].position;
                var mm = s[QuestLinkHandJoint.MiddleMetacarpal].position;
                var mp = s[QuestLinkHandJoint.MiddleProximal].position;
                Assert.Less(Vector3.Distance(palm, (mm + mp) * 0.5f), Eps);
                foreach (var joint in s.joints)
                    Assert.That(joint.radius, Is.InRange(0.004f, 0.03f));
            }
            mock.Dispose();
        }

        [Test]
        public void MockHandsAnimate()
        {
            var mock = new MockQuestLink { timeOverride = 1f };
            var left = new QuestLinkHandState(QuestLinkHand.Left);
            var right = new QuestLinkHandState(QuestLinkHand.Right);
            Assert.IsTrue(mock.TryGetHand(QuestLinkHand.Left, left));
            Assert.IsTrue(mock.TryGetHand(QuestLinkHand.Right, right));
            Assert.IsTrue(left.tracked && right.tracked);
            Assert.IsTrue(right.dominant && !left.dominant);
            Assert.Less(left.Root.position.x, right.Root.position.x);
            var tip = left[QuestLinkHandJoint.IndexTip].position;
            var pinch = left.Pinch(QuestLinkFinger.Index);
            mock.timeOverride = 2f;
            mock.TryGetHand(QuestLinkHand.Left, left);
            Assert.Greater(Vector3.Distance(tip, left[QuestLinkHandJoint.IndexTip].position), 0.005f);
            Assert.AreNotEqual(pinch, left.Pinch(QuestLinkFinger.Index));
            Assert.That(left.pinch, Has.All.InRange(0f, 1f));
        }

        [Test]
        public void XRHandsIndexSwapsWristAndPalm()
        {
            Assert.AreEqual(0, QuestLinkHandState.XRHandsIndex(QuestLinkHandJoint.Wrist));
            Assert.AreEqual(1, QuestLinkHandState.XRHandsIndex(QuestLinkHandJoint.Palm));
            Assert.AreEqual(2, QuestLinkHandState.XRHandsIndex(QuestLinkHandJoint.ThumbMetacarpal));
            Assert.AreEqual(25, QuestLinkHandState.XRHandsIndex(QuestLinkHandJoint.LittleTip));
        }

        [Test]
        public void NativeHandLayoutMatchesHeader()
        {
            // questlink.h: ql_hand 944 B with joints at 112; ql_hands 1896 B
            Assert.AreEqual(944, Marshal.SizeOf<QuestLinkNative.ql_hand>());
            Assert.AreEqual(112, (int)Marshal.OffsetOf<QuestLinkNative.ql_hand>("joints"));
            Assert.AreEqual(32, (int)Marshal.OffsetOf<QuestLinkNative.ql_hand>("root"));
            Assert.AreEqual(1896, Marshal.SizeOf<QuestLinkNative.ql_hands>());
        }

        [Test]
        public unsafe void NativeHandConvertsToUnity()
        {
            var h = new QuestLinkNative.ql_hand
            {
                flags = QuestLinkNative.HandTracked | QuestLinkNative.HandDominant,
                confidence = 1f,
                scale = 1.04f,
                root = new QuestLinkNative.ql_pose { qw = 1f, time_ns = 123 },
                aim = new QuestLinkNative.ql_pose { qw = 1f, pz = -0.5f },
            };
            h.pinch[0] = 0.7f;
            var f = 8 * (int)QuestLinkHandJoint.IndexTip;
            // 90 deg about Link +Y (turns -Z to -X), half a metre ahead
            h.joints[f + 1] = Mathf.Sqrt(0.5f);
            h.joints[f + 3] = Mathf.Sqrt(0.5f);
            h.joints[f + 6] = -0.5f;
            h.joints[f + 7] = 0.008f;
            var s = new QuestLinkHandState(QuestLinkHand.Right);
            NativeQuestLink.FromNative(h, s);
            Assert.IsTrue(s.tracked && s.dominant && !s.aimValid);
            Assert.AreEqual(123L, s.timeNs);
            Assert.AreEqual(0.7f, s.Pinch(QuestLinkFinger.Index), Eps);
            Assert.AreEqual(new Vector3(0, 0, 0.5f), s.aim.position);
            var tip = s[QuestLinkHandJoint.IndexTip];
            Assert.AreEqual(0.5f, tip.position.z, Eps);
            Assert.AreEqual(0.008f, tip.radius, Eps);
            var fwd = tip.rotation * Vector3.forward;
            Assert.AreEqual(-1f, fwd.x, Eps);  // still turned to the left

            h.flags = 0;
            NativeQuestLink.FromNative(h, s);
            Assert.IsFalse(s.tracked);
            Assert.AreEqual(Vector3.zero, s[QuestLinkHandJoint.IndexTip].position);
        }
    }
}
