using UnityEngine;

namespace ElasticSea.QuestBridge
{
    // While a QuestBridgeRig or the XR loader's driver streams in Play mode: keep running when the
    // Editor or player window loses focus (or is minimized) and render at the headset's refresh
    // rate with vSync off. Without it the Editor renders as fast as it can (Game view vSync is off
    // by default), burning GPU on frames the encoder drops, and an unfocused Editor stops playing.
    // Restore puts back what Apply changed.
    internal sealed class QuestBridgePacing
    {
        bool active, savedRunInBackground;
        int savedVSync, savedFrameRate;

        public void Apply(float refreshHz)
        {
            if (!Application.isPlaying)
                return;
            if (!active)
            {
                active = true;
                savedRunInBackground = Application.runInBackground;
                savedVSync = QualitySettings.vSyncCount;
                savedFrameRate = Application.targetFrameRate;
            }
            Application.runInBackground = true;
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = Mathf.Max(1, Mathf.RoundToInt(refreshHz));
        }

        public void Restore()
        {
            if (!active)
                return;
            active = false;
            Application.runInBackground = savedRunInBackground;
            QualitySettings.vSyncCount = savedVSync;
            Application.targetFrameRate = savedFrameRate;
        }
    }
}
