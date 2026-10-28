using System;
using UnityEngine;

namespace ElasticSea.QuestLink
{
    // The headset microphone (48 kHz mono). Each frame it pulls what arrived into a 1 s buffer
    // that scripts drain with Read(). With playThrough it also plays the mic live through an
    // AudioSource on this GameObject (so it reaches the headset too via QuestLinkAudio: keep the
    // gain low or expect feedback).
    [AddComponentMenu("QuestLink/QuestLink Microphone")]
    public class QuestLinkMicrophone : MonoBehaviour
    {
        public const int SampleRate = 48000;

        [Tooltip("Rig whose backend provides the mic; empty = the first QuestLinkRig in the scene.")]
        public QuestLinkRig rig;
        [Tooltip("Play the mic live through an AudioSource on this GameObject.")]
        public bool playThrough;
        [Range(0f, 4f)] public float playThroughGain = 1f;

        IQuestLinkBackend backendOverride;
        readonly SampleRing buffer = new SampleRing(SampleRate);
        readonly SampleRing playBuffer = new SampleRing(SampleRate / 2);
        readonly MicPlayer player = new MicPlayer();
        float[] pull = new float[SampleRate / 10];
        volatile int outputRate;
        AudioSource source;

        // RMS of the samples pulled in the last frame that had any (0..1).
        public float Level { get; private set; }
        public long ReceivedSamples { get; private set; }
        public int Available => buffer.Count;

        // Oldest first; returns samples copied.
        public int Read(float[] samples, int offset, int count) => buffer.Read(samples, offset, count);
        public void Clear() => buffer.Clear();

        IQuestLinkBackend Backend
        {
            get
            {
                if (backendOverride != null)
                    return backendOverride;
                if (rig == null)
                    rig = FindAnyObjectByType<QuestLinkRig>();
                return rig != null ? rig.Backend : null;
            }
        }

        // Tests and custom setups without a rig.
        internal void SetBackend(IQuestLinkBackend b) => backendOverride = b;

        void OnEnable()
        {
            outputRate = AudioSettings.outputSampleRate;
            if (playThrough)
                StartPlayThrough();
        }

        void OnDisable()
        {
            if (source != null)
                source.Stop();
        }

        void StartPlayThrough()
        {
            source = GetComponent<AudioSource>();
            if (source == null)
                source = gameObject.AddComponent<AudioSource>();
            // A silent looping clip keeps the source (and so OnAudioFilterRead) running
            source.clip = AudioClip.Create("QuestLink Mic (silence)", SampleRate, 1, SampleRate, false);
            source.loop = true;
            source.spatialBlend = 0f;
            source.Play();
        }

        void Update() => Pump();

        internal void Pump()
        {
            var b = Backend;
            if (b == null)
                return;
            double sum = 0;
            var total = 0;
            int n;
            while ((n = b.ReadMicrophone(pull, 0, pull.Length)) > 0)
            {
                buffer.Write(pull, 0, n);
                if (playThrough)
                    playBuffer.Write(pull, 0, n);
                for (var i = 0; i < n; i++)
                    sum += pull[i] * pull[i];
                total += n;
                if (n < pull.Length)
                    break;
            }
            if (total == 0)
                return;
            ReceivedSamples += total;
            Level = (float)Math.Sqrt(sum / total);
            if (playThrough && source == null)
                StartPlayThrough();
        }

        // Audio thread: replace the silent clip with the mic.
        internal void OnAudioFilterRead(float[] data, int channels)
        {
            if (!playThrough)
                return;
            player.Fill(playBuffer, data, channels, (double)SampleRate / Math.Max(1, outputRate), playThroughGain);
        }
    }

    // Thread-safe ring of mono samples; on overflow the oldest are dropped.
    internal sealed class SampleRing
    {
        readonly float[] ring;
        int head, count;

        public SampleRing(int capacity) => ring = new float[capacity];

        public int Count { get { lock (ring) return count; } }

        public void Write(float[] src, int offset, int n)
        {
            lock (ring)
                for (var i = 0; i < n; i++)
                {
                    if (count == ring.Length)
                    {
                        head = (head + 1) % ring.Length;
                        count--;
                    }
                    ring[(head + count) % ring.Length] = src[offset + i];
                    count++;
                }
        }

        public int Read(float[] dst, int offset, int n)
        {
            lock (ring)
            {
                n = Math.Min(n, count);
                for (var i = 0; i < n; i++)
                    dst[offset + i] = ring[(head + i) % ring.Length];
                head = (head + n) % ring.Length;
                count -= n;
                return n;
            }
        }

        public void Clear()
        {
            lock (ring)
                head = count = 0;
        }
    }

    // Plays a SampleRing at another output rate (linear interpolation). Holds back until 40 ms
    // are buffered and again after running dry, since the mic arrives in 20 ms bursts.
    internal sealed class MicPlayer
    {
        public const int PrimeSamples = QuestLinkMicrophone.SampleRate / 25;

        readonly float[] chunk = new float[1024];
        int chunkPos, chunkLen;
        double pos = 1;
        float prev, cur;
        bool primed;

        public bool Primed => primed;

        public void Fill(SampleRing src, float[] data, int channels, double step, float gain)
        {
            var frames = data.Length / channels;
            if (!primed && src.Count >= PrimeSamples)
                primed = true;
            for (var i = 0; i < frames; i++)
            {
                var v = 0f;
                if (primed)
                {
                    while (primed && pos >= 1)
                    {
                        prev = cur;
                        if (!Next(src, out cur))
                            primed = false;
                        pos -= 1;
                    }
                    if (primed)
                    {
                        v = (prev + (cur - prev) * (float)pos) * gain;
                        pos += step;
                    }
                    else
                        pos = 1;
                }
                for (var c = 0; c < channels; c++)
                    data[i * channels + c] = v;
            }
        }

        bool Next(SampleRing src, out float sample)
        {
            if (chunkPos == chunkLen)
            {
                chunkLen = src.Read(chunk, 0, chunk.Length);
                chunkPos = 0;
                if (chunkLen == 0)
                {
                    sample = 0f;
                    return false;
                }
            }
            sample = chunk[chunkPos++];
            return true;
        }
    }
}
