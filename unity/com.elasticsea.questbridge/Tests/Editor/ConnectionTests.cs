using System.Collections.Generic;
using System.Runtime.InteropServices;
using ElasticSea.QuestBridge.Native;
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestBridge.Tests
{
    public class ConnectionTests
    {
        readonly List<string> events = new List<string>();
        QuestBridgeConnection connection;

        [SetUp]
        public void SetUp()
        {
            events.Clear();
            connection = new QuestBridgeConnection();
            connection.Connected += s => events.Add($"connected {s.connects}");
            connection.Disconnected += r => events.Add($"disconnected {r}");
            connection.Reconnecting += s => events.Add($"reconnecting {s.state}");
        }

        static QuestBridgeStatus S(QuestBridgeLinkState state, int connects, int disconnects,
            QuestBridgeLossReason loss = QuestBridgeLossReason.None, int resumes = 0) =>
            new QuestBridgeStatus { state = state, connects = connects, disconnects = disconnects, lastLoss = loss, resumes = resumes };

        [Test]
        public void NativeStatusLayoutMatchesTheHeader()
        {
            Assert.AreEqual(192, Marshal.SizeOf<QuestBridgeNative.qb_link_status>());
            Assert.AreEqual(24, Marshal.OffsetOf<QuestBridgeNative.qb_link_status>("state_since_ns").ToInt32());
            Assert.AreEqual(32, Marshal.OffsetOf<QuestBridgeNative.qb_link_status>("encoder").ToInt32());
            Assert.AreEqual(4, (int)QuestBridgeLinkState.Resuming);
            Assert.AreEqual(4, (int)QuestBridgeLossReason.Silence);
        }

        [Test]
        public void FirstConnectThenLossAndNewHandshake()
        {
            connection.Update(S(QuestBridgeLinkState.NoDevice, 0, 0));
            connection.Update(S(QuestBridgeLinkState.Connecting, 0, 0));
            connection.Update(S(QuestBridgeLinkState.WaitingForLaunch, 0, 0));
            connection.Update(S(QuestBridgeLinkState.Streaming, 1, 0));
            // Link quit: USB gone, back, handshake, Launch
            connection.Update(S(QuestBridgeLinkState.NoDevice, 1, 1, QuestBridgeLossReason.Usb));
            connection.Update(S(QuestBridgeLinkState.Connecting, 1, 1, QuestBridgeLossReason.Usb));
            connection.Update(S(QuestBridgeLinkState.WaitingForLaunch, 1, 1, QuestBridgeLossReason.Usb));
            connection.Update(S(QuestBridgeLinkState.WaitingForLaunch, 1, 1, QuestBridgeLossReason.Usb));
            connection.Update(S(QuestBridgeLinkState.Streaming, 2, 1, QuestBridgeLossReason.Usb));
            CollectionAssert.AreEqual(new[]
            {
                "connected 1",
                "disconnected Usb", "reconnecting NoDevice",
                "reconnecting Connecting",
                "reconnecting WaitingForLaunch",
                "connected 2",
            }, events);
        }

        [Test]
        public void ResumeBetweenTwoSamplesStillReportsBoth()
        {
            connection.Update(S(QuestBridgeLinkState.Streaming, 1, 0));
            connection.Update(S(QuestBridgeLinkState.Streaming, 2, 1, QuestBridgeLossReason.Usb, 1));
            CollectionAssert.AreEqual(new[] { "connected 1", "disconnected Usb", "connected 2" }, events);
        }

        [Test]
        public void ConnectedAndLostAgainBetweenSamples()
        {
            connection.Update(S(QuestBridgeLinkState.Streaming, 1, 0));
            connection.Update(S(QuestBridgeLinkState.Connecting, 1, 1, QuestBridgeLossReason.HeadsetBye));
            connection.Update(S(QuestBridgeLinkState.Connecting, 2, 2, QuestBridgeLossReason.Silence));
            CollectionAssert.AreEqual(new[]
            {
                "connected 1",
                "disconnected HeadsetBye", "reconnecting Connecting",
                "connected 2", "disconnected Silence", "reconnecting Connecting",
            }, events);
        }

        [Test]
        public void LaterPlaySessionReportsTheCurrentStateOnly()
        {
            // The native runtime outlives Play mode: no replay of its earlier losses
            connection.Update(S(QuestBridgeLinkState.Streaming, 3, 2, QuestBridgeLossReason.NewHello));
            connection.Update(S(QuestBridgeLinkState.Streaming, 3, 2, QuestBridgeLossReason.NewHello));
            CollectionAssert.AreEqual(new[] { "connected 3" }, events);
            events.Clear();
            var other = new QuestBridgeConnection();
            other.Reconnecting += s => events.Add($"reconnecting {s.state}");
            other.Update(S(QuestBridgeLinkState.WaitingForLaunch, 3, 3, QuestBridgeLossReason.NewHello));
            CollectionAssert.AreEqual(new[] { "reconnecting WaitingForLaunch" }, events);
        }

        [Test]
        public void StateOnlyBackendsGetEventsFromEdges()
        {
            connection.UpdateFromState(QuestBridgeState.Idle);
            connection.UpdateFromState(QuestBridgeState.Streaming);
            connection.UpdateFromState(QuestBridgeState.Connecting);
            connection.UpdateFromState(QuestBridgeState.Streaming);
            CollectionAssert.AreEqual(new[]
            {
                "connected 1", "disconnected None", "reconnecting Connecting", "connected 2",
            }, events);
        }

        [Test]
        public void DescribeTellsTheUserWhatToDo()
        {
            StringAssert.Contains("Launch", QuestBridgeConnection.Describe(S(QuestBridgeLinkState.WaitingForLaunch, 1, 1)));
            StringAssert.Contains("plug it in", QuestBridgeConnection.Describe(S(QuestBridgeLinkState.NoDevice, 0, 0)));
            StringAssert.Contains("relaunch Link", QuestBridgeConnection.Describe(S(QuestBridgeLinkState.NoDevice, 1, 1)));
            StringAssert.Contains("USB", QuestBridgeConnection.Describe(QuestBridgeLossReason.Usb));
        }

        [Test]
        public void StatusComponentFollowsTheMockThroughALoss()
        {
            var go = new GameObject("Rig");
            try
            {
                var rig = go.AddComponent<QuestBridgeRig>();
                var mock = new MockQuestBridge { timeOverride = 1f };
                rig.Initialize(mock);
                var status = go.AddComponent<QuestBridgeConnectionStatus>();
                status.logEvents = false;
                status.Connected += s => events.Add($"connected {s.connects}");
                status.Disconnected += r => events.Add($"disconnected {r}");
                status.Reconnecting += s => events.Add($"reconnecting {s.state}");
                status.Tick();
                mock.SimulateLoss(QuestBridgeLossReason.Silence, 2f);
                Assert.AreEqual(QuestBridgeState.Connecting, mock.Poll());
                status.Tick();
                Assert.IsTrue(status.Status.Reconnecting);
                mock.timeOverride = 3.5f;
                status.Tick();
                Assert.AreEqual(QuestBridgeState.Streaming, mock.Poll());
                CollectionAssert.AreEqual(new[]
                {
                    "connected 1", "disconnected Silence", "reconnecting WaitingForLaunch", "connected 2",
                }, events);
                rig.Shutdown();
            }
            finally
            {
                Object.DestroyImmediate(go);
            }
        }
    }
}
