// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestBridge.Tests
{
    public class ConversionTests
    {
        const float Eps = 1e-4f;

        static void AreEqual(Vector3 a, Vector3 b)
        {
            Assert.AreEqual(a.x, b.x, Eps);
            Assert.AreEqual(a.y, b.y, Eps);
            Assert.AreEqual(a.z, b.z, Eps);
        }

        // Rotates v by q with plain quaternion algebra (handedness-free).
        static Vector3 Rotate(Quaternion q, Vector3 v)
        {
            var u = new Vector3(q.x, q.y, q.z);
            return 2f * Vector3.Dot(u, v) * u + (q.w * q.w - Vector3.Dot(u, u)) * v + 2f * q.w * Vector3.Cross(u, v);
        }

        [Test]
        public void LinkForwardIsUnityForward()
        {
            AreEqual(Vector3.forward, QuestBridgeMath.LinkToUnity(new Vector3(0, 0, -1)));
            AreEqual(Vector3.up, QuestBridgeMath.LinkToUnity(Vector3.up));
            AreEqual(Vector3.right, QuestBridgeMath.LinkToUnity(Vector3.right));
        }

        [Test]
        public void TurningLeftStaysTurningLeft()
        {
            // +90 deg about +Y in Link's right-handed frame turns -Z (forward) to -X (left).
            var s = Mathf.Sqrt(0.5f);
            var linkYaw = new Quaternion(0, s, 0, s);
            AreEqual(new Vector3(-1, 0, 0), Rotate(linkYaw, new Vector3(0, 0, -1)));
            AreEqual(Vector3.left, QuestBridgeMath.LinkToUnity(linkYaw) * Vector3.forward);
        }

        [Test]
        public void RotationsCommuteWithConversion()
        {
            var rng = new System.Random(7);
            for (var i = 0; i < 50; i++)
            {
                var q = new Quaternion((float)rng.NextDouble() - .5f, (float)rng.NextDouble() - .5f,
                    (float)rng.NextDouble() - .5f, (float)rng.NextDouble() - .5f).normalized;
                var v = new Vector3((float)rng.NextDouble(), (float)rng.NextDouble(), (float)rng.NextDouble());
                AreEqual(QuestBridgeMath.LinkToUnity(Rotate(q, v)),
                    QuestBridgeMath.LinkToUnity(q) * QuestBridgeMath.LinkToUnity(v));
                var back = QuestBridgeMath.UnityToLink(QuestBridgeMath.LinkToUnity(q));
                Assert.AreEqual(1f, Mathf.Abs(Quaternion.Dot(q, back)), Eps);
            }
        }
    }
}
