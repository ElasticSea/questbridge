using System.Linq;
using ElasticSea.QuestLink.XRManagement;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.XR.Management.Metadata;
using UnityEngine.Rendering;

namespace ElasticSea.QuestLink.Tests
{
    public class XRPackageTests
    {
        [Test]
        public void QuestLinkIsListedForStandalone()
        {
            var package = XRPackageMetadataStore.GetAllPackageMetadataForBuildTarget(BuildTargetGroup.Standalone)
                .FirstOrDefault(p => p.metadata.packageId == "com.elasticsea.questlink");
            Assert.IsNotNull(package, "XR Plug-in Management doesn't list Quest Link");
            var loader = package.metadata.loaderMetadata.Single();
            Assert.AreEqual("Quest Link", loader.loaderName);
            Assert.AreEqual(typeof(QuestLinkLoader).FullName, loader.loaderType);
            Assert.AreEqual(typeof(QuestLinkLoaderSettings).FullName, package.metadata.settingsType);
        }

        [Test]
        public void LoaderSupportsD3D11AndMetal()
        {
            var loader = UnityEngine.ScriptableObject.CreateInstance<QuestLinkLoader>();
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
            var s = QuestLinkLoaderSettings.GetSettings();
            Assert.AreEqual(QuestLinkRig.BackendMode.Auto, s.backend);
            Assert.AreEqual(QuestLinkLoaderSettings.TrackingOrigin.Device, s.trackingOrigin);
        }
    }
}
