using System.Linq;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest.Editor
{
    // Windows player of the FirstLight scene for hands-off bench runs (small window; start it
    // with unity\run-firstlight.ps1 so it stays minimized and leaves the mouse alone):
    // Unity.exe -batchmode -quit -projectPath unity/BridgeTest -executeMethod ElasticSea.QuestLink.BridgeTest.Editor.BuildFirstLight.Build
    // Run unity\install-native.ps1 first so questlink.dll is in the package.
    public static class BuildFirstLight
    {
        public const string Output = "Build/FirstLight/FirstLight.exe";

        public static void Build()
        {
            PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
            PlayerSettings.defaultScreenWidth = 320;
            PlayerSettings.defaultScreenHeight = 180;
            PlayerSettings.resizableWindow = true;
            PlayerSettings.runInBackground = true;
            PlayerSettings.visibleInBackground = true;

            var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
            {
                scenes = new[] { "Assets/FirstLight/FirstLight.unity" },
                locationPathName = Output,
                target = BuildTarget.StandaloneWindows64,
                options = BuildOptions.None,
            });
            var plugin = report.GetFiles().Any(f => f.path.EndsWith("questlink.dll"));
            Debug.Log($"[BuildFirstLight] {report.summary.result}, {report.summary.totalErrors} errors, questlink.dll included: {plugin}");
            if (report.summary.result != BuildResult.Succeeded || !plugin)
                EditorApplication.Exit(1);
        }
    }
}
