using Unity.Collections;
using UnityEngine;
using UnityEngine.Scripting;
using UnityEngine.XR.Hands;
using UnityEngine.XR.Hands.ProviderImplementation;

namespace ElasticSea.QuestLink.XRHands
{
    // Unity XR Hands (com.unity.xr.hands) provider fed by QuestLinkHands.Active, so XR Hands
    // visualizers and interaction code work unchanged. Only compiled when the project has XR
    // Hands 1.3+. Add this component next to QuestLinkHands; parent hand visuals to the rig
    // (joint poses are relative to the tracking origin, like an XR Origin).
    public class QuestLinkXRHands : MonoBehaviour
    {
        public const string DescriptorId = "QuestLink-Hands";

        public XRHandSubsystem Subsystem { get; private set; }
        XRHandProviderUtility.SubsystemUpdater updater;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void Register() => XRHandSubsystemDescriptor.Register(new XRHandSubsystemDescriptor.Cinfo
        {
            id = DescriptorId,
            providerType = typeof(Provider),
        });

        void OnEnable()
        {
            var descriptors = new System.Collections.Generic.List<XRHandSubsystemDescriptor>();
            SubsystemManager.GetSubsystemDescriptors(descriptors);
            var descriptor = descriptors.Find(d => d.id == DescriptorId);
            if (descriptor == null)
            {
                Register();
                SubsystemManager.GetSubsystemDescriptors(descriptors);
                descriptor = descriptors.Find(d => d.id == DescriptorId);
            }
            Subsystem = descriptor?.Create();
            if (Subsystem == null)
            {
                Debug.LogError("[QuestLink] could not create the XR Hands subsystem.", this);
                enabled = false;
                return;
            }
            Subsystem.Start();
            // Updates the subsystem in the XR player loop step and before rendering
            updater = new XRHandProviderUtility.SubsystemUpdater(Subsystem);
            updater.Start();
        }

        void OnDisable()
        {
            updater?.Destroy();
            updater = null;
            Subsystem?.Destroy();
            Subsystem = null;
        }

        [Preserve]
        class Provider : XRHandSubsystemProvider
        {
            public override void Start() { }
            public override void Stop() { }
            public override void Destroy() { }

            public override void GetHandLayout(NativeArray<bool> handJointsInLayout)
            {
                for (var i = 0; i < handJointsInLayout.Length; i++)
                    handJointsInLayout[i] = true;
            }

            public override XRHandSubsystem.UpdateSuccessFlags TryUpdateHands(XRHandSubsystem.UpdateType updateType,
                ref Pose leftHandRootPose, NativeArray<XRHandJoint> leftHandJoints,
                ref Pose rightHandRootPose, NativeArray<XRHandJoint> rightHandJoints)
            {
                var hands = QuestLinkHands.Active;
                if (hands == null)
                    return XRHandSubsystem.UpdateSuccessFlags.None;
                hands.Refresh();
                var flags = XRHandSubsystem.UpdateSuccessFlags.None;
                if (Fill(hands.Left, Handedness.Left, ref leftHandRootPose, leftHandJoints))
                    flags |= XRHandSubsystem.UpdateSuccessFlags.LeftHandRootPose | XRHandSubsystem.UpdateSuccessFlags.LeftHandJoints;
                if (Fill(hands.Right, Handedness.Right, ref rightHandRootPose, rightHandJoints))
                    flags |= XRHandSubsystem.UpdateSuccessFlags.RightHandRootPose | XRHandSubsystem.UpdateSuccessFlags.RightHandJoints;
                return flags;
            }

            static bool Fill(QuestLinkHandState state, Handedness handedness, ref Pose root, NativeArray<XRHandJoint> joints)
            {
                if (!state.tracked)
                    return false;
                var wrist = state[QuestLinkHandJoint.Wrist];
                root = new Pose(wrist.position, wrist.rotation);
                for (var j = 0; j < QuestLinkHandState.JointCount; j++)
                {
                    var joint = (QuestLinkHandJoint)j;
                    var index = QuestLinkHandState.XRHandsIndex(joint);
                    if (index >= joints.Length)
                        continue;
                    var p = state.joints[j];
                    joints[index] = XRHandProviderUtility.CreateJoint(handedness,
                        XRHandJointTrackingState.Pose | XRHandJointTrackingState.Radius,
                        XRHandJointIDUtility.FromIndex(index), new Pose(p.position, p.rotation), p.radius);
                }
                return true;
            }
        }
    }
}
