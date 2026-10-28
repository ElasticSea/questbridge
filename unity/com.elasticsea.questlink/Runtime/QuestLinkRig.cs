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
        public enum MirrorMode { None, LeftEye, BothEyes }

        [Tooltip("Auto: libquestlink if it loads, else the mock backend.")]
        public BackendMode backend = BackendMode.Auto;
        public float nearClip = 0.05f;
        public float farClip = 1000f;
        [Tooltip("Optional camera whose settings (clear flags, background, culling mask, ...) the eye cameras copy.")]
        public Camera cameraTemplate;
        [Range(1, 8)] public int antiAliasing = 1;
        [Tooltip("What the Game view shows while playing: the left eye, both eyes side by side, or nothing.")]
        public MirrorMode mirror = MirrorMode.LeftEye;
        [Tooltip("While playing: eyes copy the Main Camera's settings (when no template is set) and the Main Camera (and its AudioListener, if the rig has one) is turned off.")]
        public bool takeOverMainCamera = true;
        [Tooltip("While playing: keep running when the Editor or player window loses focus, and render at the headset's refresh rate with vSync off.")]
        public bool headsetFramePacing = true;

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
        Camera mirrorCamera, hiddenMainCamera;
        AudioListener hiddenListener;
        readonly QuestLinkPacing pacing = new QuestLinkPacing();
        float stateSince;
        bool hintLogged;

        void OnEnable()
        {
            // Two streams to one headset: the XR Plug-in Management loader already drives Camera.main
            if (QuestLinkXRDriver.Active != null)
            {
                Debug.LogWarning("[QuestLink] The Quest Link XR loader is already streaming; turn it off in XR Plug-in Management or remove this rig.", this);
                enabled = false;
                return;
            }
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
            State = QuestLinkState.Idle;
            stateSince = Time.realtimeSinceStartup;
            hintLogged = false;
            var main = Application.isPlaying && takeOverMainCamera ? MainCameraToTakeOver() : null;
            if (main != null && cameraTemplate == null)
                cameraTemplate = main;
            hasDeviceInfo = b.TryGetDeviceInfo(out var info);
            ConfigureEyes(hasDeviceInfo ? info : QuestLinkDeviceInfo.Quest3Default);
            if (main != null)
                HideMainCamera(main);
            if (Application.isPlaying && mirror != MirrorMode.None)
                CreateMirrorCamera();

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
            // Without their texture the eyes would render to the display
            foreach (var cam in new[] { LeftEye, RightEye })
                if (cam != null)
                    cam.enabled = false;
            RestorePacing();
            RestoreMainCamera();
            if (mirrorCamera != null)
                Destroy(mirrorCamera.gameObject);
            mirrorCamera = null;
        }

        // The scene's own camera, unless it is part of this rig.
        Camera MainCameraToTakeOver()
        {
            var main = Camera.main;
            return main != null && !main.transform.IsChildOf(transform) ? main : null;
        }

        // Stops the Main Camera from rendering the scene a third time; its AudioListener goes too
        // when the rig brings its own (two listeners make Unity warn every frame).
        void HideMainCamera(Camera main)
        {
            main.enabled = false;
            hiddenMainCamera = main;
            // Scripts that use Camera.main (raycasts, billboards) get the left eye instead
            LeftEye.tag = "MainCamera";
            var listener = main.GetComponent<AudioListener>();
            var own = GetComponentInChildren<AudioListener>();
            if (listener != null && listener.enabled && own != null && own != listener)
            {
                listener.enabled = false;
                hiddenListener = listener;
            }
        }

        void RestoreMainCamera()
        {
            if (hiddenMainCamera != null)
            {
                hiddenMainCamera.enabled = true;
                if (LeftEye != null)
                    LeftEye.tag = "Untagged";
            }
            if (hiddenListener != null)
                hiddenListener.enabled = true;
            hiddenMainCamera = null;
            hiddenListener = null;
        }

        // Clears the Game view under the OnGUI mirror (no other camera may render to the display).
        void CreateMirrorCamera()
        {
            mirrorCamera = new GameObject("QuestLink Mirror") { hideFlags = HideFlags.DontSave }.AddComponent<Camera>();
            mirrorCamera.transform.SetParent(transform, false);
            mirrorCamera.clearFlags = CameraClearFlags.SolidColor;
            mirrorCamera.backgroundColor = Color.black;
            mirrorCamera.cullingMask = 0;
            mirrorCamera.depth = -100;
            mirrorCamera.stereoTargetEye = StereoTargetEyeMask.None;
        }

        void ApplyPacing()
        {
            if (headsetFramePacing)
                pacing.Apply(DeviceInfo.refreshHz);
        }

        void RestorePacing() => pacing.Restore();

        void ConfigureEyes(QuestLinkDeviceInfo info)
        {
            DeviceInfo = info;
            var rt = QuestLinkEyes.EnsureTexture(EyeTexture, info, antiAliasing);
            if (rt != EyeTexture)
            {
                ReleaseTexture();
                EyeTexture = rt;
            }

            Head = Child(transform, "Head");
            LeftEye = EyeCamera(QuestLinkEye.Left);
            RightEye = EyeCamera(QuestLinkEye.Right);
            ApplyPacing();
        }

        Camera EyeCamera(QuestLinkEye eye)
        {
            var t = Child(Head, eye == QuestLinkEye.Left ? "LeftEye" : "RightEye");
            var cam = t.GetComponent<Camera>();
            if (cam == null)
                cam = t.gameObject.AddComponent<Camera>();
            QuestLinkEyes.Configure(cam, eye, DeviceInfo, EyeTexture, nearClip, farClip, cameraTemplate);
            // Shutdown disables the eyes
            cam.enabled = true;
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
            var state = Backend.Poll();
            if (state != State)
            {
                Debug.Log($"[QuestLink] {state} ({Backend.Name})", this);
                State = state;
                stateSince = Time.realtimeSinceStartup;
                hintLogged = false;
            }
            else if (state != QuestLinkState.Streaming && !hintLogged && Time.realtimeSinceStartup - stateSince > 15f)
            {
                hintLogged = true;
                Debug.Log(state == QuestLinkState.Idle
                    ? "[QuestLink] No Quest on USB yet: plug it in and quit Meta Horizon Link on the PC (its service holds the headset's Link interface)."
                    : "[QuestLink] Quest found, waiting for Link: in the headset open Quick Settings > Link and press Launch.", this);
            }
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
            if (mirror == MirrorMode.None || EyeTexture == null || Event.current.type != EventType.Repaint)
                return;
            var both = mirror == MirrorMode.BothEyes;
            var aspect = (both ? 1f : 0.5f) * EyeTexture.width / EyeTexture.height;
            var w = Mathf.Min(Screen.width, Screen.height * aspect);
            var h = w / aspect;
            var rect = new Rect((Screen.width - w) * 0.5f, (Screen.height - h) * 0.5f, w, h);
            GUI.DrawTextureWithTexCoords(rect, EyeTexture, both ? new Rect(0f, 0f, 1f, 1f) : QuestLinkMath.EyeViewport(QuestLinkEye.Left), false);
        }

        void ReleaseTexture()
        {
            if (EyeTexture == null)
                return;
            foreach (var cam in new[] { LeftEye, RightEye })
                if (cam != null)
                    cam.targetTexture = null;
            QuestLinkEyes.Release(EyeTexture);
            EyeTexture = null;
        }
    }
}
