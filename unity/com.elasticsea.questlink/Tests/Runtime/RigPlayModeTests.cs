using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace ElasticSea.QuestLink.Tests
{
    public class RigPlayModeTests
    {
        [UnityTest]
        public IEnumerator MockRigRendersAndSubmitsEveryFrame()
        {
            var go = new GameObject("Rig");
            go.SetActive(false);
            var rig = go.AddComponent<QuestLinkRig>();
            rig.backend = QuestLinkRig.BackendMode.Mock;
            go.SetActive(true);
            var mock = (MockQuestLink)rig.Backend;

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
            Assert.AreEqual(rig.FrameIndex, mock.LastFrameIndex);
            Assert.AreSame(rig.EyeTexture, mock.LastTexture);
            Assert.AreEqual(rig.RenderPose.position, mock.LastRenderPose.position);
            Object.Destroy(go);
        }
    }
}
