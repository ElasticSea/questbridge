// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using ElasticSea.QuestBridge.Input;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;

namespace ElasticSea.QuestBridge.Tests
{
    // QuestBridgeXRDriver (what the XR loader runs) on an XR Origin-like camera, mock backend.
    public class XRDriverTests : InputTestFixture
    {
        const float Eps = 1e-5f;
        GameObject origin;
        Camera cam;
        QuestBridgeXRDriver driver;
        MockQuestBridge mock;

        public override void Setup()
        {
            base.Setup();
            QuestBridgeInput.RegisterLayouts();
            origin = new GameObject("XR Origin");
            var offset = new GameObject("Camera Offset").transform;
            offset.SetParent(origin.transform, false);
            offset.localPosition = new Vector3(0f, 1.1176f, 0f);
            cam = new GameObject("Main Camera").AddComponent<Camera>();
            cam.tag = "MainCamera";
            cam.transform.SetParent(offset, false);
            cam.nearClipPlane = 0.1f;
            cam.farClipPlane = 200f;
            cam.backgroundColor = Color.magenta;

            mock = new MockQuestBridge { timeOverride = 2f };
            driver = new GameObject("Driver").AddComponent<QuestBridgeXRDriver>();  // OnEnable/Update don't run in Edit mode
            driver.Initialize(mock);
            // Explicit: the Edit mode scene may hold its own Main Camera (Camera.main is the default)
            driver.targetCamera = cam;
            driver.Tick();
        }

        public override void TearDown()
        {
            var go = driver.gameObject;
            driver.Shutdown();
            Object.DestroyImmediate(go);
            Object.DestroyImmediate(origin);
            base.TearDown();
        }

        [Test]
        public void EyesAttachUnderTheMainCamera()
        {
            Assert.AreSame(driver, QuestBridgeXRDriver.Active);
            Assert.AreSame(cam, driver.TargetCamera);
            Assert.AreSame(cam.transform, driver.LeftEye.transform.parent);
            Assert.AreSame(cam.transform, driver.RightEye.transform.parent);
            Assert.AreSame(cam.transform.parent, driver.TrackingSpace);
            // The eyes never take over Camera.main
            Assert.AreEqual("Untagged", driver.LeftEye.tag);
            Assert.AreEqual("Untagged", driver.RightEye.tag);
            Assert.Less(driver.LeftEye.depth, cam.depth);
        }

        [Test]
        public void EyesCopyTheCameraAndRenderSideBySide()
        {
            var info = QuestBridgeDeviceInfo.Quest3Default;
            var rt = driver.EyeTexture;
            Assert.AreEqual(4096, rt.width);
            Assert.AreEqual(2240, rt.height);
            Assert.AreSame(rt, driver.LeftEye.targetTexture);
            Assert.AreSame(rt, driver.RightEye.targetTexture);
            Assert.IsNull(cam.targetTexture, "the target camera stays the Game view mirror");
            Assert.AreEqual(new Rect(0, 0, 0.5f, 1), driver.LeftEye.rect);
            Assert.AreEqual(new Rect(0.5f, 0, 0.5f, 1), driver.RightEye.rect);
            Assert.AreEqual(Color.magenta, driver.LeftEye.backgroundColor);
            Assert.AreEqual(QuestBridgeMath.Projection(info.leftFov, 0.1f, 200f), driver.LeftEye.projectionMatrix);
            Assert.AreEqual(QuestBridgeMath.Projection(info.rightFov, 0.1f, 200f), driver.RightEye.projectionMatrix);
            Assert.AreEqual(-info.ipd / 2, driver.LeftEye.transform.localPosition.x, Eps);
            Assert.AreEqual(info.ipd / 2, driver.RightEye.transform.localPosition.x, Eps);
            Assert.AreEqual(Quaternion.identity, driver.RightEye.transform.localRotation);
            Assert.IsFalse(driver.LeftEye.allowHDR);
            Assert.IsFalse(driver.LeftEye.allowMSAA);
        }

