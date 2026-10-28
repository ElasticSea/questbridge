// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Management;
#if QUESTBRIDGE_CORE_UTILS
using Unity.XR.CoreUtils;
#endif

namespace ElasticSea.QuestBridge.XRManagement
{
    // "QuestBridge" in XR Plug-in Management. Managed only: Unity's display and input subsystems
    // are native-only (XR SDK), so this runs a QuestBridgeXRDriver on the scene's main camera and
    // feeds the Input System devices XRI binds to (<XRHMD>, <XRController>), plus XR Hands.
    public class QuestBridgeLoader : XRLoaderHelper
    {
        // Tests: replaces the backend the settings would pick.
        public static Func<IQuestBridgeBackend> backendFactory;

        public QuestBridgeXRDriver Driver { get; private set; }

        IQuestBridgeBackend pending;
        QuestBridgeLoaderSettings settings;

        public override bool Initialize()
        {
            settings = QuestBridgeLoaderSettings.GetSettings();
            pending = backendFactory != null ? backendFactory() : QuestBridgeRig.CreateBackend(settings.backend);
            if (pending == null)
            {
                Debug.LogError("[QuestBridge] libquestbridge not available; set the QuestBridge backend to Auto or Mock.");
                return false;
            }
            Debug.Log($"[QuestBridge] XR loader initialized ({pending.Name} backend).");
            return true;
        }

        public override bool Start()
        {
            if (Driver != null)
                return true;
            var b = pending ?? (backendFactory != null ? backendFactory() : QuestBridgeRig.CreateBackend(settings.backend));
            pending = null;
            if (b == null)
                return false;
            QuestBridgeXRDriver.CameraAttached += OnCameraAttached;
            Driver = QuestBridgeXRDriver.Create(b);
            if (settings.handTracking)
            {
                Driver.gameObject.AddComponent<QuestBridgeHands>();
#if QUESTBRIDGE_XR_HANDS
                Driver.gameObject.AddComponent<ElasticSea.QuestBridge.XRHands.QuestBridgeXRHands>();
#endif
            }
            return true;
        }

        public override bool Stop()
        {
            QuestBridgeXRDriver.CameraAttached -= OnCameraAttached;
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

        // libquestbridge encodes D3D11 / Metal textures. At run time any API is accepted (empty list):
        // the Auto backend falls back to the mock there, which also keeps -nographics tests working.
        public override List<GraphicsDeviceType> GetSupportedGraphicsDeviceTypes(bool buildingPlayer) =>
            buildingPlayer ? new List<GraphicsDeviceType> { GraphicsDeviceType.Direct3D11, GraphicsDeviceType.Metal } : new List<GraphicsDeviceType>();

        void OnCameraAttached(Camera cam) => ApplyTrackingOrigin(cam, settings.trackingOrigin);

        // No XRInputSubsystem tells the XR Origin its tracking origin, so set its Camera Offset here.
        public static void ApplyTrackingOrigin(Camera cam, QuestBridgeLoaderSettings.TrackingOrigin origin)
        {
#if QUESTBRIDGE_CORE_UTILS
            var xrOrigin = cam != null ? cam.GetComponentInParent<XROrigin>() : null;
            if (xrOrigin == null || xrOrigin.CameraFloorOffsetObject == null)
                return;
            var offset = xrOrigin.CameraFloorOffsetObject.transform;
            var p = offset.localPosition;
            p.y = origin == QuestBridgeLoaderSettings.TrackingOrigin.Floor ? 0f : xrOrigin.CameraYOffset;
            offset.localPosition = p;
#endif
        }
    }
}
