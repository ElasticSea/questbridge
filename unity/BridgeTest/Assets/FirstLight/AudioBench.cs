using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // Audio check by ear, with its instructions in the headset. A 12 s loop:
    //   0-4 s  a soft beep every 2 s (game audio -> headset)
    //   4-8 s  "say something": records the headset mic (level bar shows it arriving)
    //   8-12 s plays the recording back through the game audio
    // The Console / player log gets one line per loop with mic and send counts.
    public class AudioBench : MonoBehaviour
    {
        const float Loop = 12f, RecordStart = 4f, PlayStart = 8f, BeepEvery = 2f;
        const int Rate = QuestLinkMicrophone.SampleRate;

        public QuestLinkAudio output;
        public QuestLinkMicrophone mic;
        public TextMesh label;

        AudioSource source;
        AudioClip beep;
        float[] recording = new float[Rate * 4];
        int recorded;
        int lastBeep = -1, lastLoop = -1;
        bool played;
        float peak;
        float start;

        void Start()
        {
            source = gameObject.AddComponent<AudioSource>();
            source.spatialBlend = 0f;
            beep = Beep();
            start = Time.realtimeSinceStartup;
        }

        void Update()
        {
            var t = Time.realtimeSinceStartup - start;
            var loop = (int)(t / Loop);
            var phase = t - loop * Loop;
            if (loop != lastLoop)
                NewLoop(loop);

            string text;
            if (phase < RecordStart)
            {
                var n = (int)(phase / BeepEvery);
                if (n != lastBeep)
                {
                    lastBeep = n;
                    source.PlayOneShot(beep);
                }
                text = "AUDIO: you should hear\na soft beep every 2 s";
                Drain();
            }
            else if (phase < PlayStart)
            {
                Record();
                text = $"Say something now ({Mathf.CeilToInt(PlayStart - phase)})";
            }
            else
            {
                if (!played)
                    PlayBack();
                text = "Playing back what you said";
                Drain();
            }
            if (label != null)
                label.text = text + "\nmic " + Bar(mic != null ? mic.Level : 0f);
        }

        void NewLoop(int loop)
        {
            if (lastLoop >= 0)
                Debug.Log($"[AudioBench] loop {lastLoop}: mic {(mic != null ? mic.ReceivedSamples : 0)} samples, " +
                          $"recorded {recorded} peak {peak:F3}, sent {(output != null ? output.SubmittedFrames : 0)} frames");
            lastLoop = loop;
            lastBeep = -1;
            recorded = 0;
            peak = 0f;
            played = false;
        }

        void Record()
        {
            if (mic == null)
                return;
            recorded += mic.Read(recording, recorded, recording.Length - recorded);
        }

        // Keep the mic buffer fresh outside the recording window
        void Drain()
        {
            if (mic != null)
                mic.Clear();
        }

        void PlayBack()
        {
            played = true;
            if (recorded == 0)
                return;
            for (var i = 0; i < recorded; i++)
                peak = Mathf.Max(peak, Mathf.Abs(recording[i]));
            // Normalise speech to a comfortable level, but don't blow up silence more than 20x
            var gain = Mathf.Min(0.5f / Mathf.Max(peak, 1e-6f), 20f);
            var samples = new float[recorded];
            for (var i = 0; i < recorded; i++)
                samples[i] = recording[i] * gain;
            var clip = AudioClip.Create("Mic recording", recorded, 1, Rate, false);
            clip.SetData(samples, 0);
            source.PlayOneShot(clip);
        }

        // 880 Hz, 150 ms, raised-cosine envelope, quiet
        static AudioClip Beep()
        {
            var n = Rate * 15 / 100;
            var s = new float[n];
            for (var i = 0; i < n; i++)
            {
                var env = 0.5f - 0.5f * Mathf.Cos(2f * Mathf.PI * i / n);
                s[i] = 0.25f * env * Mathf.Sin(2f * Mathf.PI * 880f * i / Rate);
            }
            var clip = AudioClip.Create("Beep", n, 1, Rate, false);
            clip.SetData(s, 0);
            return clip;
        }

        static string Bar(float level)
        {
            // ~-60 dBFS .. 0 dBFS over 20 cells
            var db = 20f * Mathf.Log10(Mathf.Max(level, 1e-6f));
            var cells = Mathf.Clamp(Mathf.RoundToInt((db + 60f) / 3f), 0, 20);
            return new string('|', cells).PadRight(20, '.');
        }
    }
}