        [Test]
        public void TargetCameraOverrideMovesTheEyes()
        {
            var other = new GameObject("Other").AddComponent<Camera>();
            try
            {
                driver.targetCamera = other;
                driver.Tick();
                Assert.AreSame(other, driver.TargetCamera);
                Assert.AreSame(other.transform, driver.LeftEye.transform.parent);
                Assert.AreEqual(0, cam.transform.childCount, "old eyes removed");
            }
            finally
            {
                driver.targetCamera = cam;
                driver.Tick();
                Object.DestroyImmediate(other.gameObject);
            }
        }

        [Test]
        public void LatchQueuesTheHeadForTrackedPoseDriver()
        {
            driver.LatchPose();
            InputSystem.Update();
            var head = MockQuestBridge.HeadAt(2f);
            Assert.AreEqual(head.position, QuestBridgeInput.Hmd.centerEyePosition.ReadValue());
            Assert.AreEqual(1UL, driver.FrameIndex);
            Assert.AreEqual(head.timeNs, driver.LatchedPose.timeNs);
        }

        [Test]
        public void SubmitSendsTheCameraTrackingSpacePoseOnceBothEyesRendered()
        {
            driver.LatchPose();
            // TrackedPoseDriver would have applied the latched pose to the camera's local transform
            var head = MockQuestBridge.HeadAt(2f);
            cam.transform.localPosition = head.position;
            cam.transform.localRotation = head.rotation;

            driver.OnCameraRendered(driver.LeftEye);
            Assert.AreEqual(0UL, mock.SubmittedFrames, "only after both eyes");
            driver.OnCameraRendered(cam);
            Assert.AreEqual(0UL, mock.SubmittedFrames, "the mirror camera doesn't count");
            driver.OnCameraRendered(driver.RightEye);
            Assert.AreEqual(1UL, mock.SubmittedFrames);
            Assert.AreSame(driver.EyeTexture, mock.LastTexture);
            Assert.AreEqual(driver.FrameIndex, mock.LastFrameIndex);
            Assert.AreEqual(head.position, mock.LastRenderPose.position);
            Assert.Less(Quaternion.Angle(head.rotation, mock.LastRenderPose.rotation), 0.01f);
            Assert.AreEqual(head.timeNs, mock.LastRenderPose.timeNs);

            driver.OnCameraRendered(driver.LeftEye);
            driver.OnCameraRendered(driver.RightEye);
            Assert.AreEqual(1UL, mock.SubmittedFrames, "one submit per latched pose");
        }

        [Test]
        public void SubmitUsesWhatTheCameraRenderedEvenIfNotTheLatchedPose()
        {
            driver.LatchPose();
            cam.transform.localPosition = new Vector3(1f, 2f, 3f);
            driver.OnCameraRendered(driver.LeftEye);
            driver.OnCameraRendered(driver.RightEye);
            Assert.AreEqual(new Vector3(1f, 2f, 3f), mock.LastRenderPose.position);
        }

        [Test]
        public void TickFeedsControllersAndHapticsReachTheBackend()
        {
            InputSystem.Update();
            Assert.AreSame(QuestBridgeInput.LeftController, XRController.leftHand);
            mock.TryGetController(QuestBridgeHand.Right, out var right);
            Assert.AreEqual(right.pose.position, QuestBridgeInput.RightController.devicePosition.ReadValue());
            QuestBridgeInput.RightController.SendImpulse(0.5f, 0.1f);
            Assert.AreEqual(QuestBridgeHand.Right, mock.LastHaptic?.hand);
        }

        [Test]
        public void HandsFallBackToTheDriver()
        {
            var go = new GameObject("Hands");
            try
            {
                var hands = go.AddComponent<QuestBridgeHands>();
                hands.Refresh();
                Assert.IsTrue(hands.HasData);
                Assert.IsTrue(hands.Left.tracked);
                Assert.AreSame(cam.transform.parent, hands.TrackingSpace);
            }
            finally { Object.DestroyImmediate(go); }
        }

        [Test]
        public void ShutdownRemovesDevicesAndEyes()
        {
            driver.Shutdown();
            Assert.IsNull(QuestBridgeXRDriver.Active);
            Assert.IsNull(QuestBridgeInput.Hmd);
            Assert.IsNull(InputSystem.GetDevice<XRHMD>());
            Assert.AreEqual(0, cam.transform.childCount);
            Assert.IsNull(driver.EyeTexture);
        }
    }
}
