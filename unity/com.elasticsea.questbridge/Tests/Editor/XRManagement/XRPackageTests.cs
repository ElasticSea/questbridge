// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System.Linq;
using ElasticSea.QuestBridge.XRManagement;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.XR.Management.Metadata;
using UnityEngine.Rendering;

namespace ElasticSea.QuestBridge.Tests
{
    public class XRPackageTests
    {
        [Test]
        public void QuestBridgeIsListedForStandalone()
        {
            var package = XRPackageMetadataStore.GetAllPackageMetadataForBuildTarget(BuildTargetGroup.Standalone)
                .FirstOrDefault(p => p.metadata.packageId == "com.elasticsea.questbridge");
            Assert.IsNotNull(package, "XR Plug-in Management doesn't list QuestBridge");
            var loader = package.metadata.loaderMetadata.Single();
            Assert.AreEqual("QuestBridge", loader.loaderName);
            Assert.AreEqual(typeof(QuestBridgeLoader).FullName, loader.loaderType);
            Assert.AreEqual(typeof(QuestBridgeLoaderSettings).FullName, package.metadata.settingsType);
        }

        [Test]
        public void LoaderSupportsD3D11AndMetal()
        {
            var loader = UnityEngine.ScriptableObject.CreateInstance<QuestBridgeLoader>();
            try
            {
                CollectionAssert.AreEquivalent(new[] { GraphicsDeviceType.Direct3D11, GraphicsDeviceType.Metal },
                    loader.GetSupportedGraphicsDeviceTypes(true));
                CollectionAssert.IsEmpty(loader.GetSupportedGraphicsDeviceTypes(false), "any API at run time");
            }
            finally { UnityEngine.Object.DestroyImmediate(loader); }
        }

        [Test]
        public void SettingsDefaultWithoutAnAsset()
        {
            var s = QuestBridgeLoaderSettings.GetSettings();
            Assert.AreEqual(QuestBridgeRig.BackendMode.Auto, s.backend);
            Assert.AreEqual(QuestBridgeLoaderSettings.TrackingOrigin.Device, s.trackingOrigin);
        }
    }
}
