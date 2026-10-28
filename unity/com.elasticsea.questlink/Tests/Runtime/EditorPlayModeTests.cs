using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace ElasticSea.QuestLink.Tests
{
    // What the rig does to the scene and app settings while playing, and undoes on stop.
    public class EditorPlayModeTests
    {
        GameObject main, rigObject;

        [TearDown]
        public void TearDown()
        {
            if (rigObject != null) Object.Destroy(rigObject);
            if (main != null) Object.Destroy(main);
        }

        QuestLinkRig Rig(bool ownListener)
        {
            rigObject = new GameObject("Rig");
            rigObject.SetActive(false);
            var rig = rigObject.AddComponent<QuestLinkRig>();
            rig.backend = QuestLinkRig.BackendMode.Mock;
            if (ownListener)
                new GameObject("Head", typeof(AudioListener)).transform.SetParent(rigObject.transform, false);
            rigObject.SetActive(true);
            return rig;
        }

        Camera MainCamera(Vector3 position)
        {
            main = new GameObject("Main Camera", typeof(Camera), typeof(AudioListener)) { tag = "MainCamera" };
            main.transform.position = position;
            var cam = main.GetComponent<Camera>();
            cam.backgroundColor = Color.magenta;
            cam.cullingMask = 1 << 3;
            return cam;
        }

        [UnityTest]
        public IEnumerator TakesOverTheMainCameraAndGivesItBack()
        {
            var cam = MainCamera(new Vector3(5f, 5f, 5f));
            var listener = main.GetComponent<AudioListener>();
            var rig = Rig(ownListener: true);

            Assert.IsFalse(cam.enabled);
            Assert.IsFalse(listener.enabled);
            Assert.AreSame(rig.LeftEye, Camera.main, "scripts using Camera.main get the left eye");
            // Settings copied, but CopyFrom's transform copy must not move the eyes off the head
            Assert.AreEqual(Color.magenta, rig.LeftEye.backgroundColor);
            Assert.AreEqual(1 << 3, rig.RightEye.cullingMask);
            Assert.AreEqual(QuestLinkMath.EyeOffset(QuestLinkEye.Left, rig.DeviceInfo.ipd), rig.LeftEye.transform.localPosition);
            Assert.AreSame(rig.EyeTexture, rig.LeftEye.targetTexture);

            Object.Destroy(rigObject);
            yield return null;
            Assert.IsTrue(cam.enabled);
            Assert.IsTrue(listener.enabled);
            Assert.AreSame(cam, Camera.main);
        }

        [UnityTest]
        public IEnumerator KeepsTheMainCameraListenerWhenTheRigHasNone()
        {
            MainCamera(Vector3.zero);
            Rig(ownListener: false);
            yield return null;
            Assert.IsTrue(main.GetComponent<AudioListener>().enabled);
            Assert.IsFalse(main.GetComponent<Camera>().enabled);
        }

        [UnityTest]
        public IEnumerator PacesToTheHeadsetWhilePlayingAndRestores()
        {
            var vsync = QualitySettings.vSyncCount;
            var rate = Application.targetFrameRate;
            var background = Application.runInBackground;
            try
            {
                QualitySettings.vSyncCount = 1;
                Application.targetFrameRate = -1;
                Application.runInBackground = false;
                var rig = Rig(ownListener: false);
                Assert.AreEqual(0, QualitySettings.vSyncCount);
                Assert.AreEqual(Mathf.RoundToInt(rig.DeviceInfo.refreshHz), Application.targetFrameRate);
                Assert.IsTrue(Application.runInBackground);

                Object.Destroy(rigObject);
                yield return null;
                Assert.AreEqual(1, QualitySettings.vSyncCount);
                Assert.AreEqual(-1, Application.targetFrameRate);
                Assert.IsFalse(Application.runInBackground);
            }
            finally
            {
                QualitySettings.vSyncCount = vsync;
                Application.targetFrameRate = rate;
                Application.runInBackground = background;
            }
        }

        [UnityTest]
        public IEnumerator MirrorCameraOnlyWhileRunning()
        {
            var rig = Rig(ownListener: false);
            var mirror = rigObject.transform.Find("QuestLink Mirror");
            Assert.IsNotNull(mirror);
            Assert.AreEqual(0, mirror.GetComponent<Camera>().cullingMask);
            Assert.IsNull(mirror.GetComponent<Camera>().targetTexture);

            rig.enabled = false;
            yield return null;
            Assert.IsNull(rigObject.transform.Find("QuestLink Mirror"));
            Assert.IsFalse(rig.LeftEye.enabled, "eyes without their texture would draw to the display");
            Assert.IsFalse(rig.RightEye.enabled);
        }

        // A real (non-batchmode) Editor, minimized so it has no focus and no visible Game view
        // (the tester in the headset): the rig must still render and submit at the headset's rate.
        [UnityTest]
        public IEnumerator SubmitsAtHeadsetRateWhileMinimized()
        {
            if (Application.isBatchMode)
                Assert.Ignore("batchmode doesn't render; run in a hidden non-batchmode Editor");
#if UNITY_EDITOR_WIN
            var rig = Rig(ownListener: false);
            var mock = (MockQuestLink)rig.Backend;
            var window = GetActiveWindow();
            ShowWindow(window, 6);  // SW_MINIMIZE
            try
            {
                for (var i = 0; i < 30; i++)
                    yield return null;  // warm up
                var start = mock.SubmittedFrames;
                var t0 = Time.realtimeSinceStartup;
                while (Time.realtimeSinceStartup - t0 < 4f)
                    yield return null;
                var rate = (mock.SubmittedFrames - start) / (Time.realtimeSinceStartup - t0);
                Debug.Log($"[QuestLink] minimized={IsIconic(window)} focused={Application.isFocused}, {rate:F1} submits/s");
                Assert.IsTrue(IsIconic(window), "Editor window didn't minimize");
                Assert.That(rate, Is.InRange(rig.DeviceInfo.refreshHz * 0.85f, rig.DeviceInfo.refreshHz * 1.1f));
            }
            finally
            {
                ShowWindow(window, 9);  // SW_RESTORE
            }
#else
            Assert.Ignore("Windows Editor only");
            yield break;
#endif
        }

#if UNITY_EDITOR_WIN
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern System.IntPtr GetActiveWindow();
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool ShowWindow(System.IntPtr window, int command);
        [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool IsIconic(System.IntPtr window);
#endif
    }
}
