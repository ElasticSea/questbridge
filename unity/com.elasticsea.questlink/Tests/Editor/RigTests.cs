using ElasticSea.QuestLink.Input;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.InputSystem;

namespace ElasticSea.QuestLink.Tests
{
    public class RigTests : InputTestFixture
    {
        const float Eps = 1e-5f;
        GameObject go;
        QuestLinkRig rig;
        MockQuestLink mock;

        public override void Setup()
        {
            base.Setup();
            QuestLinkInput.RegisterLayouts();
            go = new GameObject("Rig");
            rig = go.AddComponent<QuestLinkRig>();  // OnEnable doesn't run in Edit mode
            mock = new MockQuestLink { timeOverride = 2f };
            rig.Initialize(mock);
        }

        public override void TearDown()
        {
            rig.Shutdown();
            Object.DestroyImmediate(go);
            base.TearDown();
        }

        [Test]
        public void EyesShareOneEncodeSizedTexture()
        {
            var rt = rig.EyeTexture;
            Assert.AreEqual(4096, rt.width);
            Assert.AreEqual(2240, rt.height);
            Assert.AreSame(rt, rig.LeftEye.targetTexture);
            Assert.AreSame(rt, rig.RightEye.targetTexture);
        }

        [Test]
        public void EyesRenderStraightIntoTheTexture()
        {
            // No intermediate target that would reach EyeTexture only after the submit
            foreach (var cam in new[] { rig.LeftEye, rig.RightEye })
            {
                Assert.IsFalse(cam.allowHDR);
                Assert.IsFalse(cam.allowMSAA);
                Assert.IsFalse(cam.allowDynamicResolution);
            }
        }

        [Test]
        public void LeftEyeLeftHalfRightEyeRightHalf()
        {
            Assert.AreEqual(new Rect(0, 0, 0.5f, 1), rig.LeftEye.rect);
            Assert.AreEqual(new Rect(0.5f, 0, 0.5f, 1), rig.RightEye.rect);
            Assert.AreEqual(new Rect(0, 0, 2048, 2240), rig.LeftEye.pixelRect);
            Assert.AreEqual(new Rect(2048, 0, 2048, 2240), rig.RightEye.pixelRect);
            Assert.AreEqual(new RectInt(2048, 0, 2048, 2240), QuestLinkMath.EyePixelRect(QuestLinkEye.Right, 4096, 2240));
        }

        [Test]
        public void EyesUseOffAxisProjectionAndIpd()
        {
            var info = QuestLinkDeviceInfo.Quest3Default;
            Assert.AreEqual(QuestLinkMath.Projection(info.leftFov, rig.nearClip, rig.farClip), rig.LeftEye.projectionMatrix);
            Assert.AreEqual(QuestLinkMath.Projection(info.rightFov, rig.nearClip, rig.farClip), rig.RightEye.projectionMatrix);
            Assert.AreEqual(-info.ipd / 2, rig.LeftEye.transform.localPosition.x, Eps);
            Assert.AreEqual(info.ipd / 2, rig.RightEye.transform.localPosition.x, Eps);
            Assert.AreSame(rig.Head, rig.LeftEye.transform.parent);
        }

        [Test]
        public void SubmitSendsThePoseTheFrameWasRenderedWith()
        {
            rig.LatchPose();
            var latched = MockQuestLink.HeadAt(2f);
            Assert.AreEqual(latched.position, rig.Head.localPosition);
            Assert.Less(Quaternion.Angle(latched.rotation, rig.Head.localRotation), 1e-3f);
            Assert.AreEqual(1UL, rig.FrameIndex);

            mock.timeOverride = 3f;  // head moved after rendering started
            rig.Submit();
            Assert.AreEqual(1UL, mock.SubmittedFrames);
            Assert.AreEqual(1UL, mock.LastFrameIndex);
            Assert.AreEqual(latched.position, mock.LastRenderPose.position);
            Assert.AreEqual(latched.rotation, mock.LastRenderPose.rotation);
            Assert.AreEqual(latched.timeNs, mock.LastRenderPose.timeNs);
            Assert.AreSame(rig.EyeTexture, mock.LastTexture);

            rig.Submit();  // nothing new rendered
            Assert.AreEqual(1UL, mock.SubmittedFrames);

            rig.LatchPose();
            rig.Submit();
            Assert.AreEqual(2UL, mock.LastFrameIndex);
            Assert.AreEqual(MockQuestLink.HeadAt(3f).position, mock.LastRenderPose.position);
        }

        [Test]
        public void SubmitWaitsForBothEyes()
        {
            rig.LatchPose();
            rig.OnCameraRendered(rig.RightEye);
            Assert.AreEqual(0UL, mock.SubmittedFrames);
            rig.OnCameraRendered(rig.LeftEye);
            Assert.AreEqual(1UL, mock.SubmittedFrames);
            rig.OnCameraRendered(rig.LeftEye);
            rig.OnCameraRendered(rig.RightEye);
            Assert.AreEqual(1UL, mock.SubmittedFrames, "one submit per latched pose");
        }

        [Test]
        public void LatchFeedsTheHmdDevice()
        {
            rig.LatchPose();
            InputSystem.Update();
            var p = QuestLinkInput.Hmd.centerEyePosition.ReadValue();
            Assert.AreEqual(MockQuestLink.HeadAt(2f).position.y, p.y, Eps);
        }

        [Test]
        public void ShutdownRemovesDevicesAndTexture()
        {
            rig.Shutdown();
            Assert.IsNull(rig.EyeTexture);
            Assert.IsNull(InputSystem.GetDevice<QuestLinkHMD>());
            Assert.IsNull(QuestLinkInput.hapticHandler);
        }
    }
}
