// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using System.Collections.Generic;
using ElasticSea.QuestBridge.Input;
using ElasticSea.QuestBridge.XRManagement;
using NUnit.Framework;
using Unity.XR.CoreUtils;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.XR;
using UnityEngine.TestTools;
using UnityEngine.XR.Management;

namespace ElasticSea.QuestBridge.Tests
{
    // An unchanged XR Origin + TrackedPoseDriver scene with only the QuestBridge loader active
    // (as if ticked in XR Plug-in Management), mock backend as the transport.
    public class XRLoaderPlayModeTests
    {
        const float Eps = 1e-4f;
        XRManagerSettings manager;
        QuestBridgeLoader loader;
        MockQuestBridge mock;
        GameObject originGo;
        XROrigin origin;
        Camera cam;

        [SetUp]
        public void SetUp()
        {
            mock = new MockQuestBridge { timeOverride = 2f };
            QuestBridgeLoader.backendFactory = () => mock;
            manager = ScriptableObject.CreateInstance<XRManagerSettings>();
            loader = ScriptableObject.CreateInstance<QuestBridgeLoader>();
            // TryAddLoader refuses loaders that weren't registered before Play mode
#pragma warning disable 618
            manager.loaders = new List<XRLoader> { loader };
#pragma warning restore 618

            // XR Origin (XR Rig) like XRI's starter assets: Camera Offset under the origin, camera with a TrackedPoseDriver
            originGo = new GameObject("XR Origin");
            originGo.SetActive(false);
            var offset = new GameObject("Camera Offset");
            offset.transform.SetParent(originGo.transform, false);
            var camGo = new GameObject("Main Camera") { tag = "MainCamera" };
            camGo.transform.SetParent(offset.transform, false);
            cam = camGo.AddComponent<Camera>();
            var tpd = camGo.AddComponent<TrackedPoseDriver>();
            tpd.positionInput = new InputActionProperty(new InputAction(binding: "<XRHMD>/centerEyePosition", expectedControlType: "Vector3"));
            tpd.rotationInput = new InputActionProperty(new InputAction(binding: "<XRHMD>/centerEyeRotation", expectedControlType: "Quaternion"));
            origin = originGo.AddComponent<XROrigin>();
            origin.Origin = originGo;
            origin.CameraFloorOffsetObject = offset;
            origin.Camera = cam;
            origin.CameraYOffset = 1.1176f;
            offset.transform.localPosition = new Vector3(0f, 5f, 0f);  // the loader must set it
            originGo.SetActive(true);
        }

        [TearDown]
        public void TearDown()
        {
            if (manager.isInitializationComplete)
            {
                manager.StopSubsystems();
                manager.DeinitializeLoader();
            }
            QuestBridgeLoader.backendFactory = null;
            Object.Destroy(originGo);
            Object.Destroy(manager);
            Object.Destroy(loader);
        }

        IEnumerator StartXR()
        {
            manager.InitializeLoaderSync();
            Assert.AreSame(loader, manager.activeLoader, "QuestBridge loader didn't initialize");
            manager.StartSubsystems();
            Assert.IsNotNull(loader.Driver);
            yield return null;
        }

        // Batchmode skips onBeforeRender and camera rendering; drive one frame by hand.
        void RenderFrame()
        {
            if (!Application.isBatchMode)
                return;
            var d = loader.Driver;
            d.LatchPose();
            d.LeftEye.Render();
            d.RightEye.Render();
        }

        [UnityTest]
        public IEnumerator TrackedPoseDriverFollowsTheHeadsetAndFramesSubmit()
        {
            yield return StartXR();
            var head = MockQuestBridge.HeadAt(2f);
            for (var i = 0; i < 10; i++)
            {
                yield return null;
                RenderFrame();
            }

            var d = loader.Driver;
            Assert.AreSame(cam, d.TargetCamera);
            Assert.AreSame(cam.transform, d.LeftEye.transform.parent);
            Assert.That(Vector3.Distance(head.position, cam.transform.localPosition), Is.LessThan(Eps), "camera not driven by <XRHMD>");
            Assert.That(Quaternion.Angle(head.rotation, cam.transform.localRotation), Is.LessThan(0.01f));
            Assert.GreaterOrEqual(mock.SubmittedFrames, 5UL);
            Assert.AreEqual(d.FrameIndex, mock.LastFrameIndex);
            Assert.AreSame(d.EyeTexture, mock.LastTexture);
            Assert.That(Vector3.Distance(head.position, mock.LastRenderPose.position), Is.LessThan(Eps));
        }

        [UnityTest]
        public IEnumerator CameraOffsetFollowsTheTrackingOrigin()
        {
            yield return StartXR();
            yield return null;
            Assert.AreEqual(1.1176f, origin.CameraFloorOffsetObject.transform.localPosition.y, Eps);
            QuestBridgeLoader.ApplyTrackingOrigin(cam, QuestBridgeLoaderSettings.TrackingOrigin.Floor);
            Assert.AreEqual(0f, origin.CameraFloorOffsetObject.transform.localPosition.y, Eps);
        }

        [UnityTest]
        public IEnumerator ControllersBindAsXRControllers()
        {
            yield return StartXR();
            var action = new InputAction(binding: "<XRController>{LeftHand}/devicePosition");
            action.Enable();
            yield return null;
            yield return null;
            Assert.AreSame(QuestBridgeInput.LeftController, XRController.leftHand);
            Assert.AreSame(QuestBridgeInput.RightController, XRController.rightHand);
            mock.TryGetController(QuestBridgeHand.Left, out var left);
            Assert.That(Vector3.Distance(left.pose.position, action.ReadValue<Vector3>()), Is.LessThan(Eps));
            action.Dispose();
            QuestBridgeInput.LeftController.SendImpulse(0.7f, 0.2f);
            Assert.AreEqual(QuestBridgeHand.Left, mock.LastHaptic?.hand);
        }

#if QUESTBRIDGE_XR_HANDS
        [UnityTest]
        public IEnumerator HandSubsystemRuns()
        {
            yield return StartXR();
            yield return null;
            yield return null;
            var hands = new List<UnityEngine.XR.Hands.XRHandSubsystem>();
            SubsystemManager.GetSubsystems(hands);
            var sub = hands.Find(h => h.running && h.subsystemDescriptor.id == "QuestBridge-Hands");
            Assert.IsNotNull(sub, "no running QuestBridge XRHandSubsystem");
            Assert.IsTrue(sub.leftHand.isTracked);
        }
#endif

        [UnityTest]
        public IEnumerator StopRemovesEverything()
        {
            yield return StartXR();
            yield return null;
            manager.StopSubsystems();
            manager.DeinitializeLoader();
            yield return null;
            Assert.IsNull(QuestBridgeXRDriver.Active);
            Assert.IsNull(InputSystem.GetDevice<XRHMD>());
            Assert.AreEqual(0, cam.transform.childCount);
        }
    }
}
