// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace ElasticSea.QuestBridge.Editor
{
    public enum CheckStatus { Ok = 0, Warn = 1, Fail = 2, Skip = 3 }

    public struct SetupCheck
    {
        public string id, title, detail, fix;
        public CheckStatus status;
    }

    // First-run diagnostic: QuestBridge > Check Setup, or batch mode
    //   Unity -batchmode -nographics -quit -projectPath <p> -executeMethod ElasticSea.QuestBridge.Editor.QuestBridgeCheckSetup.RunFromCommandLine
    // (exit code 1 if anything failed). The Unity-side checks live here; everything about USB,
    // Meta Link, OVRService and the encoder comes from qb_check_setup (native/src/setup_check.cpp).
    public static class QuestBridgeCheckSetup
    {
        // qb_check layout (questbridge.h): int32 status, char id[32], title[64], detail[256], fix[512]
        public const int CheckSize = 868;
        const int IdOffset = 4, TitleOffset = 36, DetailOffset = 100, FixOffset = 356;
        const int MaxChecks = 16;    // QB_MAX_CHECKS
        const uint NoClaim = 1;      // QB_CHECK_NO_CLAIM

        [DllImport("questbridge")] static extern int qb_check_setup(uint flags, IntPtr checks, int cap);
        [DllImport("questbridge")] static extern int qb_stop_ovr_service();

        public static List<SetupCheck> Run(bool claimUsb = true)
        {
            var checks = new List<SetupCheck>();
            checks.Add(GraphicsCheck(Application.platform, SystemInfo.graphicsDeviceType));
            try
            {
                checks.AddRange(RunNative(claimUsb));
                checks.Insert(0, Check("native_library", "QuestBridge native library", CheckStatus.Ok, "loaded"));
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is BadImageFormatException)
            {
                checks.Insert(0, NativeLibraryMissing(Application.platform, SystemInfo.processorType, e));
            }
            return checks;
        }

        static List<SetupCheck> RunNative(bool claimUsb)
        {
            var buf = Marshal.AllocHGlobal(CheckSize * MaxChecks);
            try
            {
                int n = qb_check_setup(claimUsb ? 0 : NoClaim, buf, MaxChecks);
                if (n < 0)
                    return new List<SetupCheck> { Check("native_check", "Setup check", CheckStatus.Fail, $"failed ({n})", "Report this with the Editor log.") };
                var bytes = new byte[CheckSize * Math.Min(n, MaxChecks)];
                Marshal.Copy(buf, bytes, 0, bytes.Length);
                return Parse(bytes, Math.Min(n, MaxChecks));
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        // qb_check[] as raw bytes -> checks
        public static List<SetupCheck> Parse(byte[] bytes, int count)
        {
            var list = new List<SetupCheck>();
            for (int i = 0; i < count; i++)
            {
                int b = i * CheckSize;
                list.Add(new SetupCheck
                {
                    status = (CheckStatus)(BitConverter.ToInt32(bytes, b) & 3),
                    id = CString(bytes, b + IdOffset, 32),
                    title = CString(bytes, b + TitleOffset, 64),
                    detail = CString(bytes, b + DetailOffset, 256),
                    fix = CString(bytes, b + FixOffset, 512),
                });
            }
            return list;
        }

        static string CString(byte[] bytes, int start, int cap)
        {
            int len = 0;
            while (len < cap && bytes[start + len] != 0) len++;
            return Encoding.UTF8.GetString(bytes, start, len);
        }

        static SetupCheck Check(string id, string title, CheckStatus status, string detail, string fix = "") =>
            new SetupCheck { id = id, title = title, status = status, detail = detail, fix = fix };

        public static SetupCheck NativeLibraryMissing(RuntimePlatform platform, string cpu, Exception e)
        {
            const string title = "QuestBridge native library";
            if (e is EntryPointNotFoundException)
                return Check("native_library", title, CheckStatus.Fail, "older than this package",
                    "Close Unity, delete the project's Library/PackageCache/com.elasticsea.questbridge* folder and open the project again.");
            if (platform == RuntimePlatform.OSXEditor)
                return Check("native_library", title, CheckStatus.Fail, $"libquestbridge.dylib didn't load ({cpu})",
                    "It needs a Mac with Apple silicon (M1 or newer) and macOS 26 or later. On such a Mac, reinstall the package.");
            if (platform == RuntimePlatform.WindowsEditor)
                return Check("native_library", title, CheckStatus.Fail, "questbridge.dll didn't load",
                    "It needs 64-bit Windows 10/11. Reinstall the package; if it persists, install the latest Microsoft Visual C++ Redistributable (x64).");
            return Check("native_library", title, CheckStatus.Fail, $"not available on {platform}",
                "QuestBridge runs in the Unity Editor on Windows and on macOS with Apple silicon.");
        }

        public static SetupCheck GraphicsCheck(RuntimePlatform platform, GraphicsDeviceType api)
        {
            const string title = "Editor graphics API";
            if (api == GraphicsDeviceType.Null)
                return Check("graphics_api", title, CheckStatus.Skip, "no graphics device (batch mode -nographics)");
            if (platform == RuntimePlatform.WindowsEditor)
                return api == GraphicsDeviceType.Direct3D11
                    ? Check("graphics_api", title, CheckStatus.Ok, "Direct3D 11")
                    : Check("graphics_api", title, CheckStatus.Fail, $"{api}; streaming needs Direct3D 11",
                        "Use Direct3D 11 below (or Project Settings > Player > Windows > Other Settings: untick Auto Graphics API, move Direct3D11 to the top), then restart Unity.");
            if (platform == RuntimePlatform.OSXEditor)
                return api == GraphicsDeviceType.Metal
                    ? Check("graphics_api", title, CheckStatus.Ok, "Metal")
                    : Check("graphics_api", title, CheckStatus.Fail, $"{api}; streaming needs Metal",
                        "Project Settings > Player > macOS > Other Settings: put Metal first, then restart Unity.");
            return Check("graphics_api", title, CheckStatus.Skip, api.ToString());
        }

        // Worst status; Skip counts as Ok
        public static CheckStatus Overall(IEnumerable<SetupCheck> checks)
        {
            var worst = CheckStatus.Ok;
            foreach (var c in checks)
                if (c.status != CheckStatus.Skip && c.status > worst) worst = c.status;
            return worst;
        }

        public static string Format(IEnumerable<SetupCheck> checks)
        {
            var tags = new[] { "[ OK ]", "[WARN]", "[FAIL]", "[SKIP]" };
            var sb = new StringBuilder();
            foreach (var c in checks)
            {
                sb.Append(tags[(int)c.status]).Append(' ').Append(c.title).Append(": ").Append(c.detail).Append('\n');
                if (c.status != CheckStatus.Ok && !string.IsNullOrEmpty(c.fix)) sb.Append("       ").Append(c.fix).Append('\n');
            }
            return sb.ToString();
        }

        // True if a UAC prompt is up (OVRService was running)
        public static bool StopOvrService()
        {
            try { return qb_stop_ovr_service() == 1; }
            catch (Exception) { return false; }
        }

        public static void UseDirect3D11()
        {
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { GraphicsDeviceType.Direct3D11 });
            if (EditorUtility.DisplayDialog("QuestBridge", "Restart the Editor on Direct3D 11 now?", "Restart", "Later"))
                EditorApplication.OpenProject(System.IO.Directory.GetCurrentDirectory());
        }

        public static void RunFromCommandLine()
        {
            var args = Environment.GetCommandLineArgs();
            var checks = Run(Array.IndexOf(args, "-questbridgeNoClaim") < 0);
            var overall = Overall(checks);
            Debug.Log("[QuestBridge] Check Setup\n" + Format(checks) + (overall == CheckStatus.Fail ? "Not ready." : "Ready."));
            EditorApplication.Exit(overall == CheckStatus.Fail ? 1 : 0);
        }

        [MenuItem("QuestBridge/Check Setup", false, 0)]
        public static void Open() => EditorWindow.GetWindow<QuestBridgeCheckSetupWindow>(false, "QuestBridge Setup").Refresh();

        // One Console hint per machine after the package is installed
        [InitializeOnLoadMethod]
        static void Hint()
        {
            const string key = "ElasticSea.QuestBridge.SetupHintShown";
            if (Application.isBatchMode || EditorPrefs.GetBool(key, false)) return;
            EditorPrefs.SetBool(key, true);
            Debug.Log("[QuestBridge] Installed. Run QuestBridge > Check Setup to see whether this machine is ready to stream to a Quest.");
        }
    }

    public class QuestBridgeCheckSetupWindow : EditorWindow
    {
        List<SetupCheck> checks;
        Vector2 scroll;

        public void Refresh()
        {
            checks = QuestBridgeCheckSetup.Run();
            Repaint();
        }

        void OnGUI()
        {
            if (checks == null) Refresh();
            var overall = QuestBridgeCheckSetup.Overall(checks);
            EditorGUILayout.HelpBox(
                overall == CheckStatus.Fail ? "Not ready yet: fix the red items, then Check again."
                : overall == CheckStatus.Warn ? "Ready, with notes below."
                : "Ready. Add a QuestBridge Rig to the scene and press Play; in the headset press Launch on the Link dialog.",
                overall == CheckStatus.Fail ? MessageType.Error : overall == CheckStatus.Warn ? MessageType.Warning : MessageType.Info);

            scroll = EditorGUILayout.BeginScrollView(scroll);
            foreach (var c in checks)
            {
                var type = c.status == CheckStatus.Fail ? MessageType.Error
                    : c.status == CheckStatus.Warn ? MessageType.Warning
                    : c.status == CheckStatus.Ok ? MessageType.Info : MessageType.None;
                var text = $"{c.title}: {c.detail}";
                if (c.status != CheckStatus.Ok && !string.IsNullOrEmpty(c.fix)) text += "\n" + c.fix;
                EditorGUILayout.HelpBox(text, type);
                if (c.id == "graphics_api" && c.status == CheckStatus.Fail && Application.platform == RuntimePlatform.WindowsEditor
                    && GUILayout.Button("Use Direct3D 11"))
                    QuestBridgeCheckSetup.UseDirect3D11();
                if (c.id == "ovr_service" && c.detail.StartsWith("running") && GUILayout.Button("Stop OVRService now (admin prompt)"))
                {
                    QuestBridgeCheckSetup.StopOvrService();
                    EditorApplication.delayCall += () => { if (this) Refresh(); };
                }
            }
            EditorGUILayout.EndScrollView();

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("Check again")) Refresh();
                if (GUILayout.Button("Copy report")) EditorGUIUtility.systemCopyBuffer = QuestBridgeCheckSetup.Format(checks);
            }
        }
    }
}
