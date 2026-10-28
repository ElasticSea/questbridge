using System.Collections.Generic;
using ElasticSea.QuestLink.Input;
using UnityEngine;
using UnityEngine.Rendering;

namespace ElasticSea.QuestLink
{
    // Put on the tracking-origin object. Builds Head/LeftEye/RightEye children, renders both eyes
    // into one encode-sized texture (left half / right half), late-latches the head pose in
    // Application.onBeforeRender and submits each frame with the exact pose it was rendered from.
    [DisallowMultipleComponent]
    public class QuestLinkRig : MonoBehaviour
    {
        public enum BackendMode { Auto, Native, Mock }

        [Tooltip("Auto: libquestlink if it loads, else the mock backend.")]
        public BackendMode backend = BackendMode.Auto;
        public float nearClip = 0.05f;
        public float farClip = 1000f;
        [Tooltip("Optional camera whose settings (clear flags, background, culling mask, ...) the eye cameras copy.")]
        public Camera cameraTemplate;
        [Range(1, 8)] public int antiAliasing = 1;
        [Tooltip("Draw the stereo texture into the Game view.")]
        public bool mirrorToScreen = true;

        public IQuestLinkBackend Backend { get; private set; }
        public QuestLinkState State { get; private set; }
        public QuestLinkDeviceInfo DeviceInfo { get; private set; }
        public Transform Head { get; private set; }
        public Camera LeftEye { get; private set; }
        public Camera RightEye { get; private set; }
        public RenderTexture EyeTexture { get; private set; }

        // Pose the current frame is rendered with, and its index; sent together on submit.
        public QuestLinkPose RenderPose { get; private set; } = QuestLinkPose.Identity;
        public ulong FrameIndex { get; private set; }

        bool latched, leftRendered, rightRendered;
        bool hasDeviceInfo;

        void OnEnable()
        {
            var b = CreateBackend(backend);
            if (b == null)
            {
                Debug.LogError("[QuestLink] libquestlink not available; set the rig to Auto or Mock.", this);
                enabled = false;
                return;
            }
            Initialize(b);
        }

        void OnDisable() => Shutdown();

        public static IQuestLinkBackend CreateBackend(BackendMode mode)
        {
            if (mode == BackendMode.Mock)
                return new MockQuestLink();
            var native = NativeQuestLink.TryCreate();
            if (native != null || mode == BackendMode.Native)
                return native;
            Debug.Log("[QuestLink] libquestlink not found, using the mock backend.");
            return new MockQuestLink();
        }

        // Also the entry point for tests and custom backends.
        public void Initialize(IQuestLinkBackend b)
        {
            Shutdown();
            Backend = b;
            hasDeviceInfo = b.TryGetDeviceInfo(out var info);
            ConfigureEyes(hasDeviceInfo ? info : QuestLinkDeviceInfo.Quest3Default);

            QuestLinkInput.AddDevices();
            QuestLinkInput.hapticHandler = b.SendHaptic;
            Application.onBeforeRender += LatchPose;
            // Submit once both eyes have rendered: Camera.onPostRender (built-in) or the end of the
            // SRP context. WaitForEndOfFrame is avoided: it doesn't fire in batchmode or with a hidden Game view.
            Camera.onPostRender += OnCameraRendered;
            RenderPipelineManager.endCameraRendering += OnSrpCameraRendered;
            RenderPipelineManager.endContextRendering += OnSrpContextRendered;
        }

        public void Shutdown()
        {
            if (Backend == null)
                return;
            Application.onBeforeRender -= LatchPose;
            Camera.onPostRender -= OnCameraRendered;
            RenderPipelineManager.endCameraRendering -= OnSrpCameraRendered;
            RenderPipelineManager.endContextRendering -= OnSrpContextRendered;
            if (QuestLinkInput.hapticHandler == (System.Action<QuestLinkHand, float, float>)Backend.SendHaptic)
                QuestLinkInput.hapticHandler = null;
            QuestLinkInput.RemoveDevices();
            Backend.Dispose();
            Backend = null;
            ReleaseTexture();
        }

