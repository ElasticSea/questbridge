// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using ElasticSea.QuestBridge.Editor;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Rendering;

namespace ElasticSea.QuestBridge.Tests
{
    // QuestBridge > Check Setup: the Unity-side checks and the qb_check[] parsing. The native
    // check wording is tested in native/tests/test_setup.cpp.
    public class CheckSetupTests
    {
        static byte[] NativeChecks(params (int status, string id, string title, string detail, string fix)[] checks)
        {
            var bytes = new byte[QuestBridgeCheckSetup.CheckSize * checks.Length];
            for (int i = 0; i < checks.Length; i++)
            {
                int b = i * QuestBridgeCheckSetup.CheckSize;
                BitConverter.GetBytes(checks[i].status).CopyTo(bytes, b);
                Encoding.ASCII.GetBytes(checks[i].id).CopyTo(bytes, b + 4);
                Encoding.ASCII.GetBytes(checks[i].title).CopyTo(bytes, b + 36);
                Encoding.ASCII.GetBytes(checks[i].detail).CopyTo(bytes, b + 100);
                Encoding.ASCII.GetBytes(checks[i].fix).CopyTo(bytes, b + 356);
            }
            return bytes;
        }

        [Test]
        public void ParsesNativeChecks()
        {
            var bytes = NativeChecks(
                (0, "quest_usb", "Quest on USB", "found (USB product ID 0x5013)", ""),
                (1, "ovr_service", "Meta OVRService", "running", new string('x', 511)));
            var checks = QuestBridgeCheckSetup.Parse(bytes, 2);
            Assert.AreEqual(2, checks.Count);
            Assert.AreEqual("quest_usb", checks[0].id);
            Assert.AreEqual(CheckStatus.Ok, checks[0].status);
            Assert.AreEqual("found (USB product ID 0x5013)", checks[0].detail);
            Assert.AreEqual("", checks[0].fix);
            Assert.AreEqual(CheckStatus.Warn, checks[1].status);
            Assert.AreEqual(511, checks[1].fix.Length);  // full field, no terminator overrun
        }

        [Test]
        public void WindowsEditorNeedsDirect3D11()
        {
            Assert.AreEqual(CheckStatus.Ok, QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.WindowsEditor, GraphicsDeviceType.Direct3D11).status);
            var c = QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.WindowsEditor, GraphicsDeviceType.Direct3D12);
            Assert.AreEqual(CheckStatus.Fail, c.status);
            StringAssert.Contains("Direct3D 11", c.detail);
            StringAssert.Contains("restart Unity", c.fix);
        }

        [Test]
        public void MacEditorNeedsMetal()
        {
            Assert.AreEqual(CheckStatus.Ok, QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.OSXEditor, GraphicsDeviceType.Metal).status);
            Assert.AreEqual(CheckStatus.Fail, QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.OSXEditor, GraphicsDeviceType.OpenGLCore).status);
            Assert.AreEqual(CheckStatus.Skip, QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.LinuxEditor, GraphicsDeviceType.Vulkan).status);
            // -nographics batch runs have no device: not a failure
            Assert.AreEqual(CheckStatus.Skip, QuestBridgeCheckSetup.GraphicsCheck(RuntimePlatform.WindowsEditor, GraphicsDeviceType.Null).status);
        }

        [Test]
        public void MissingNativeLibraryExplainsPerPlatform()
        {
            var mac = QuestBridgeCheckSetup.NativeLibraryMissing(RuntimePlatform.OSXEditor, "Intel(R) Core(TM) i9", new DllNotFoundException());
            Assert.AreEqual(CheckStatus.Fail, mac.status);
            StringAssert.Contains("Apple silicon", mac.fix);
            StringAssert.Contains("Intel", mac.detail);
            var win = QuestBridgeCheckSetup.NativeLibraryMissing(RuntimePlatform.WindowsEditor, "x", new DllNotFoundException());
            StringAssert.Contains("questbridge.dll", win.detail);
            var old = QuestBridgeCheckSetup.NativeLibraryMissing(RuntimePlatform.WindowsEditor, "x", new EntryPointNotFoundException());
            StringAssert.Contains("older", old.detail);
        }

        [Test]
        public void OverallAndFormat()
        {
            var checks = QuestBridgeCheckSetup.Parse(NativeChecks(
                (3, "ovr_service", "Meta OVRService", "Windows only", ""),
                (0, "quest_usb", "Quest on USB", "found", ""),
                (1, "gpu_encoder", "GPU video encoder", "software only", "Update the GPU driver.")), 3);
            Assert.AreEqual(CheckStatus.Warn, QuestBridgeCheckSetup.Overall(checks));
            var text = QuestBridgeCheckSetup.Format(checks);
            StringAssert.Contains("[SKIP] Meta OVRService: Windows only\n", text);
            StringAssert.Contains("[WARN] GPU video encoder: software only\n       Update the GPU driver.\n", text);
            Assert.AreEqual(CheckStatus.Ok, QuestBridgeCheckSetup.Overall(checks.Take(2)));
            checks.Add(new SetupCheck { id = "x", title = "X", detail = "d", status = CheckStatus.Fail });
            Assert.AreEqual(CheckStatus.Fail, QuestBridgeCheckSetup.Overall(checks));
        }

        // The real library, without opening the Quest (enumeration, SCM and encoder only)
        [Test]
        public void RunsAgainstTheNativeLibrary()
        {
            List<SetupCheck> checks = QuestBridgeCheckSetup.Run(claimUsb: false);
            var lib = checks.First(c => c.id == "native_library");
            if (lib.status != CheckStatus.Ok) Assert.Ignore("native library not installed: " + lib.detail);
            var ids = checks.Select(c => c.id).ToArray();
            CollectionAssert.AreEqual(new[] { "native_library", "graphics_api", "os", "link_app", "quest_usb", "xrsp_interface",
                "ovr_service", "xrsp_claim", "gpu_encoder" }, ids);
            // Not opened: skipped, or Ok if this Editor's runtime already holds the Quest
            Assert.That(checks.First(c => c.id == "xrsp_claim").status, Is.EqualTo(CheckStatus.Skip).Or.EqualTo(CheckStatus.Ok));
        }
    }
}
