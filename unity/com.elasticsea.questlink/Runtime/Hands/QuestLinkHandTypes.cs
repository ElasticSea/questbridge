using UnityEngine;

namespace ElasticSea.QuestLink
{
    // OpenXR XR_EXT_hand_tracking joints, in XrHandJointEXT order (libquestlink's order too).
    public enum QuestLinkHandJoint
    {
        Palm, Wrist,
        ThumbMetacarpal, ThumbProximal, ThumbDistal, ThumbTip,
        IndexMetacarpal, IndexProximal, IndexIntermediate, IndexDistal, IndexTip,
        MiddleMetacarpal, MiddleProximal, MiddleIntermediate, MiddleDistal, MiddleTip,
        RingMetacarpal, RingProximal, RingIntermediate, RingDistal, RingTip,
        LittleMetacarpal, LittleProximal, LittleIntermediate, LittleDistal, LittleTip,
    }

    public enum QuestLinkFinger { Index, Middle, Ring, Little }

    // Joint pose in Unity convention, tracking space. Like Unity XR Hands: +Z points toward the
    // fingertip, +Y out of the back of the hand.
    public struct QuestLinkJointPose
    {
        public Vector3 position;
        public Quaternion rotation;
        public float radius;  // metres
    }

    // One hand, filled in place by IQuestLinkBackend.TryGetHand (no per-frame allocation).
    public sealed class QuestLinkHandState
    {
        public const int JointCount = 26;

        public readonly QuestLinkHand hand;
        public bool tracked;
        public float confidence;     // 0..1
        public float scale;          // hand size relative to the bind skeleton
        public bool dominant;
        public bool aimValid;
        public QuestLinkPose aim;    // pointer ray: +Z = pointing direction
        public long timeNs;          // headset clock (sample time)
        public readonly float[] pinch = new float[4];  // QuestLinkFinger order, 0..1
        public readonly QuestLinkJointPose[] joints = new QuestLinkJointPose[JointCount];

        public QuestLinkHandState(QuestLinkHand hand) => this.hand = hand;

        // Unity XR Hands joint index (XRHandJointID.ToIndex()): same order with wrist and palm swapped
        public static int XRHandsIndex(QuestLinkHandJoint joint) =>
            joint == QuestLinkHandJoint.Palm ? 1 : joint == QuestLinkHandJoint.Wrist ? 0 : (int)joint;

        public QuestLinkJointPose this[QuestLinkHandJoint joint] => joints[(int)joint];
        public float Pinch(QuestLinkFinger finger) => pinch[(int)finger];
        public QuestLinkPose Root => new QuestLinkPose { position = joints[1].position, rotation = joints[1].rotation, timeNs = timeNs };

        public void Clear()
        {
            tracked = dominant = aimValid = false;
            confidence = scale = 0f;
            aim = QuestLinkPose.Identity;
            timeNs = 0;
            System.Array.Clear(pinch, 0, pinch.Length);
            System.Array.Clear(joints, 0, joints.Length);
        }

        public void CopyFrom(QuestLinkHandState other)
        {
            tracked = other.tracked;
            confidence = other.confidence;
            scale = other.scale;
            dominant = other.dominant;
            aimValid = other.aimValid;
            aim = other.aim;
            timeNs = other.timeNs;
            System.Array.Copy(other.pinch, pinch, pinch.Length);
            System.Array.Copy(other.joints, joints, joints.Length);
        }
    }
}
