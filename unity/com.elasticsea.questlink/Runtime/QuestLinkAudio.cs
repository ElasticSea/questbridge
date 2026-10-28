using System;
using System.Threading;
using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Sends what the AudioListener hears to the headset. Put it next to the scene's AudioListener
    // (ideally on the rig's Head so 3D sound follows the headset). The native side queues the
    // audio and sends a 10 ms block every 10 ms; Unity's mix is downmixed to stereo here.
    [RequireComponent(typeof(AudioListener))]
    [AddComponentMenu("QuestLink/QuestLink Audio")]
    public class QuestLinkAudio : MonoBehaviour
    {
        [Tooltip("Rig whose backend gets the audio; empty = the first QuestLinkRig in the scene, else the XR loader's.")]
        public QuestLinkRig rig;
        [Tooltip("Silence the PC's own speakers; the headset still gets the audio.")]
        public bool muteLocal = true;

        volatile IQuestLinkBackend backend;
        volatile int sampleRate;
        float[] stereo = new float[0];
        long submittedFrames;
        volatile float peak;

        public long SubmittedFrames => Interlocked.Read(ref submittedFrames);

        // Largest |sample| sent since the last call (level meters, diagnostics)
        public float TakePeak()
        {
            var p = peak;
            peak = 0f;
            return p;
        }

        void OnEnable()
        {
            sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.OnAudioConfigurationChanged += OnAudioConfigurationChanged;
        }

        void OnDisable()
        {
            AudioSettings.OnAudioConfigurationChanged -= OnAudioConfigurationChanged;
            backend = null;
        }

        void OnAudioConfigurationChanged(bool deviceWasChanged) => sampleRate = AudioSettings.outputSampleRate;

        void Update()
        {
            if (rig == null)
                rig = FindAnyObjectByType<QuestLinkRig>();
            backend = rig != null ? rig.Backend : QuestLinkXRDriver.Active?.Backend;
        }

        // Tests and custom setups without a rig.
        internal void SetBackend(IQuestLinkBackend b, int rate)
        {
            backend = b;
            sampleRate = rate;
        }

        // Audio thread: the listener's final mix.
        internal void OnAudioFilterRead(float[] data, int channels)
        {
            var b = backend;
            if (b != null)
            {
                var frames = ToStereo(data, channels, ref stereo);
                b.SubmitAudio(stereo, frames, sampleRate);
                Interlocked.Add(ref submittedFrames, frames);
                var p = peak;
                for (var i = 0; i < frames * 2; i++)
                    p = Math.Max(p, Math.Abs(stereo[i]));
                peak = p;
            }
            if (muteLocal)
                Array.Clear(data, 0, data.Length);
        }

        // Interleaved mix with any channel count -> interleaved stereo. Mono is duplicated;
        // surround keeps front L R and folds the centre (channel 2 in Unity's 5.0/5.1/7.1) in at -3 dB.
        internal static int ToStereo(float[] data, int channels, ref float[] stereo)
        {
            if (channels <= 0)
                return 0;
            var frames = data.Length / channels;
            if (stereo.Length < frames * 2)
                stereo = new float[frames * 2];
            for (var i = 0; i < frames; i++)
            {
                var j = i * channels;
                float l, r;
                if (channels == 1)
                    l = r = data[j];
                else
                {
                    l = data[j];
                    r = data[j + 1];
                    if (channels >= 5)
                    {
                        var c = data[j + 2] * 0.7071f;
                        l += c;
                        r += c;
                    }
                }
                stereo[2 * i] = l;
                stereo[2 * i + 1] = r;
            }
            return frames;
        }
    }
}
