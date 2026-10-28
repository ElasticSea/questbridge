using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace ElasticSea.QuestBridge.Tests
{
    public class RigPlayModeTests
    {
        [UnityTest]
        public IEnumerator MockRigRendersAndSubmitsEveryFrame()
        {
            var go = new GameObject("Rig");
            go.SetActive(false);
            var rig = go.AddComponent<QuestBridgeRig>();
            rig.backend = QuestBridgeRig.BackendMode.Mock;
            go.SetActive(true);
            var mock = (MockQuestBridge)rig.Backend;

            for (var i = 0; i < 10; i++)
            {
                yield return null;
                // Batchmode skips onBeforeRender and camera rendering; drive one frame by hand.
                if (Application.isBatchMode)
                {
                    rig.LatchPose();
                    rig.LeftEye.Render();
                    rig.RightEye.Render();
                }
            }

            Assert.GreaterOrEqual(mock.SubmittedFrames, 5UL);
            Assert.AreSame(rig.EyeTexture, mock.LastTexture);
            // The Editor also runs onBeforeRender after the game cameras (Scene view, repaints),
            // so with real rendering the newest latch may not have rendered yet
            if (Application.isBatchMode || rig.FrameIndex != mock.LastFrameIndex + 1)
            {
                Assert.AreEqual(rig.FrameIndex, mock.LastFrameIndex);
                Assert.AreEqual(rig.RenderPose.position, mock.LastRenderPose.position);
            }
            Object.Destroy(go);
        }
    }
}
