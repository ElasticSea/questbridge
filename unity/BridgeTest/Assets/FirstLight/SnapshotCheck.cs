using System.Collections;
using System.IO;
using UnityEngine;

namespace ElasticSea.QuestLink.BridgeTest
{
    // "-snapshot <ppm>": after the eyes have rendered for a second, writes the eye texture (both eyes
    // side by side, what the encoder gets) to a binary PPM (no ImageConversion module here) and
    // quits with 1 when the picture is broken: magenta-dominant (missing or stripped shaders) or one flat colour (nothing rendered).
    // Use with "-mock" so it needs no headset (tools/check_firstlight.ps1).
    public class SnapshotCheck : MonoBehaviour
    {
        public QuestLinkRig rig;
        public string path;

        IEnumerator Start()
        {
            while (rig.EyeTexture == null)
                yield return null;
            for (var i = 0; i < 72; i++)
                yield return new WaitForEndOfFrame();
            var rt = rig.EyeTexture;
            var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGBA32, false);
            var previous = RenderTexture.active;
            RenderTexture.active = rt;
            tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0);
            tex.Apply();
            RenderTexture.active = previous;
            var pixels = tex.GetPixels32();
            WritePpm(path, pixels, rt.width, rt.height);

            // Every 8th pixel: magenta = strong red and blue, little green
            int total = 0, magenta = 0;
            var first = pixels[0];
            var flat = true;
            for (var i = 0; i < pixels.Length; i += 8)
            {
                var p = pixels[i];
                total++;
                if (p.r > 180 && p.b > 180 && p.g < 80)
                    magenta++;
                if (Mathf.Abs(p.r - first.r) + Mathf.Abs(p.g - first.g) + Mathf.Abs(p.b - first.b) > 24)
                    flat = false;
            }
            var share = magenta / (float)total;
            var ok = share < 0.05f && !flat;
            Debug.Log($"[Snapshot] {rt.width}x{rt.height} {rt.format} -> {path}: magenta {share:P1}, flat {flat}, {(ok ? "OK" : "BROKEN")}");
            Application.Quit(ok ? 0 : 1);
        }

        // Rows top-down (Unity's are bottom-up)
        static void WritePpm(string file, Color32[] pixels, int w, int h)
        {
            var header = System.Text.Encoding.ASCII.GetBytes($"P6\n{w} {h}\n255\n");
            var data = new byte[header.Length + w * h * 3];
            header.CopyTo(data, 0);
            var o = header.Length;
            for (var y = h - 1; y >= 0; y--)
                for (var x = 0; x < w; x++)
                {
                    var p = pixels[y * w + x];
                    data[o++] = p.r;
                    data[o++] = p.g;
                    data[o++] = p.b;
                }
            File.WriteAllBytes(file, data);
        }
    }
}
