// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using ElasticSea.QuestBridge.Input;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.LowLevel;
using UnityEngine.Rendering;

namespace ElasticSea.QuestBridge
{
    // What the XR Plug-in Management loader runs: streams an unchanged scene's main camera (an XR
    // Origin camera driven by TrackedPoseDriver/XRI) instead of a QuestBridgeRig. Feeds the Input
    // System devices, adds a hidden left/right eye camera under the target camera that render into
    // the stereo texture, and submits each frame with the pose the target camera was rendered at.
    // The target camera itself keeps rendering to the Game view as the mirror.
    [DisallowMultipleComponent]
    public sealed class QuestBridgeXRDriver : MonoBehaviour
    {
        // The running driver (one per process); QuestBridgeHands/Audio/Microphone fall back to its backend.
        public static QuestBridgeXRDriver Active { get; private set; }

        // Raised when the eyes move to a new target camera (XR Origin offset handling hooks in here).
        public static event Action<Camera> CameraAttached;

        [Tooltip("Camera to stream; empty = Camera.main.")]
        public Camera targetCamera;
        [Range(1, 8)] public int antiAliasing = 1;

        public IQuestBridgeBackend Backend { get; private set; }
        public QuestBridgeState State { get; private set; }
        public QuestBridgeDeviceInfo DeviceInfo { get; private set; }
        public Camera TargetCamera { get; private set; }
        public Camera LeftEye { get; private set; }
        public Camera RightEye { get; private set; }
        public RenderTexture EyeTexture { get; private set; }
        // Newest head pose latched before rendering, and the pose the last frame was submitted with
        public QuestBridgePose LatchedPose { get; private set; } = QuestBridgePose.Identity;
        public QuestBridgePose RenderPose { get; private set; } = QuestBridgePose.Identity;
        public ulong FrameIndex { get; private set; }

        // Head poses relative to the target camera's parent (XR Origin's Camera Offset).
        public Transform TrackingSpace => TargetCamera != null ? TargetCamera.transform.parent : null;

        bool latched, leftRendered, rightRendered, hasDeviceInfo;
        readonly QuestBridgePacing pacing = new QuestBridgePacing();
        int latchedFrame = -1;

        // Hidden, scene-independent driver over b (owned: disposed on destroy).
        public static QuestBridgeXRDriver Create(IQuestBridgeBackend b)
        {
            var go = new GameObject("QuestBridge XR");
            if (Application.isPlaying)
                DontDestroyOnLoad(go);
            var driver = go.AddComponent<QuestBridgeXRDriver>();
            driver.Initialize(b);
            return driver;
        }

        public void Initialize(IQuestBridgeBackend b)
        {
            Shutdown();
            Backend = b;
            hasDeviceInfo = b.TryGetDeviceInfo(out var info);
            DeviceInfo = hasDeviceInfo ? info : QuestBridgeDeviceInfo.Quest3Default;
            pacing.Apply(DeviceInfo.refreshHz);
            if (Active != null && Active != this)
                Debug.LogWarning("[QuestBridge] a second QuestBridgeXRDriver started; the newest one wins.", this);
            Active = this;

            QuestBridgeInput.AddDevices();
            QuestBridgeInput.hapticHandler = b.SendHaptic;
            // Late latch inside the Input System's before-render update so TrackedPoseDriver sees the
            // newest pose this frame; onBeforeRender is the fallback when that update doesn't run.
            InputSystem.onBeforeUpdate += OnBeforeInputUpdate;
            Application.onBeforeRender += OnBeforeRender;
            Camera.onPostRender += OnCameraRendered;
            RenderPipelineManager.endCameraRendering += OnSrpCameraRendered;
            RenderPipelineManager.endContextRendering += OnSrpContextRendered;
        }

        public void Shutdown()
        {
            if (Backend == null)
                return;
            InputSystem.onBeforeUpdate -= OnBeforeInputUpdate;
            Application.onBeforeRender -= OnBeforeRender;
            Camera.onPostRender -= OnCameraRendered;
            RenderPipelineManager.endCameraRendering -= OnSrpCameraRendered;
            RenderPipelineManager.endContextRendering -= OnSrpContextRendered;
            if (QuestBridgeInput.hapticHandler == (Action<QuestBridgeHand, float, float>)Backend.SendHaptic)
                QuestBridgeInput.hapticHandler = null;
            QuestBridgeInput.RemoveDevices();
            Backend.Dispose();
            Backend = null;
            if (Active == this)
                Active = null;
            DetachEyes();
            QuestBridgeEyes.Release(EyeTexture);
            EyeTexture = null;
            pacing.Restore();
        }

        void OnDestroy() => Shutdown();

        void Update() => Tick();

