using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace ElasticSea.QuestLink.Editor
{
    // Project checks for Editor Play mode, shown in the QuestLinkRig inspector, plus the
    // GameObject > XR > QuestLink Rig menu item.
    public static class QuestLinkSetup
    {
        public const string PackageRoot = "Packages/com.elasticsea.questlink";
        public const string RigPrefabPath = PackageRoot + "/Runtime/Prefabs/QuestLink Rig.prefab";
        public const string WindowsPluginPath = PackageRoot + "/Runtime/Plugins/Windows/x86_64/questlink.dll";
        public const string MacPluginPath = PackageRoot + "/Runtime/Plugins/macOS/libquestlink.dylib";

        public class Problem
        {
            public string message;
            public MessageType type;
            public string fixLabel;  // null = nothing to click
            public Action fix;
        }

        public static List<Problem> Check()
        {
            var problems = new List<Problem>();
            if (Application.platform == RuntimePlatform.WindowsEditor)
            {
                if (AssetImporter.GetAtPath(WindowsPluginPath) == null)
                    problems.Add(new Problem
                    {
                        message = "questlink.dll is missing from the package, so Play uses the mock headset. Install a package version that includes it.",
                        type = MessageType.Error,
                    });
                if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11)
                    problems.Add(new Problem
                    {
                        message = $"The Editor runs on {SystemInfo.graphicsDeviceType}; streaming needs Direct3D 11. Switching restarts the Editor.",
                        type = MessageType.Error,
                        fixLabel = "Use Direct3D 11",
                        fix = UseDirect3D11,
                    });
            }
#if !ENABLE_INPUT_SYSTEM
            problems.Add(new Problem
            {
                message = "Active Input Handling doesn't include the Input System package, so head and controller input won't reach your scripts.",
                type = MessageType.Warning,
                fixLabel = "Open Player Settings",
                fix = () => SettingsService.OpenProjectSettings("Project/Player"),
            });
#endif
            return problems;
        }

        static void UseDirect3D11()
        {
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { GraphicsDeviceType.Direct3D11 });
            if (EditorUtility.DisplayDialog("Quest Link", "Restart the Editor on Direct3D 11 now?", "Restart", "Later"))
                EditorApplication.OpenProject(System.IO.Directory.GetCurrentDirectory());
        }

        [MenuItem("GameObject/XR/QuestLink Rig", false, 10)]
        static void CreateRig(MenuCommand command)
        {
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(RigPrefabPath);
            if (prefab == null)
            {
                UnityEngine.Debug.LogError($"[QuestLink] {RigPrefabPath} not found.");
                return;
            }
            var rig = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
            GameObjectUtility.SetParentAndAlign(rig, command.context as GameObject);
            Undo.RegisterCreatedObjectUndo(rig, "Create QuestLink Rig");
            Selection.activeObject = rig;
        }
    }

    [CustomEditor(typeof(QuestLinkRig))]
    public class QuestLinkRigEditor : UnityEditor.Editor
    {
        List<QuestLinkSetup.Problem> problems;
        double checkedAt = -10;

        public override bool RequiresConstantRepaint() => Application.isPlaying;

        public override void OnInspectorGUI()
        {
            var rig = (QuestLinkRig)target;
            if (Application.isPlaying && rig.Backend != null)
            {
                var text = rig.State switch
                {
                    QuestLinkState.Streaming => $"Streaming ({rig.Backend.Name}), frame {rig.FrameIndex}",
                    QuestLinkState.Connecting => $"Quest found ({rig.Backend.Name}). In the headset: Quick Settings > Link > Launch.",
                    _ => rig.Backend.Name == "Native" ? "Waiting for the Quest on USB." : $"{rig.Backend.Name} headset (no streaming).",
                };
                EditorGUILayout.HelpBox(text, rig.State == QuestLinkState.Streaming ? MessageType.Info : MessageType.None);
            }

            // Asset lookups every repaint are wasteful; refresh the checks every few seconds
            if (problems == null || EditorApplication.timeSinceStartup - checkedAt > 3)
            {
                problems = QuestLinkSetup.Check();
                checkedAt = EditorApplication.timeSinceStartup;
            }
            foreach (var p in problems)
            {
                EditorGUILayout.HelpBox(p.message, p.type);
                if (p.fixLabel != null && GUILayout.Button(p.fixLabel))
                    p.fix();
            }

            DrawDefaultInspector();
        }
    }
}
