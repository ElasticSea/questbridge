using UnityEngine;

namespace ElasticSea.QuestLink
{
    // Stereo texture and eye cameras shared by QuestLinkRig and the XR loader's QuestLinkXRDriver.
    internal static class QuestLinkEyes
    {
        // One encode-sized texture, left eye in the left half, right eye in the right half. Returns
        // rt when it still fits, else a new texture (the caller releases the old one).
        public static RenderTexture EnsureTexture(RenderTexture rt, QuestLinkDeviceInfo info, int antiAliasing)
        {
            if (rt != null && rt.width == info.encodeWidth && rt.height == info.encodeHeight && rt.antiAliasing == antiAliasing)
                return rt;
            rt = new RenderTexture(info.encodeWidth, info.encodeHeight, 24, RenderTextureFormat.BGRA32, RenderTextureReadWrite.sRGB)
            {
                name = "QuestLink Eyes",
                antiAliasing = antiAliasing,
            };
            rt.Create();
            return rt;
        }

        public static void Configure(Camera cam, QuestLinkEye eye, QuestLinkDeviceInfo info, RenderTexture target,
            float nearClip, float farClip, Camera template)
        {
            var t = cam.transform;
            if (template != null && template != cam)
                cam.CopyFrom(template);
            t.localPosition = QuestLinkMath.EyeOffset(eye, info.ipd);
            t.localRotation = Quaternion.identity;
            cam.stereoTargetEye = StereoTargetEyeMask.None;
            // Render straight into the target. With HDR or camera MSAA the built-in pipeline renders
            // into an intermediate and copies it to the target only after onPostRender, so the
            // submit (after the right eye) would carry the previous frame's right half.
            cam.allowHDR = false;
            cam.allowMSAA = false;
            cam.allowDynamicResolution = false;
            cam.targetTexture = target;
            cam.rect = QuestLinkMath.EyeViewport(eye);
            cam.nearClipPlane = nearClip;
            cam.farClipPlane = farClip;
            cam.projectionMatrix = QuestLinkMath.Projection(info.Fov(eye), nearClip, farClip);
        }

        public static void Release(RenderTexture rt)
        {
            if (rt == null)
                return;
            rt.Release();
            if (Application.isPlaying) Object.Destroy(rt);
            else Object.DestroyImmediate(rt);
        }
    }
}
