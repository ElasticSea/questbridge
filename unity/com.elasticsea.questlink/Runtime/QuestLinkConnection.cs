using System;

namespace ElasticSea.QuestLink
{
    // Backends that know the link's recovery state (libquestlink API 4, the mock).
    public interface IQuestLinkStatusSource
    {
        bool TryGetStatus(out QuestLinkStatus status);
    }

    // Turns link status samples into Connected / Disconnected / Reconnecting events. Works on the
    // counters, not on edges between samples, so a stream lost and resumed between two frames
    // still raises Disconnected and then Connected. Main thread.
    public sealed class QuestLinkConnection
    {
        public QuestLinkStatus Status { get; private set; }
        public bool HasStatus { get; private set; }

        // The stream is up: first connect, reconnect after a new handshake, or a resume.
        public event Action<QuestLinkStatus> Connected;
        // The stream was lost; the native runtime is already recovering.
        public event Action<QuestLinkLossReason> Disconnected;
        // While recovering, each time the state changes (waiting for the headset, Launch, ...).
        public event Action<QuestLinkStatus> Reconnecting;

        QuestLinkLinkState lastFromState = QuestLinkLinkState.NoDevice;
        int fromStateConnects, fromStateDisconnects;

        public void Update(QuestLinkStatus s)
        {
            var prev = Status;
            var first = !HasStatus;
            Status = s;
            HasStatus = true;
            if (first)
            {
                // The native runtime outlives Play mode: report where it is, not its history
                if (s.state == QuestLinkLinkState.Streaming)
                    Connected?.Invoke(s);
                else if (s.Reconnecting)
                    Reconnecting?.Invoke(s);
                return;
            }
            if (s.connects < prev.connects || s.disconnects < prev.disconnects)
                prev = default;  // another runtime (library reloaded): count from here
            var connected = s.connects > prev.connects;
            var lost = s.disconnects > prev.disconnects;
            var streaming = s.state == QuestLinkLinkState.Streaming;
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

        // For backends without IQuestLinkStatusSource: status from QuestLinkRig.State edges.
        public void UpdateFromState(QuestLinkState state)
        {
            var link = state == QuestLinkState.Streaming ? QuestLinkLinkState.Streaming
                : state == QuestLinkState.Connecting ? QuestLinkLinkState.Connecting
                : QuestLinkLinkState.NoDevice;
            if (link == QuestLinkLinkState.Streaming && lastFromState != QuestLinkLinkState.Streaming)
                fromStateConnects++;
            if (link != QuestLinkLinkState.Streaming && lastFromState == QuestLinkLinkState.Streaming)
                fromStateDisconnects++;
            lastFromState = link;
            Update(new QuestLinkStatus { state = link, connects = fromStateConnects, disconnects = fromStateDisconnects });
        }

        // One line for logs and the overlay.
        public static string Describe(QuestLinkStatus s)
        {
            switch (s.state)
            {
                case QuestLinkLinkState.Streaming:
                    return "Streaming";
                case QuestLinkLinkState.Resuming:
                    return "USB dropped, resuming the stream...";
                case QuestLinkLinkState.WaitingForLaunch:
                    return s.disconnects > 0 ? "Press Launch in the headset to reconnect" : "Press Launch in the headset (Quick Settings > Link)";
                case QuestLinkLinkState.Connecting:
                    return s.disconnects > 0 ? "Reconnecting to the headset..." : "Connecting to the headset...";
                default:
                    return s.disconnects > 0
                        ? "Headset not on USB: waiting for it (relaunch Link in the headset)"
                        : "No Quest on USB: plug it in and close Meta Horizon Link on this computer";
            }
        }

        public static string Describe(QuestLinkLossReason r)
        {
            switch (r)
            {
                case QuestLinkLossReason.Usb: return "USB connection lost";
                case QuestLinkLossReason.HeadsetBye: return "headset ended the stream";
                case QuestLinkLossReason.NewHello: return "headset restarted Link";
                case QuestLinkLossReason.Silence: return "headset stopped responding";
                default: return "stream ended";
            }
        }
    }
}
