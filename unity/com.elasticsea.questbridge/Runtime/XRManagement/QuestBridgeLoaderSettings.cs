// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using UnityEngine;
using UnityEngine.XR.Management;

namespace ElasticSea.QuestBridge.XRManagement
{
    // Project Settings > XR Plug-in Management > Quest Link. Optional: defaults apply without it.
    [XRConfigurationData("QuestBridge", SettingsKey)]
    public class QuestBridgeLoaderSettings : ScriptableObject
    {
        public const string SettingsKey = "com.elasticsea.questbridge.loader_settings";

        public enum TrackingOrigin
        {
            // Link poses are relative to the headset's own origin: the XR Origin's Camera Y Offset is applied
            Device,
            // Link poses are floor-relative: the Camera Offset stays at 0
            Floor,
        }

        [Tooltip("Auto: libquestbridge if it loads, else the mock backend.")]
        public QuestBridgeRig.BackendMode backend = QuestBridgeRig.BackendMode.Auto;
        [Tooltip("How the XR Origin's Camera Offset is set (no XRInputSubsystem reports one).")]
        public TrackingOrigin trackingOrigin = TrackingOrigin.Device;
        [Tooltip("Run QuestBridgeHands and, with Unity XR Hands installed, an XRHandSubsystem.")]
        public bool handTracking = true;

        static QuestBridgeLoaderSettings runtimeInstance;

        // Players: the instance in the preloaded assets (added at build time).
        void OnEnable()
        {
            if (!Application.isEditor)
                runtimeInstance = this;
        }

        public static QuestBridgeLoaderSettings GetSettings()
        {
            QuestBridgeLoaderSettings settings = null;
#if UNITY_EDITOR
            UnityEditor.EditorBuildSettings.TryGetConfigObject(SettingsKey, out settings);
#else
            settings = runtimeInstance;
#endif
            return settings != null ? settings : CreateInstance<QuestBridgeLoaderSettings>();
        }
    }
}
