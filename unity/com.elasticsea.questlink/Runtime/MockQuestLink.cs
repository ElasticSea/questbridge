using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Hardware-free backend: Quest 3 device info, a slowly looking-around head, two
    // controllers held in front of it and two hands curling their fingers (through the same
    // forward kinematics as libquestlink). Frames are counted, not sent.
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
        public long SubmittedAudioFrames { get; private set; }
        public int LastAudioSampleRate { get; private set; }
        public float[] LastAudio { get; private set; } = new float[0];  // interleaved stereo

        readonly System.Collections.Generic.Queue<float> microphone = new System.Collections.Generic.Queue<float>();

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

        public bool TryGetHand(QuestLinkHand hand, QuestLinkHandState state)
        {
            HandAt(hand, T, state);
            return true;
        }

        // Main thread only (shared buffers)
        static readonly Quaternion[] handLocal = new Quaternion[QuestLinkHandSkeleton.BoneCount];
        static readonly Pose[] handBones = new Pose[QuestLinkHandSkeleton.BoneCount];
        // Finger bones (proximal, intermediate, distal) and how far each bends at full curl, degrees
        static readonly int[][] fingerBones = { new[] { 6, 7, 8 }, new[] { 9, 10, 11 }, new[] { 12, 13, 14 }, new[] { 16, 17, 18 }, new[] { 3, 4, 5 } };
        static readonly float[] bend = { 70f, 95f, 60f };

        // Hands at chest height, palms turned inward, fingers curling in a wave; the curl of
        // each finger is also its pinch strength.
        public static void HandAt(QuestLinkHand hand, float t, QuestLinkHandState state)
        {
            var side = hand == QuestLinkHand.Left ? -1f : 1f;
            var phase = hand == QuestLinkHand.Left ? 0f : 1.3f;
            for (var b = 0; b < QuestLinkHandSkeleton.BoneCount; b++)
                handLocal[b] = b == 2 || b == 15 ? QuestLinkHandSkeleton.BindRotation(b) : Quaternion.identity;
            state.Clear();
            for (var f = 0; f < fingerBones.Length; f++)
            {
                var curl = 0.5f + 0.5f * Mathf.Sin(t * 1.5f + phase + f * 0.6f);
                var bones = fingerBones[f];
                for (var i = 0; i < bones.Length; i++)
                    handLocal[bones[i]] = QuestLinkHandSkeleton.BindRotation(bones[i]) *
                                          Quaternion.AngleAxis(curl * bend[i] * (f == 4 ? 0.5f : 1f), Vector3.forward);
                if (f < 4)
                    state.pinch[f] = curl;
            }

            // Wrist in Unity convention (+Z toward the fingers, +Y back of the hand), then Link
            var wristPos = new Vector3(side * 0.18f, StandingHeight - 0.35f + 0.05f * Mathf.Sin(t * 0.9f + phase), 0.35f);
            var wristRot = Quaternion.Euler(-15f, side * 10f, -side * 70f);
            var root = new Pose(QuestLinkMath.UnityToLink(wristPos),
                QuestLinkHandSkeleton.RootFromWrist(hand, QuestLinkMath.UnityToLink(wristRot)));
            const float scale = 1f;
            QuestLinkHandSkeleton.Bones(hand, root, handLocal, scale, handBones);
            QuestLinkHandSkeleton.Joints(hand, handBones, scale, state.joints);
            QuestLinkHandSkeleton.ToUnity(state.joints);

            state.tracked = true;
            state.confidence = 1f;
            state.scale = scale;
            state.dominant = hand == QuestLinkHand.Right;
            state.aimValid = true;
            var forward = wristRot * Vector3.forward;
            state.aim = new QuestLinkPose { position = wristPos + forward * 0.05f, rotation = Quaternion.LookRotation(forward), timeNs = (long)(t * 1e9) };
            state.timeNs = (long)(t * 1e9);
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

        public void SubmitAudio(float[] stereo, int frames, int sampleRate)
        {
            lock (microphone)
            {
                SubmittedAudioFrames += frames;
                LastAudioSampleRate = sampleRate;
                LastAudio = new float[frames * 2];
                System.Array.Copy(stereo, LastAudio, frames * 2);
            }
        }

        // Tests: samples the "headset" microphone will return.
        public void QueueMicrophone(float[] samples)
        {
            lock (microphone)
                foreach (var s in samples)
                    microphone.Enqueue(s);
        }

        public int ReadMicrophone(float[] samples, int offset, int count)
        {
            lock (microphone)
            {
                var n = Mathf.Min(count, microphone.Count);
                for (var i = 0; i < n; i++)
                    samples[offset + i] = microphone.Dequeue();
                return n;
            }
        }

        public void Dispose() { }
    }
}
