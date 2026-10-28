// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using ElasticSea.QuestBridge.Input;
using ElasticSea.QuestBridge.Native;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Controls;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestBridge.Tests
{
    public class InputDeviceTests : InputTestFixture
    {
        const float Eps = 1e-5f;

        public override void Setup()
        {
            base.Setup();
            QuestBridgeInput.RegisterLayouts();
            QuestBridgeInput.AddDevices();
        }

        public override void TearDown()
        {
            QuestBridgeInput.RemoveDevices();
            QuestBridgeInput.hapticHandler = null;
            base.TearDown();
        }

        [Test]
        public void DevicesRegisterAsGenericXRDevices()
        {
            Assert.AreSame(QuestBridgeInput.Hmd, InputSystem.GetDevice<XRHMD>());
            Assert.AreSame(QuestBridgeInput.LeftController, XRController.leftHand);
            Assert.AreSame(QuestBridgeInput.RightController, XRController.rightHand);
            Assert.AreEqual(QuestBridgeHand.Left, QuestBridgeInput.LeftController.Hand);
            Assert.AreEqual(QuestBridgeHand.Right, QuestBridgeInput.RightController.Hand);
            Assert.IsTrue(QuestBridgeInput.Hmd.updateBeforeRender);
        }

        [Test]
        public void AddDevicesIsIdempotent()
        {
            QuestBridgeInput.AddDevices();
            Assert.AreEqual(3, InputSystem.devices.Count);
        }

        [Test]
        public void HeadPoseReachesHmdControls()
        {
            var rot = Quaternion.Euler(0, 90, 0);
            QuestBridgeInput.QueueHead(new QuestBridgePose { position = new Vector3(1, 2, 3), rotation = rot }, 0.064f, true);
            InputSystem.Update();

            var hmd = QuestBridgeInput.Hmd;
            Assert.AreEqual(new Vector3(1, 2, 3), hmd.centerEyePosition.ReadValue());
            Assert.AreEqual(new Vector3(1, 2, 3), hmd.devicePosition.ReadValue());
            Assert.AreEqual(rot, hmd.centerEyeRotation.ReadValue());
            Assert.AreEqual(3, hmd.trackingState.ReadValue());
            Assert.IsTrue(hmd.isTracked.isPressed);
            // Head turned 90 deg right: the left eye sits toward +Z.
            var left = hmd.leftEyePosition.ReadValue();
            Assert.AreEqual(1f, left.x, Eps);
            Assert.AreEqual(3.032f, left.z, Eps);
        }

        [Test]
        public void ControllerStateReachesControls()
        {
            QuestBridgeInput.QueueController(QuestBridgeHand.Right, new QuestBridgeControllerState
            {
                connected = true, positionTracked = true, rotationTracked = true,
                pose = new QuestBridgePose { position = new Vector3(0.2f, 1.2f, 0.3f), rotation = Quaternion.identity },
                trigger = 0.75f, grip = 0.25f, thumbstick = new Vector2(0.5f, -0.5f),
                primaryButton = true, menu = true,
            });
            InputSystem.Update();

            var c = QuestBridgeInput.RightController;
            Assert.AreEqual(new Vector3(0.2f, 1.2f, 0.3f), c.devicePosition.ReadValue());
            Assert.AreEqual(0.75f, ((AxisControl)c["trigger"]).ReadValue(), Eps);
            Assert.IsTrue(((ButtonControl)c["triggerPressed"]).isPressed);
            Assert.IsFalse(((ButtonControl)c["gripPressed"]).isPressed);
            Assert.IsTrue(((ButtonControl)c["primaryButton"]).isPressed);
            Assert.IsTrue(((ButtonControl)c["menu"]).isPressed);
            Assert.AreEqual(0.5f, ((StickControl)c["thumbstick"]).ReadValue().x, 1e-3f);
            Assert.IsFalse(((ButtonControl)QuestBridgeInput.LeftController["primaryButton"]).isPressed);
        }

        [TestCase("<XRHMD>/centerEyePosition")]
        [TestCase("<XRHMD>/centerEyeRotation")]
        [TestCase("<XRController>{LeftHand}/devicePosition")]
        [TestCase("<XRController>{RightHand}/deviceRotation")]
        [TestCase("<XRController>{LeftHand}/pointerPosition")]
        [TestCase("<XRController>{LeftHand}/trigger")]
        [TestCase("<XRController>{LeftHand}/{TriggerButton}")]
        [TestCase("<XRController>{LeftHand}/{GripButton}")]
        [TestCase("<XRController>{LeftHand}/{Primary2DAxis}")]
        [TestCase("<XRController>{RightHand}/primary2DAxis")]
        [TestCase("<XRController>{RightHand}/{PrimaryButton}")]
        [TestCase("<XRController>{RightHand}/{MenuButton}")]
        [TestCase("<XRController>{RightHand}/{PrimaryTouch}")]
        [TestCase("<XRController>{LeftHand}/{TriggerTouch}")]
        [TestCase("<XRController>{LeftHand}/thumbrestTouched")]
        public void CommonXRBindingPathsResolve(string path)
        {
            Assert.IsNotNull(InputSystem.FindControl(path), path);
        }

        // qb_controller as libquestbridge fills it for the right controller with A pressed (sensors fixture "a")
        static QuestBridgeNative.qb_controller NativeRightA() => new QuestBridgeNative.qb_controller
        {
            pose = new QuestBridgeNative.qb_pose { qx = -0.337228f, qy = -0.596684f, qz = 0.300931f, qw = 0.663088f, px = -0.0655078f, py = -0.00378974f, pz = 0.645326f, time_ns = 355_000_000_000 },
            buttons = QuestBridgeNative.ButtonPrimary,
            touches = QuestBridgeNative.ButtonPrimary | QuestBridgeNative.TouchThumbrest,
            trigger = 0.25f, grip = 1f, stick_x = 0.5f, stick_y = -1f,
            flags = QuestBridgeNative.FlagConnected | QuestBridgeNative.FlagPositionTracked | QuestBridgeNative.FlagRotationTracked,
        };

        [Test]
        public void NativeControllerMapsToState()
        {
            Assert.IsTrue(NativeQuestBridge.FromNative(NativeRightA(), out var s));
            Assert.IsTrue(s.connected && s.positionTracked && s.rotationTracked);
            Assert.IsTrue(s.primaryButton && s.primaryTouch && s.thumbrestTouch);
            Assert.IsFalse(s.secondaryButton || s.thumbstickClick || s.menu || s.triggerTouch);
            Assert.AreEqual(new Vector2(0.5f, -1f), s.thumbstick);
            Assert.AreEqual(0.25f, s.trigger, Eps);
            // Link (right-handed, -Z forward) to Unity: z flips, quaternion x/y flip
            Assert.AreEqual(-0.645326f, s.pose.position.z, Eps);
            Assert.AreEqual(0.337228f, s.pose.rotation.x, Eps);
            Assert.AreEqual(0.663088f, s.pose.rotation.w, Eps);
            Assert.AreEqual(355_000_000_000L, s.pose.timeNs);
        }

        [Test]
        public void NativeControllerPutDownIsNotConnected()
        {
            var c = NativeRightA();
            c.flags = 0;  // controllers put down: hands tracked instead
            Assert.IsFalse(NativeQuestBridge.FromNative(c, out var s));
            Assert.IsFalse(s.connected);
        }

        [Test]
        public void NativeControllerReachesXRControls()
        {
            NativeQuestBridge.FromNative(NativeRightA(), out var s);
            QuestBridgeInput.QueueController(QuestBridgeHand.Right, s);
            InputSystem.Update();

            var c = XRController.rightHand;
            Assert.IsTrue(((ButtonControl)InputSystem.FindControl("<XRController>{RightHand}/{PrimaryButton}")).isPressed);
            Assert.IsTrue(((ButtonControl)c["thumbrestTouched"]).isPressed);
            Assert.IsTrue(((ButtonControl)c["gripPressed"]).isPressed);
            Assert.AreEqual(3, ((IntegerControl)c["trackingState"]).ReadValue());
            Assert.AreEqual(-1f, ((StickControl)c["thumbstick"]).ReadValue().y, 1e-3f);
        }

        [Test]
        public void HapticImpulseIsForwarded()
        {
            (QuestBridgeHand hand, float amp, float dur)? got = null;
            QuestBridgeInput.hapticHandler = (h, a, d) => got = (h, a, d);
            QuestBridgeInput.LeftController.SendImpulse(0.6f, 0.2f);
            Assert.IsTrue(got.HasValue);
            Assert.AreEqual(QuestBridgeHand.Left, got.Value.hand);
            Assert.AreEqual(0.6f, got.Value.amp, Eps);
            Assert.AreEqual(0.2f, got.Value.dur, Eps);
        }
    }
}
