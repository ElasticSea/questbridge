// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;

namespace ElasticSea.QuestBridge
{
    // Backends that know the link's recovery state (libquestbridge API 4, the mock).
    public interface IQuestBridgeStatusSource
    {
        bool TryGetStatus(out QuestBridgeStatus status);
    }

    // Turns link status samples into Connected / Disconnected / Reconnecting events. Works on the
    // counters, not on edges between samples, so a stream lost and resumed between two frames
    // still raises Disconnected and then Connected. Main thread.
    public sealed class QuestBridgeConnection
    {
        public QuestBridgeStatus Status { get; private set; }
        public bool HasStatus { get; private set; }

        // The stream is up: first connect, reconnect after a new handshake, or a resume.
        public event Action<QuestBridgeStatus> Connected;
        // The stream was lost; the native runtime is already recovering.
        public event Action<QuestBridgeLossReason> Disconnected;
        // While recovering, each time the state changes (waiting for the headset, Launch, ...).
        public event Action<QuestBridgeStatus> Reconnecting;

        QuestBridgeLinkState lastFromState = QuestBridgeLinkState.NoDevice;
        int fromStateConnects, fromStateDisconnects;

        public void Update(QuestBridgeStatus s)
        {
            var prev = Status;
            var first = !HasStatus;
            Status = s;
            HasStatus = true;
            if (first)
            {
                // The native runtime outlives Play mode: report where it is, not its history
                if (s.state == QuestBridgeLinkState.Streaming)
                    Connected?.Invoke(s);
                else if (s.Reconnecting)
                    Reconnecting?.Invoke(s);
                return;
            }
            if (s.connects < prev.connects || s.disconnects < prev.disconnects)
                prev = default;  // another runtime (library reloaded): count from here
            var connected = s.connects > prev.connects;
            var lost = s.disconnects > prev.disconnects;
            var streaming = s.state == QuestBridgeLinkState.Streaming;
            // Streaming now: the loss came first. Not streaming: a connect, then the loss.
            if (connected && !streaming)
                Connected?.Invoke(s);
            if (lost)
                Disconnected?.Invoke(s.lastLoss);
            if (connected && streaming)
                Connected?.Invoke(s);
            if (s.Reconnecting && (lost || s.state != prev.state))
                Reconnecting?.Invoke(s);
        }

        // For backends without IQuestBridgeStatusSource: status from QuestBridgeRig.State edges.
        public void UpdateFromState(QuestBridgeState state)
        {
            var link = state == QuestBridgeState.Streaming ? QuestBridgeLinkState.Streaming
                : state == QuestBridgeState.Connecting ? QuestBridgeLinkState.Connecting
                : QuestBridgeLinkState.NoDevice;
            if (link == QuestBridgeLinkState.Streaming && lastFromState != QuestBridgeLinkState.Streaming)
                fromStateConnects++;
            if (link != QuestBridgeLinkState.Streaming && lastFromState == QuestBridgeLinkState.Streaming)
                fromStateDisconnects++;
            lastFromState = link;
            Update(new QuestBridgeStatus { state = link, connects = fromStateConnects, disconnects = fromStateDisconnects });
        }

        // One line for logs and the overlay.
        public static string Describe(QuestBridgeStatus s)
        {
            switch (s.state)
            {
                case QuestBridgeLinkState.Streaming:
                    return "Streaming";
                case QuestBridgeLinkState.Resuming:
                    return "USB dropped, resuming the stream...";
                case QuestBridgeLinkState.WaitingForLaunch:
                    return s.disconnects > 0 ? "Press Launch in the headset to reconnect" : "Press Launch in the headset (Quick Settings > Link)";
                case QuestBridgeLinkState.Connecting:
                    return s.disconnects > 0 ? "Reconnecting to the headset..." : "Connecting to the headset...";
                default:
                    return s.disconnects > 0
                        ? "Headset not on USB: waiting for it (relaunch Link in the headset)"
                        : "No Quest on USB: plug it in and close Meta Horizon Link on this computer";
            }
        }

        public static string Describe(QuestBridgeLossReason r)
        {
            switch (r)
            {
                case QuestBridgeLossReason.Usb: return "USB connection lost";
                case QuestBridgeLossReason.HeadsetBye: return "headset ended the stream";
                case QuestBridgeLossReason.NewHello: return "headset restarted Link";
                case QuestBridgeLossReason.Silence: return "headset stopped responding";
                default: return "stream ended";
            }
        }
    }
}
