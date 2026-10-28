using System.Runtime.InteropServices;
using ElasticSea.QuestBridge.Native;
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestBridge.Tests
{
    public class ProjectionTests
    {
        const float Eps = 1e-4f;

        static Vector3 Ndc(Matrix4x4 m, Vector3 view)
        {
            var c = m * new Vector4(view.x, view.y, view.z, 1f);
            return new Vector3(c.x / c.w, c.y / c.w, c.z / c.w);
        }

        [Test]
        public void Quest3DefaultsComeFromHelloAngles()
        {
            var info = QuestBridgeDeviceInfo.Quest3Default;
            Assert.AreEqual(Mathf.Tan(44f * Mathf.Deg2Rad), info.leftFov.up, Eps);
            Assert.AreEqual(Mathf.Tan(55f * Mathf.Deg2Rad), info.leftFov.down, Eps);
            Assert.AreEqual(Mathf.Tan(54f * Mathf.Deg2Rad), info.leftFov.left, Eps);   // temporal side
            Assert.AreEqual(Mathf.Tan(40f * Mathf.Deg2Rad), info.leftFov.right, Eps);  // nasal side
            // Right eye is the mirror image.
            Assert.AreEqual(info.leftFov.left, info.rightFov.right, Eps);
            Assert.AreEqual(info.leftFov.right, info.rightFov.left, Eps);
            Assert.AreEqual(info.leftFov.up, info.rightFov.up, Eps);
            Assert.AreEqual(info.leftFov.down, info.rightFov.down, Eps);
            Assert.AreEqual(0.0635f, info.ipd, Eps);
            Assert.AreEqual(4096, info.encodeWidth);
            Assert.AreEqual(2240, info.encodeHeight);
        }

        [Test]
        public void NativeDeviceInfoLayoutMatchesHeader()
        {
            // questbridge.h: qb_device_info 256 B, features at 104, firmware at 176, stream_codec at 240,
            // max_slice_bytes at 244, max_message_bytes at 248, default_bitrate at 252
            Assert.AreEqual(256, Marshal.SizeOf<QuestBridgeNative.qb_device_info>());
            Assert.AreEqual(240, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("stream_codec"));
            Assert.AreEqual(244, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("max_slice_bytes"));
            Assert.AreEqual(248, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("max_message_bytes"));
            Assert.AreEqual(252, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("default_bitrate"));
            Assert.AreEqual(104, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("features"));
            Assert.AreEqual(176, (int)Marshal.OffsetOf<QuestBridgeNative.qb_device_info>("firmware"));
        }

        [Test]
        public void FrustumEdgesMapToNdcEdges()
        {
            var fov = QuestBridgeDeviceInfo.Quest3Default.leftFov;
            var m = QuestBridgeMath.Projection(fov, 0.1f, 100f);
            const float d = 3f;  // Unity view space looks down -Z
            Assert.AreEqual(1f, Ndc(m, new Vector3(fov.right * d, 0, -d)).x, Eps);
            Assert.AreEqual(-1f, Ndc(m, new Vector3(-fov.left * d, 0, -d)).x, Eps);
            Assert.AreEqual(1f, Ndc(m, new Vector3(0, fov.up * d, -d)).y, Eps);
            Assert.AreEqual(-1f, Ndc(m, new Vector3(0, -fov.down * d, -d)).y, Eps);
        }

        [Test]
        public void StraightAheadIsOffCentreForAsymmetricFov()
        {
            var fov = QuestBridgeDeviceInfo.Quest3Default.leftFov;
            var ndc = Ndc(QuestBridgeMath.Projection(fov, 0.1f, 100f), new Vector3(0, 0, -5));
            Assert.AreEqual((fov.left - fov.right) / (fov.left + fov.right), ndc.x, Eps);
            Assert.AreEqual((fov.down - fov.up) / (fov.up + fov.down), ndc.y, Eps);
            Assert.Greater(ndc.x, 0f, "left eye sees more to its left, so straight ahead sits right of centre");
        }

        [Test]
        public void NearAndFarPlanesMapToClipRange()
        {
            var m = QuestBridgeMath.Projection(QuestBridgeDeviceInfo.Quest3Default.rightFov, 0.05f, 500f);
            Assert.AreEqual(-1f, Ndc(m, new Vector3(0, 0, -0.05f)).z, Eps);
            Assert.AreEqual(1f, Ndc(m, new Vector3(0, 0, -500f)).z, 1e-3f);
        }

        [Test]
        public void SymmetricFovMatchesPerspective()
        {
            var fov = EyeFov.FromDegrees(45f, 45f, 45f, 45f);
            var expected = Matrix4x4.Perspective(90f, 1f, 0.1f, 100f);
            var actual = QuestBridgeMath.Projection(fov, 0.1f, 100f);
            for (var i = 0; i < 16; i++)
                Assert.AreEqual(expected[i], actual[i], Eps, $"element {i}");
        }
    }
}
