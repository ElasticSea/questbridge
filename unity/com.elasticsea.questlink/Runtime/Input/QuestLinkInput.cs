using System;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Utilities;

namespace ElasticSea.QuestLink.Input
{
    // Owns the Input System devices for one headset and feeds them from backend poses.
#if UNITY_EDITOR
    [UnityEditor.InitializeOnLoad]
#endif
    public static class QuestLinkInput
    {
        public static QuestLinkHMD Hmd { get; private set; }
        public static QuestLinkController LeftController { get; private set; }
        public static QuestLinkController RightController { get; private set; }

        // Receives haptic impulses sent to either controller (hand, amplitude 0..1, seconds).
        public static Action<QuestLinkHand, float, float> hapticHandler;

        // InputSystem.trackingState values (UnityEngine.XR.InputTrackingState).
        const int TrackedPosition = 1, TrackedRotation = 2;

        static QuestLinkInput() => RegisterLayouts();

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void RuntimeInit() => RegisterLayouts();

        // Idempotent; re-run after the Input System is reset (e.g. InputTestFixture).
        public static void RegisterLayouts()
        {
            InputSystem.RegisterLayout<QuestLinkHMD>();
            InputSystem.RegisterLayout<QuestLinkController>();
        }

        public static void AddDevices()
        {
            if (Hmd == null || !Hmd.added)
                Hmd = InputSystem.AddDevice<QuestLinkHMD>("QuestLinkHMD");
            if (LeftController == null || !LeftController.added)
                LeftController = AddController(CommonUsages.LeftHand, "QuestLinkLeftController");
            if (RightController == null || !RightController.added)
                RightController = AddController(CommonUsages.RightHand, "QuestLinkRightController");
        }

        public static void RemoveDevices()
        {
            foreach (var d in new InputDevice[] { Hmd, LeftController, RightController })
                if (d != null && d.added)
                    InputSystem.RemoveDevice(d);
            Hmd = null;
            LeftController = RightController = null;
        }

        static QuestLinkController AddController(InternedString hand, string name)
        {
            var c = InputSystem.AddDevice<QuestLinkController>(name);
            InputSystem.SetDeviceUsage(c, hand);
            return c;
        }

        public static void QueueHead(QuestLinkPose head, float ipd, bool tracked)
        {
            if (Hmd == null || !Hmd.added)
                return;
            var r = head.rotation;
            InputSystem.QueueStateEvent(Hmd, new QuestLinkHMDInputState
            {
                trackingState = tracked ? TrackedPosition | TrackedRotation : 0,
                isTracked = tracked ? 1f : 0f,
                devicePosition = head.position,
                deviceRotation = r,
                centerEyePosition = head.position,
                centerEyeRotation = r,
                leftEyePosition = head.position + r * QuestLinkMath.EyeOffset(QuestLinkEye.Left, ipd),
                leftEyeRotation = r,
                rightEyePosition = head.position + r * QuestLinkMath.EyeOffset(QuestLinkEye.Right, ipd),
                rightEyeRotation = r,
            });
        }

        public static void QueueController(QuestLinkHand hand, in QuestLinkControllerState s)
        {
            var device = hand == QuestLinkHand.Left ? LeftController : RightController;
            if (device == null || !device.added)
                return;
            var tracking = (s.positionTracked ? TrackedPosition : 0) | (s.rotationTracked ? TrackedRotation : 0);
            InputSystem.QueueStateEvent(device, new QuestLinkControllerInputState
            {
                trackingState = s.connected ? tracking : 0,
                isTracked = s.connected && tracking != 0 ? 1f : 0f,
                devicePosition = s.pose.position,
                deviceRotation = s.pose.rotation,
                pointerPosition = s.pose.position,
                pointerRotation = s.pose.rotation,
                trigger = s.trigger,
                grip = s.grip,
                thumbstick = s.thumbstick,
                triggerPressed = s.trigger > 0.5f ? 1f : 0f,
                gripPressed = s.grip > 0.5f ? 1f : 0f,
                primaryButton = s.primaryButton ? 1f : 0f,
                secondaryButton = s.secondaryButton ? 1f : 0f,
                thumbstickClicked = s.thumbstickClick ? 1f : 0f,
                thumbstickTouched = s.thumbstickTouch ? 1f : 0f,
                primaryTouched = s.primaryTouch ? 1f : 0f,
                secondaryTouched = s.secondaryTouch ? 1f : 0f,
                triggerTouched = s.triggerTouch ? 1f : 0f,
                menu = s.menu ? 1f : 0f,
                thumbrestTouched = s.thumbrestTouch ? 1f : 0f,
            });
        }
    }
}
