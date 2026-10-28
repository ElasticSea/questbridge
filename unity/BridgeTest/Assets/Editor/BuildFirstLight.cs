using System.Linq;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest.Editor
{
    // Windows player of the FirstLight scene for hands-off bench runs (small window; run it with
    // unity\run-firstlight.ps1, which keeps it on tools\unity_hidden.ps1's hidden desktop). Build:
    // tools\unity_hidden.ps1 -ArgLine "-batchmode -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildFirstLight.Build -logFile build.log"
    // Run unity\install-native.ps1 first so questlink.dll is in the package.
    // macOS (arm64 .app, Metal), after unity/install-native.sh:
    // Unity -batchmode -nographics -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildFirstLight.BuildMac -logFile build.log
    public static class BuildFirstLight
    {
        public const string Output = "Build/FirstLight/FirstLight.exe";
        public const string MacOutput = "Build/FirstLightMac/FirstLight.app";

        public static void Build()
        {
            SetPlayerSettings();
            Run(BuildTarget.StandaloneWindows64, Output, "questlink.dll");
        }

        public static void BuildMac()
        {
            SetPlayerSettings();
            // Apple silicon only: libquestlink.dylib is arm64 (same key as OSXStandalone.UserBuildSettings.architecture)
            EditorUserBuildSettings.SetPlatformSettings(BuildPipeline.GetBuildTargetName(BuildTarget.StandaloneOSX),
                "Architecture", "ARM64");
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneOSX, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneOSX, new[] { UnityEngine.Rendering.GraphicsDeviceType.Metal });
            Run(BuildTarget.StandaloneOSX, MacOutput, "libquestlink.dylib");
        }

        static void SetPlayerSettings()
        {
            PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
            PlayerSettings.defaultScreenWidth = 320;
            PlayerSettings.defaultScreenHeight = 180;
            PlayerSettings.resizableWindow = true;
            PlayerSettings.runInBackground = true;
            PlayerSettings.visibleInBackground = true;
        }

        static void Run(BuildTarget target, string output, string pluginFile)
        {
            var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
            {
                scenes = new[] { "Assets/FirstLight/FirstLight.unity" },
                locationPathName = output,
                target = target,
                options = BuildOptions.None,
            });
            var plugin = report.GetFiles().Any(f => f.path.EndsWith(pluginFile));
            Debug.Log($"[BuildFirstLight] {target}: {report.summary.result}, {report.summary.totalErrors} errors, {pluginFile} included: {plugin}");
            if (report.summary.result != BuildResult.Succeeded || !plugin)
                EditorApplication.Exit(1);
        }
    }
}
