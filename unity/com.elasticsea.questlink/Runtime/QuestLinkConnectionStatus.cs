using System;
using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Put next to a QuestLinkRig. Raises Connected / Disconnected / Reconnecting as the headset
    // link drops and recovers (Play mode keeps running; libquestlink reconnects by itself) and
    // shows the link state in the corner of the Game view until the stream is back.
    [DisallowMultipleComponent]
    public class QuestLinkConnectionStatus : MonoBehaviour
    {
        [Tooltip("The rig to watch; defaults to one on this object, else the first in the scene.")]
        public QuestLinkRig rig;
        [Tooltip("Draw the link state in the Game view while not streaming.")]
        public bool showOverlay = true;
        [Tooltip("Seconds \"Reconnected\" stays on screen after the stream is back.")]
        public float reconnectedSeconds = 3f;
        public bool logEvents = true;

        public event Action<QuestLinkStatus> Connected;
        public event Action<QuestLinkLossReason> Disconnected;
        public event Action<QuestLinkStatus> Reconnecting;

        public QuestLinkStatus Status => connection.Status;

        readonly QuestLinkConnection connection = new QuestLinkConnection();
        float reconnectedAt = -100f;
        GUIStyle style;
        bool wired;

        // Not in Awake: components added in Edit mode (tests) get no Awake
        void Wire()
        {
            if (wired)
                return;
            wired = true;
            connection.Connected += s =>
            {
                if (s.disconnects > 0)
                    reconnectedAt = Time.realtimeSinceStartup;
                if (logEvents)
                    Debug.Log(s.disconnects > 0 ? "[QuestLink] Reconnected" : "[QuestLink] Connected", this);
                Connected?.Invoke(s);
            };
            connection.Disconnected += r =>
            {
                if (logEvents)
                    Debug.LogWarning($"[QuestLink] Disconnected: {QuestLinkConnection.Describe(r)}; reconnecting", this);
                Disconnected?.Invoke(r);
            };
            connection.Reconnecting += s =>
            {
                if (logEvents)
                    Debug.Log($"[QuestLink] {QuestLinkConnection.Describe(s)}", this);
                Reconnecting?.Invoke(s);
            };
        }

        void Update() => Tick();

        // Also called by tests (no Update in Edit mode).
        public void Tick()
        {
            Wire();
            if (rig == null)
                rig = GetComponent<QuestLinkRig>();
            if (rig == null)
                rig = FindAnyObjectByType<QuestLinkRig>();
            var backend = rig != null ? rig.Backend : null;
            if (backend == null)
                return;
            if (backend is IQuestLinkStatusSource source && source.TryGetStatus(out var s))
                connection.Update(s);
            else
                connection.UpdateFromState(rig.State);
        }

        void OnGUI()
        {
            if (!showOverlay || !connection.HasStatus || Event.current.type != EventType.Repaint)
                return;
            var s = connection.Status;
            string text;
            if (s.state != QuestLinkLinkState.Streaming)
                text = s.disconnects > 0 ? $"Quest Link: {QuestLinkConnection.Describe(s.lastLoss)}\n{QuestLinkConnection.Describe(s)}" : $"Quest Link: {QuestLinkConnection.Describe(s)}";
            else if (Time.realtimeSinceStartup - reconnectedAt < reconnectedSeconds)
                text = "Quest Link: reconnected";
            else
                return;
            if (style == null)
                style = new GUIStyle(GUI.skin.box) { alignment = TextAnchor.MiddleLeft, fontSize = 16, wordWrap = true, padding = new RectOffset(12, 12, 8, 8) };
            var size = style.CalcSize(new GUIContent(text));
            var width = Mathf.Min(size.x, Screen.width - 20f);
            GUI.Box(new Rect(10f, 10f, width, style.CalcHeight(new GUIContent(text), width)), text, style);
        }
    }
}
