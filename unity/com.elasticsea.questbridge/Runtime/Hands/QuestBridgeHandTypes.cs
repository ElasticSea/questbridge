using UnityEngine;

namespace ElasticSea.QuestBridge
{
    // OpenXR XR_EXT_hand_tracking joints, in XrHandJointEXT order (libquestbridge's order too).
    public enum QuestBridgeHandJoint
    {
        Palm, Wrist,
        ThumbMetacarpal, ThumbProximal, ThumbDistal, ThumbTip,
        IndexMetacarpal, IndexProximal, IndexIntermediate, IndexDistal, IndexTip,
        MiddleMetacarpal, MiddleProximal, MiddleIntermediate, MiddleDistal, MiddleTip,
        RingMetacarpal, RingProximal, RingIntermediate, RingDistal, RingTip,
        LittleMetacarpal, LittleProximal, LittleIntermediate, LittleDistal, LittleTip,
    }

    public enum QuestBridgeFinger { Index, Middle, Ring, Little }

    // Joint pose in Unity convention, tracking space. Like Unity XR Hands: +Z points toward the
    // fingertip, +Y out of the back of the hand.
    public struct QuestBridgeJointPose
    {
        public Vector3 position;
        public Quaternion rotation;
        public float radius;  // metres
    }

    // One hand, filled in place by IQuestBridgeBackend.TryGetHand (no per-frame allocation).
    public sealed class QuestBridgeHandState
    {
        public const int JointCount = 26;

        public readonly QuestBridgeHand hand;
        public bool tracked;
        public float confidence;     // 0..1
        public float scale;          // hand size relative to the bind skeleton
        public bool dominant;
        public bool aimValid;
        public QuestBridgePose aim;    // pointer ray: +Z = pointing direction
        public long timeNs;          // headset clock (sample time)
        public readonly float[] pinch = new float[4];  // QuestBridgeFinger order, 0..1
        public readonly QuestBridgeJointPose[] joints = new QuestBridgeJointPose[JointCount];

        public QuestBridgeHandState(QuestBridgeHand hand) => this.hand = hand;

        // Unity XR Hands joint index (XRHandJointID.ToIndex()): same order with wrist and palm swapped
        public static int XRHandsIndex(QuestBridgeHandJoint joint) =>
            joint == QuestBridgeHandJoint.Palm ? 1 : joint == QuestBridgeHandJoint.Wrist ? 0 : (int)joint;

        public QuestBridgeJointPose this[QuestBridgeHandJoint joint] => joints[(int)joint];
        public float Pinch(QuestBridgeFinger finger) => pinch[(int)finger];
        public QuestBridgePose Root => new QuestBridgePose { position = joints[1].position, rotation = joints[1].rotation, timeNs = timeNs };

        public void Clear()
        {
            tracked = dominant = aimValid = false;
            confidence = scale = 0f;
            aim = QuestBridgePose.Identity;
            timeNs = 0;
            System.Array.Clear(pinch, 0, pinch.Length);
            System.Array.Clear(joints, 0, joints.Length);
        }

        public void CopyFrom(QuestBridgeHandState other)
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
