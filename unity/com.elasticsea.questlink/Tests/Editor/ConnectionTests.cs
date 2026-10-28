using System.Collections.Generic;
using System.Runtime.InteropServices;
using ElasticSea.QuestLink.Native;
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestLink.Tests
{
    public class ConnectionTests
    {
        readonly List<string> events = new List<string>();
        QuestLinkConnection connection;

        [SetUp]
        public void SetUp()
        {
            events.Clear();
            connection = new QuestLinkConnection();
            connection.Connected += s => events.Add($"connected {s.connects}");
            connection.Disconnected += r => events.Add($"disconnected {r}");
            connection.Reconnecting += s => events.Add($"reconnecting {s.state}");
        }

        static QuestLinkStatus S(QuestLinkLinkState state, int connects, int disconnects,
            QuestLinkLossReason loss = QuestLinkLossReason.None, int resumes = 0) =>
            new QuestLinkStatus { state = state, connects = connects, disconnects = disconnects, lastLoss = loss, resumes = resumes };

        [Test]
        public void NativeStatusLayoutMatchesTheHeader()
        {
            Assert.AreEqual(192, Marshal.SizeOf<QuestLinkNative.ql_link_status>());
            Assert.AreEqual(24, Marshal.OffsetOf<QuestLinkNative.ql_link_status>("state_since_ns").ToInt32());
            Assert.AreEqual(32, Marshal.OffsetOf<QuestLinkNative.ql_link_status>("encoder").ToInt32());
            Assert.AreEqual(4, (int)QuestLinkLinkState.Resuming);
            Assert.AreEqual(4, (int)QuestLinkLossReason.Silence);
        }

        [Test]
        public void FirstConnectThenLossAndNewHandshake()
        {
            connection.Update(S(QuestLinkLinkState.NoDevice, 0, 0));
            connection.Update(S(QuestLinkLinkState.Connecting, 0, 0));
            connection.Update(S(QuestLinkLinkState.WaitingForLaunch, 0, 0));
            connection.Update(S(QuestLinkLinkState.Streaming, 1, 0));
            // Link quit: USB gone, back, handshake, Launch
            connection.Update(S(QuestLinkLinkState.NoDevice, 1, 1, QuestLinkLossReason.Usb));
            connection.Update(S(QuestLinkLinkState.Connecting, 1, 1, QuestLinkLossReason.Usb));
            connection.Update(S(QuestLinkLinkState.WaitingForLaunch, 1, 1, QuestLinkLossReason.Usb));
            connection.Update(S(QuestLinkLinkState.WaitingForLaunch, 1, 1, QuestLinkLossReason.Usb));
            connection.Update(S(QuestLinkLinkState.Streaming, 2, 1, QuestLinkLossReason.Usb));
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
            connection.Update(S(QuestLinkLinkState.Streaming, 1, 0));
            connection.Update(S(QuestLinkLinkState.Streaming, 2, 1, QuestLinkLossReason.Usb, 1));
            CollectionAssert.AreEqual(new[] { "connected 1", "disconnected Usb", "connected 2" }, events);
        }

        [Test]
        public void ConnectedAndLostAgainBetweenSamples()
        {
            connection.Update(S(QuestLinkLinkState.Streaming, 1, 0));
            connection.Update(S(QuestLinkLinkState.Connecting, 1, 1, QuestLinkLossReason.HeadsetBye));
            connection.Update(S(QuestLinkLinkState.Connecting, 2, 2, QuestLinkLossReason.Silence));
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
            connection.Update(S(QuestLinkLinkState.Streaming, 3, 2, QuestLinkLossReason.NewHello));
            connection.Update(S(QuestLinkLinkState.Streaming, 3, 2, QuestLinkLossReason.NewHello));
            CollectionAssert.AreEqual(new[] { "connected 3" }, events);
            events.Clear();
            var other = new QuestLinkConnection();
            other.Reconnecting += s => events.Add($"reconnecting {s.state}");
            other.Update(S(QuestLinkLinkState.WaitingForLaunch, 3, 3, QuestLinkLossReason.NewHello));
            CollectionAssert.AreEqual(new[] { "reconnecting WaitingForLaunch" }, events);
        }

        [Test]
        public void StateOnlyBackendsGetEventsFromEdges()
        {
            connection.UpdateFromState(QuestLinkState.Idle);
            connection.UpdateFromState(QuestLinkState.Streaming);
            connection.UpdateFromState(QuestLinkState.Connecting);
            connection.UpdateFromState(QuestLinkState.Streaming);
            CollectionAssert.AreEqual(new[]
            {
                "connected 1", "disconnected None", "reconnecting Connecting", "connected 2",
            }, events);
        }

        [Test]
        public void DescribeTellsTheUserWhatToDo()
        {
            StringAssert.Contains("Launch", QuestLinkConnection.Describe(S(QuestLinkLinkState.WaitingForLaunch, 1, 1)));
            StringAssert.Contains("plug it in", QuestLinkConnection.Describe(S(QuestLinkLinkState.NoDevice, 0, 0)));
            StringAssert.Contains("relaunch Link", QuestLinkConnection.Describe(S(QuestLinkLinkState.NoDevice, 1, 1)));
            StringAssert.Contains("USB", QuestLinkConnection.Describe(QuestLinkLossReason.Usb));
        }

        [Test]
        public void StatusComponentFollowsTheMockThroughALoss()
        {
            var go = new GameObject("Rig");
            try
            {
                var rig = go.AddComponent<QuestLinkRig>();
                var mock = new MockQuestLink { timeOverride = 1f };
                rig.Initialize(mock);
                var status = go.AddComponent<QuestLinkConnectionStatus>();
                status.logEvents = false;
                status.Connected += s => events.Add($"connected {s.connects}");
                status.Disconnected += r => events.Add($"disconnected {r}");
                status.Reconnecting += s => events.Add($"reconnecting {s.state}");
                status.Tick();
                mock.SimulateLoss(QuestLinkLossReason.Silence, 2f);
                Assert.AreEqual(QuestLinkState.Connecting, mock.Poll());
                status.Tick();
                Assert.IsTrue(status.Status.Reconnecting);
                mock.timeOverride = 3.5f;
                status.Tick();
                Assert.AreEqual(QuestLinkState.Streaming, mock.Poll());
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