        void ConfigureEyes(QuestLinkDeviceInfo info)
        {
            DeviceInfo = info;
            if (EyeTexture == null || EyeTexture.width != info.encodeWidth || EyeTexture.height != info.encodeHeight ||
                EyeTexture.antiAliasing != antiAliasing)
            {
                ReleaseTexture();
                EyeTexture = new RenderTexture(info.encodeWidth, info.encodeHeight, 24, RenderTextureFormat.BGRA32, RenderTextureReadWrite.sRGB)
                {
                    name = "QuestLink Eyes",
                    antiAliasing = antiAliasing,
                };
                EyeTexture.Create();
            }

            Head = Child(transform, "Head");
            LeftEye = EyeCamera(QuestLinkEye.Left);
            RightEye = EyeCamera(QuestLinkEye.Right);
        }

        Camera EyeCamera(QuestLinkEye eye)
        {
            var t = Child(Head, eye == QuestLinkEye.Left ? "LeftEye" : "RightEye");
            t.localPosition = QuestLinkMath.EyeOffset(eye, DeviceInfo.ipd);
            t.localRotation = Quaternion.identity;
            var cam = t.GetComponent<Camera>();
            if (cam == null)
                cam = t.gameObject.AddComponent<Camera>();
            if (cameraTemplate != null && cameraTemplate != cam)
                cam.CopyFrom(cameraTemplate);
            cam.stereoTargetEye = StereoTargetEyeMask.None;
            cam.targetTexture = EyeTexture;
            cam.rect = QuestLinkMath.EyeViewport(eye);
            cam.nearClipPlane = nearClip;
            cam.farClipPlane = farClip;
            cam.projectionMatrix = QuestLinkMath.Projection(DeviceInfo.Fov(eye), nearClip, farClip);
            return cam;
        }

        static Transform Child(Transform parent, string name)
        {
            var t = parent.Find(name);
            if (t != null)
                return t;
            t = new GameObject(name).transform;
            t.SetParent(parent, false);
            return t;
        }

        void Update()
        {
            if (Backend == null)
                return;
            State = Backend.Poll();
            if (!hasDeviceInfo && Backend.TryGetDeviceInfo(out var info))
            {
                hasDeviceInfo = true;
                ConfigureEyes(info);
            }

            // Early head sample for gameplay this frame; LatchPose refreshes it before rendering.
            if (Backend.TryGetHeadPose(out var head))
                QuestLinkInput.QueueHead(head, DeviceInfo.ipd, true);
            for (var h = QuestLinkHand.Left; h <= QuestLinkHand.Right; h++)
                QuestLinkInput.QueueController(h, Backend.TryGetController(h, out var c) ? c : default);
        }

        // Application.onBeforeRender: newest pose onto the head, remembered for this frame's submit.
        internal void LatchPose()
        {
            if (Backend == null || !Backend.TryGetHeadPose(out var pose))
                return;
            Head.localPosition = pose.position;
            Head.localRotation = pose.rotation;
            RenderPose = pose;
            FrameIndex++;
            latched = true;
            leftRendered = rightRendered = false;
            QuestLinkInput.QueueHead(pose, DeviceInfo.ipd, true);
        }

        // After both eye cameras rendered this frame; at most once per latched pose.
        internal void Submit()
        {
            if (!latched || Backend == null)
                return;
            latched = false;
            Backend.SubmitFrame(EyeTexture, RenderPose, FrameIndex);
        }

        void MarkRendered(Camera cam)
        {
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

        // SRP cameras are only executed once their context is submitted, so wait for the context end.
        void OnSrpContextRendered(ScriptableRenderContext context, List<Camera> cameras)
        {
            if (leftRendered && rightRendered)
                Submit();
        }

        void OnValidate()
        {
            if (Backend != null && Head != null)
                ConfigureEyes(DeviceInfo);
        }

        void OnGUI()
        {
            if (!mirrorToScreen || EyeTexture == null || Event.current.type != EventType.Repaint)
                return;
            var aspect = (float)EyeTexture.width / EyeTexture.height;
            var w = Mathf.Min(Screen.width, Screen.height * aspect);
            var h = w / aspect;
            GUI.DrawTexture(new Rect((Screen.width - w) * 0.5f, (Screen.height - h) * 0.5f, w, h), EyeTexture, ScaleMode.ScaleToFit, false);
        }

        void ReleaseTexture()
        {
            if (EyeTexture == null)
                return;
            foreach (var cam in new[] { LeftEye, RightEye })
                if (cam != null)
                    cam.targetTexture = null;
            EyeTexture.Release();
            if (Application.isPlaying) Destroy(EyeTexture);
            else DestroyImmediate(EyeTexture);
            EyeTexture = null;
        }
    }
}
