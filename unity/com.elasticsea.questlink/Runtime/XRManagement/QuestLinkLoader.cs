using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Management;
#if QUESTLINK_CORE_UTILS
using Unity.XR.CoreUtils;
#endif

namespace ElasticSea.QuestLink.XRManagement
{
    // "Quest Link" in XR Plug-in Management. Managed only: Unity's display and input subsystems
    // are native-only (XR SDK), so this runs a QuestLinkXRDriver on the scene's main camera and
    // feeds the Input System devices XRI binds to (<XRHMD>, <XRController>), plus XR Hands.
    public class QuestLinkLoader : XRLoaderHelper
    {
        // Tests: replaces the backend the settings would pick.
        public static Func<IQuestLinkBackend> backendFactory;

        public QuestLinkXRDriver Driver { get; private set; }

        IQuestLinkBackend pending;
        QuestLinkLoaderSettings settings;

        public override bool Initialize()
        {
            settings = QuestLinkLoaderSettings.GetSettings();
            pending = backendFactory != null ? backendFactory() : QuestLinkRig.CreateBackend(settings.backend);
            if (pending == null)
            {
                Debug.LogError("[QuestLink] libquestlink not available; set the Quest Link backend to Auto or Mock.");
                return false;
            }
            Debug.Log($"[QuestLink] XR loader initialized ({pending.Name} backend).");
            return true;
        }

        public override bool Start()
        {
            if (Driver != null)
                return true;
            var b = pending ?? (backendFactory != null ? backendFactory() : QuestLinkRig.CreateBackend(settings.backend));
            pending = null;
            if (b == null)
                return false;
            QuestLinkXRDriver.CameraAttached += OnCameraAttached;
            Driver = QuestLinkXRDriver.Create(b);
            if (settings.handTracking)
            {
                Driver.gameObject.AddComponent<QuestLinkHands>();
#if QUESTLINK_XR_HANDS
                Driver.gameObject.AddComponent<ElasticSea.QuestLink.XRHands.QuestLinkXRHands>();
#endif
            }
            return true;
        }

        public override bool Stop()
        {
            QuestLinkXRDriver.CameraAttached -= OnCameraAttached;
            if (Driver != null)
            {
                var go = Driver.gameObject;
                Driver.Shutdown();
                if (Application.isPlaying) Destroy(go);
                else DestroyImmediate(go);
            }
            Driver = null;
            return true;
        }

        public override bool Deinitialize()
        {
            Stop();
            pending?.Dispose();
            pending = null;
            return base.Deinitialize();
        }

        // libquestlink encodes D3D11 / Metal textures. At run time any API is accepted (empty list):
        // the Auto backend falls back to the mock there, which also keeps -nographics tests working.
        public override List<GraphicsDeviceType> GetSupportedGraphicsDeviceTypes(bool buildingPlayer) =>
            buildingPlayer ? new List<GraphicsDeviceType> { GraphicsDeviceType.Direct3D11, GraphicsDeviceType.Metal } : new List<GraphicsDeviceType>();

        void OnCameraAttached(Camera cam) => ApplyTrackingOrigin(cam, settings.trackingOrigin);

        // No XRInputSubsystem tells the XR Origin its tracking origin, so set its Camera Offset here.
        public static void ApplyTrackingOrigin(Camera cam, QuestLinkLoaderSettings.TrackingOrigin origin)
        {
#if QUESTLINK_CORE_UTILS
            var xrOrigin = cam != null ? cam.GetComponentInParent<XROrigin>() : null;
            if (xrOrigin == null || xrOrigin.CameraFloorOffsetObject == null)
                return;
            var offset = xrOrigin.CameraFloorOffsetObject.transform;
            var p = offset.localPosition;
            p.y = origin == QuestLinkLoaderSettings.TrackingOrigin.Floor ? 0f : xrOrigin.CameraYOffset;
            offset.localPosition = p;
#endif
        }
    }
}
