// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using UnityEngine;

namespace ElasticSea.QuestBridge
{
    // Transport-agnostic source of poses and sink for frames. All poses are in Unity convention.
    public interface IQuestBridgeBackend : IDisposable
    {
        string Name { get; }

        // Main thread, once per frame.
        QuestBridgeState Poll();

        bool TryGetDeviceInfo(out QuestBridgeDeviceInfo info);
        bool TryGetHeadPose(out QuestBridgePose pose);
        bool TryGetController(QuestBridgeHand hand, out QuestBridgeControllerState state);
        // Fills state in place; false (state cleared) when no hand data arrived yet.
        bool TryGetHand(QuestBridgeHand hand, QuestBridgeHandState state);
        void SendHaptic(QuestBridgeHand hand, float amplitude, float durationSeconds);

        // Main thread, after the frame's cameras have rendered. Implementations hand the
        // texture to the render thread themselves.
        void SubmitFrame(RenderTexture eyes, QuestBridgePose renderPose, ulong frameIndex);

        // Any thread (Unity's audio thread). Game audio for the headset: the first `frames`
        // interleaved L R frames of `stereo` at `sampleRate`. Dropped while not streaming.
        void SubmitAudio(float[] stereo, int frames, int sampleRate);

        // Any thread. Headset microphone, 48 kHz mono, oldest first; returns samples copied.
        int ReadMicrophone(float[] samples, int offset, int count);
    }
}
