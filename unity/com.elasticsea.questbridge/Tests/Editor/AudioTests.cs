// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using NUnit.Framework;
using UnityEngine;

namespace ElasticSea.QuestBridge.Tests
{
    public class AudioTests
    {
        GameObject go;

        [SetUp]
        public void SetUp() => go = new GameObject("Audio");

        [TearDown]
        public void TearDown() => Object.DestroyImmediate(go);

        [Test]
        public void ToStereoDuplicatesMonoAndFoldsCentre()
        {
            var stereo = new float[0];
            Assert.AreEqual(2, QuestBridgeAudio.ToStereo(new[] { 0.1f, 0.2f }, 1, ref stereo));
            CollectionAssert.AreEqual(new[] { 0.1f, 0.1f, 0.2f, 0.2f }, stereo);

            Assert.AreEqual(1, QuestBridgeAudio.ToStereo(new[] { 0.1f, -0.2f }, 2, ref stereo));
            Assert.AreEqual(0.1f, stereo[0]);
            Assert.AreEqual(-0.2f, stereo[1]);

            // 5.1: L R C LFE Ls Rs
            Assert.AreEqual(1, QuestBridgeAudio.ToStereo(new[] { 0.1f, 0.2f, 0.5f, 1f, 1f, 1f }, 6, ref stereo));
            Assert.AreEqual(0.1f + 0.5f * 0.7071f, stereo[0], 1e-6f);
            Assert.AreEqual(0.2f + 0.5f * 0.7071f, stereo[1], 1e-6f);
        }

        [Test]
        public void ListenerMixGoesToBackendAndIsMutedLocally()
        {
            var audio = go.AddComponent<QuestBridgeAudio>();
            var listener = go.GetComponent<AudioListener>();
            Assert.IsNotNull(listener);
            // Otherwise Unity's DSP thread also calls OnAudioFilterRead and adds its own frames
            listener.enabled = false;
            var mock = new MockQuestBridge();
            audio.SetBackend(mock, 44100);

            var data = new float[1024 * 2];
            for (var i = 0; i < data.Length; i++)
                data[i] = i % 2 == 0 ? 0.25f : -0.25f;
            audio.OnAudioFilterRead(data, 2);

            Assert.AreEqual(1024, mock.SubmittedAudioFrames);
            Assert.AreEqual(1024, audio.SubmittedFrames);
            Assert.AreEqual(44100, mock.LastAudioSampleRate);
            Assert.AreEqual(0.25f, mock.LastAudio[0]);
            Assert.AreEqual(-0.25f, mock.LastAudio[2047]);
            Assert.AreEqual(0f, data[0], "PC output muted");

            audio.muteLocal = false;
            data[0] = 0.5f;
            audio.OnAudioFilterRead(data, 2);
            Assert.AreEqual(0.5f, data[0]);
            Assert.AreEqual(2048, mock.SubmittedAudioFrames);

            audio.SetBackend(null, 48000);  // no rig: nothing sent
            audio.OnAudioFilterRead(data, 2);
            Assert.AreEqual(2048, mock.SubmittedAudioFrames);
        }

        [Test]
        public void MicrophoneBuffersSamplesAndMeasuresLevel()
        {
            var mic = go.AddComponent<QuestBridgeMicrophone>();
            var mock = new MockQuestBridge();
            mic.SetBackend(mock);
            var samples = new float[6000];
            for (var i = 0; i < samples.Length; i++)
                samples[i] = i % 2 == 0 ? 0.5f : -0.5f;
            mock.QueueMicrophone(samples);

            mic.Pump();
            Assert.AreEqual(6000, mic.Available);
            Assert.AreEqual(6000, mic.ReceivedSamples);
            Assert.AreEqual(0.5f, mic.Level, 1e-5f);

            var read = new float[10000];
            Assert.AreEqual(6000, mic.Read(read, 0, read.Length));
            Assert.AreEqual(-0.5f, read[5999]);
            Assert.AreEqual(0, mic.Read(read, 0, read.Length));
        }

        [Test]
        public void SampleRingDropsOldest()
        {
            var ring = new SampleRing(4);
            ring.Write(new[] { 1f, 2f, 3f, 4f, 5f, 6f }, 0, 6);
            Assert.AreEqual(4, ring.Count);
            var out4 = new float[4];
            Assert.AreEqual(4, ring.Read(out4, 0, 4));
            CollectionAssert.AreEqual(new[] { 3f, 4f, 5f, 6f }, out4);
        }

        [Test]
        public void MicPlayerPrimesPlaysAndRecoversFromUnderflow()
        {
            var ring = new SampleRing(48000);
            var player = new MicPlayer();
            var out1 = new float[256 * 2];

            // Below the 40 ms cushion: silence, nothing consumed
            ring.Write(Ramp(1000), 0, 1000);
            player.Fill(ring, out1, 2, 1.0, 1f);
            Assert.IsFalse(player.Primed);
            Assert.AreEqual(1000, ring.Count);
            Assert.AreEqual(0f, out1[0]);

            // Primed at 48 kHz out: the ramp, one sample late, on both channels
            ring.Write(Ramp(2000), 1000, 1000);
            player.Fill(ring, out1, 2, 1.0, 1f);
            Assert.IsTrue(player.Primed);
            Assert.AreEqual(0f, out1[2 * 1]);
            Assert.AreEqual(out1[2 * 100], out1[2 * 100 + 1]);
            Assert.AreEqual(99f, out1[2 * 100]);

            // Drain everything: runs dry, outputs silence and waits for the cushion again
            var big = new float[4000 * 2];
            player.Fill(ring, big, 2, 1.0, 1f);
            Assert.IsFalse(player.Primed);
            Assert.AreEqual(0f, big[big.Length - 2]);

            // 24 kHz out (step 2) after re-priming: every other sample, with gain
            var p2 = new MicPlayer();
            var r2 = new SampleRing(48000);
            r2.Write(Ramp(4000), 0, 4000);
            var mono = new float[100];
            p2.Fill(r2, mono, 1, 2.0, 0.5f);
            Assert.AreEqual(0.5f * 2f, mono[2] - mono[1], 1e-6f);
        }

        static float[] Ramp(int n)
        {
            var r = new float[n];
            for (var i = 0; i < n; i++)
                r[i] = i;
            return r;
        }
    }
}
