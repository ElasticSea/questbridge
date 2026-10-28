using UnityEngine;
using UnityEngine.XR.Management;

namespace ElasticSea.QuestLink.XRManagement
{
    // Project Settings > XR Plug-in Management > Quest Link. Optional: defaults apply without it.
    [XRConfigurationData("Quest Link", SettingsKey)]
    public class QuestLinkLoaderSettings : ScriptableObject
    {
        public const string SettingsKey = "com.elasticsea.questlink.loader_settings";

        public enum TrackingOrigin
        {
            // Link poses are relative to the headset's own origin: the XR Origin's Camera Y Offset is applied
            Device,
            // Link poses are floor-relative: the Camera Offset stays at 0
            Floor,
        }

        [Tooltip("Auto: libquestlink if it loads, else the mock backend.")]
        public QuestLinkRig.BackendMode backend = QuestLinkRig.BackendMode.Auto;
        [Tooltip("How the XR Origin's Camera Offset is set (no XRInputSubsystem reports one).")]
        public TrackingOrigin trackingOrigin = TrackingOrigin.Device;
        [Tooltip("Run QuestLinkHands and, with Unity XR Hands installed, an XRHandSubsystem.")]
        public bool handTracking = true;

        static QuestLinkLoaderSettings runtimeInstance;

        // Players: the instance in the preloaded assets (added at build time).
        void OnEnable()
        {
            if (!Application.isEditor)
                runtimeInstance = this;
        }

        public static QuestLinkLoaderSettings GetSettings()
        {
            QuestLinkLoaderSettings settings = null;
#if UNITY_EDITOR
            UnityEditor.EditorBuildSettings.TryGetConfigObject(SettingsKey, out settings);
#else
            settings = runtimeInstance;
#endif
            return settings != null ? settings : CreateInstance<QuestLinkLoaderSettings>();
        }
    }
}
