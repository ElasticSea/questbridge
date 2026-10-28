using System;
using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Transport-agnostic source of poses and sink for frames. All poses are in Unity convention.
    public interface IQuestLinkBackend : IDisposable
    {
        string Name { get; }

        // Main thread, once per frame.
        QuestLinkState Poll();

        bool TryGetDeviceInfo(out QuestLinkDeviceInfo info);
        bool TryGetHeadPose(out QuestLinkPose pose);
        bool TryGetController(QuestLinkHand hand, out QuestLinkControllerState state);
        // Fills state in place; false (state cleared) when no hand data arrived yet.
        bool TryGetHand(QuestLinkHand hand, QuestLinkHandState state);
        void SendHaptic(QuestLinkHand hand, float amplitude, float durationSeconds);

        // Main thread, after the frame's cameras have rendered. Implementations hand the
        // texture to the render thread themselves.
        void SubmitFrame(RenderTexture eyes, QuestLinkPose renderPose, ulong frameIndex);

        // Any thread (Unity's audio thread). Game audio for the headset: the first `frames`
        // interleaved L R frames of `stereo` at `sampleRate`. Dropped while not streaming.
        void SubmitAudio(float[] stereo, int frames, int sampleRate);

        // Any thread. Headset microphone, 48 kHz mono, oldest first; returns samples copied.
        int ReadMicrophone(float[] samples, int offset, int count);
    }
}
