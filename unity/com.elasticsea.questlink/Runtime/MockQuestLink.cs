using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Hardware-free backend: Quest 3 device info, a slowly looking-around head and two
    // controllers held in front of it. Frames are counted, not sent.
    public sealed class MockQuestLink : IQuestLinkBackend
    {
        public const float StandingHeight = 1.6f;

        // Seconds; Time.realtimeSinceStartup unless a test drives it.
        public float? timeOverride;

        public ulong SubmittedFrames { get; private set; }
        public ulong LastFrameIndex { get; private set; }
        public QuestLinkPose LastRenderPose { get; private set; }
        public RenderTexture LastTexture { get; private set; }
        public (QuestLinkHand hand, float amplitude, float duration)? LastHaptic { get; private set; }

        public string Name => "Mock";

        float T => timeOverride ?? Time.realtimeSinceStartup;

        public QuestLinkState Poll() => QuestLinkState.Streaming;

        public bool TryGetDeviceInfo(out QuestLinkDeviceInfo info)
        {
            info = QuestLinkDeviceInfo.Quest3Default;
            return true;
        }

        public bool TryGetHeadPose(out QuestLinkPose pose)
        {
            pose = HeadAt(T);
            return true;
        }

        public static QuestLinkPose HeadAt(float t) => new QuestLinkPose
        {
            position = new Vector3(0.05f * Mathf.Sin(t * 0.7f), StandingHeight + 0.02f * Mathf.Sin(t * 1.3f), 0f),
            rotation = Quaternion.Euler(8f * Mathf.Sin(t * 0.5f), 35f * Mathf.Sin(t * 0.3f), 0f),
            timeNs = (long)(t * 1e9),
        };

        public bool TryGetController(QuestLinkHand hand, out QuestLinkControllerState state)
        {
            var t = T;
            var side = hand == QuestLinkHand.Left ? -1f : 1f;
            var phase = hand == QuestLinkHand.Left ? 0f : Mathf.PI;
            var pulse = Mathf.Sin(t * 2f + phase) * 0.5f + 0.5f;
            state = new QuestLinkControllerState
            {
                connected = true,
                positionTracked = true,
                rotationTracked = true,
                pose = new QuestLinkPose
                {
                    position = new Vector3(side * 0.2f, StandingHeight - 0.35f + 0.1f * Mathf.Sin(t + phase), 0.35f),
                    rotation = Quaternion.Euler(-20f, side * 10f, side * 15f * Mathf.Sin(t * 0.8f)),
                    timeNs = (long)(t * 1e9),
                },
                trigger = pulse,
                grip = 1f - pulse,
                thumbstick = new Vector2(Mathf.Cos(t), Mathf.Sin(t)) * 0.5f,
                primaryButton = pulse > 0.8f,
                triggerTouch = pulse > 0.1f,
            };
            return true;
        }

        public void SendHaptic(QuestLinkHand hand, float amplitude, float durationSeconds) =>
            LastHaptic = (hand, amplitude, durationSeconds);

        public void SubmitFrame(RenderTexture eyes, QuestLinkPose renderPose, ulong frameIndex)
        {
            SubmittedFrames++;
            LastFrameIndex = frameIndex;
            LastRenderPose = renderPose;
            LastTexture = eyes;
        }

        public void Dispose() { }
    }
}