        // Once per frame: state, device info, target camera, early head and controller samples.
        internal void Tick()
        {
            if (Backend == null)
                return;
            State = Backend.Poll();
            if (!hasDeviceInfo && Backend.TryGetDeviceInfo(out var info))
            {
                hasDeviceInfo = true;
                DeviceInfo = info;
                pacing.Apply(DeviceInfo.refreshHz);
                if (TargetCamera != null)
                    ConfigureEyes();
            }

            var cam = targetCamera != null ? targetCamera : Camera.main;
            if (cam != TargetCamera || (cam != null && LeftEye == null))
                Attach(cam);

            if (Backend.TryGetHeadPose(out var head))
                QuestBridgeInput.QueueHead(head, DeviceInfo.ipd, true);
            for (var h = QuestBridgeHand.Left; h <= QuestBridgeHand.Right; h++)
                QuestBridgeInput.QueueController(h, Backend.TryGetController(h, out var c) ? c : default);
        }

        void Attach(Camera cam)
        {
            DetachEyes();
            TargetCamera = cam;
            if (cam == null)
                return;
            LeftEye = EyeCamera(QuestBridgeEye.Left);
            RightEye = EyeCamera(QuestBridgeEye.Right);
            ConfigureEyes();
            CameraAttached?.Invoke(cam);
        }

        Camera EyeCamera(QuestBridgeEye eye)
        {
            var go = new GameObject(eye == QuestBridgeEye.Left ? "QuestBridge LeftEye" : "QuestBridge RightEye")
            {
                hideFlags = HideFlags.DontSave,
            };
            go.transform.SetParent(TargetCamera.transform, false);
            return go.AddComponent<Camera>();
        }

        void ConfigureEyes()
        {
            var rt = QuestBridgeEyes.EnsureTexture(EyeTexture, DeviceInfo, antiAliasing);
            if (rt != EyeTexture)
            {
                QuestBridgeEyes.Release(EyeTexture);
                EyeTexture = rt;
            }
            foreach (var eye in new[] { QuestBridgeEye.Left, QuestBridgeEye.Right })
            {
                var cam = eye == QuestBridgeEye.Left ? LeftEye : RightEye;
                QuestBridgeEyes.Configure(cam, eye, DeviceInfo, EyeTexture, TargetCamera.nearClipPlane, TargetCamera.farClipPlane, TargetCamera);
                // CopyFrom takes the tag too; the eyes must never become Camera.main
                cam.gameObject.tag = "Untagged";
                cam.depth = TargetCamera.depth - 1;
            }
        }

        void DetachEyes()
        {
            foreach (var cam in new[] { LeftEye, RightEye })
            {
                if (cam == null)
                    continue;
                cam.targetTexture = null;
                if (Application.isPlaying) Destroy(cam.gameObject);
                else DestroyImmediate(cam.gameObject);
            }
            LeftEye = RightEye = null;
            TargetCamera = null;
        }

        void OnBeforeInputUpdate()
        {
            if (InputState.currentUpdateType == InputUpdateType.BeforeRender)
                LatchPose();
        }

        void OnBeforeRender()
        {
            if (latchedFrame != Time.frameCount)
                LatchPose();
        }

        // Newest pose for this frame's render, queued to the HMD for TrackedPoseDriver.
        internal void LatchPose()
        {
            if (Backend == null || !Backend.TryGetHeadPose(out var pose))
                return;
            LatchedPose = pose;
            FrameIndex++;
            latched = true;
            latchedFrame = Time.frameCount;
            leftRendered = rightRendered = false;
            QuestBridgeInput.QueueHead(pose, DeviceInfo.ipd, true);
        }

        // After both eyes rendered: submit with the target camera's pose in tracking space, which is
        // exactly what the eyes rendered from (the latched pose once TrackedPoseDriver applied it).
        internal void Submit()
        {
            if (!latched || Backend == null || TargetCamera == null)
                return;
            latched = false;
            var t = TargetCamera.transform;
            RenderPose = new QuestBridgePose { position = t.localPosition, rotation = t.localRotation, timeNs = LatchedPose.timeNs };
            Backend.SubmitFrame(EyeTexture, RenderPose, FrameIndex);
        }

        void MarkRendered(Camera cam)
        {
            if (cam == null) return;
            if (cam == LeftEye) leftRendered = true;
            else if (cam == RightEye) rightRendered = true;
        }

        internal void OnCameraRendered(Camera cam)
        {
            MarkRendered(cam);
            if (leftRendered && rightRendered)
                Submit();
        }

        void OnSrpCameraRendered(ScriptableRenderContext context, Camera cam) => MarkRendered(cam);

        void OnSrpContextRendered(ScriptableRenderContext context, List<Camera> cameras)
        {
            if (leftRendered && rightRendered)
                Submit();
        }
    }
}
