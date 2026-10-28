// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.XR.Management.Metadata;
using UnityEngine;

namespace ElasticSea.QuestBridge.XRManagement.Editor
{
    // Lists "QuestBridge" as a plug-in provider in Project Settings > XR Plug-in Management (Windows, Mac &
    // Linux tab). XR Plug-in Management creates the settings asset and its page from [XRConfigurationData].
    class QuestBridgeXRPackage : IXRPackage
    {
        class LoaderMetadata : IXRLoaderMetadata
        {
            public string loaderName { get; set; }
            public string loaderType { get; set; }
            public List<BuildTargetGroup> supportedBuildTargets { get; set; }
        }

        class PackageMetadata : IXRPackageMetadata
        {
            public string packageName { get; set; }
            public string packageId { get; set; }
            public string settingsType { get; set; }
            public List<IXRLoaderMetadata> loaderMetadata { get; set; }
        }

        public IXRPackageMetadata metadata => new PackageMetadata
        {
            packageName = "QuestBridge",
            packageId = "com.elasticsea.questbridge",
            settingsType = typeof(QuestBridgeLoaderSettings).FullName,
            loaderMetadata = new List<IXRLoaderMetadata>
            {
                new LoaderMetadata
                {
                    loaderName = "QuestBridge",
                    loaderType = typeof(QuestBridgeLoader).FullName,
                    supportedBuildTargets = new List<BuildTargetGroup> { BuildTargetGroup.Standalone },
                },
            },
        };

        public bool PopulateNewSettingsInstance(ScriptableObject obj) => obj is QuestBridgeLoaderSettings;
    }

    // Players read the settings from the preloaded assets.
    class QuestBridgeSettingsBuildStep : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        public void OnPreprocessBuild(BuildReport report)
        {
            Strip();
            if (!EditorBuildSettings.TryGetConfigObject(QuestBridgeLoaderSettings.SettingsKey, out QuestBridgeLoaderSettings settings) || settings == null)
                return;
            var assets = PlayerSettings.GetPreloadedAssets().ToList();
            assets.Add(settings);
            PlayerSettings.SetPreloadedAssets(assets.ToArray());
        }

        public void OnPostprocessBuild(BuildReport report) => Strip();

        static void Strip() =>
            PlayerSettings.SetPreloadedAssets(PlayerSettings.GetPreloadedAssets().Where(a => a != null && !(a is QuestBridgeLoaderSettings)).ToArray());
    }
}
