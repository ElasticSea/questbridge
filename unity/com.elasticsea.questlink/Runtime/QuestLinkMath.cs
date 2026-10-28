using UnityEngine;

namespace ElasticSea.QuestLink
{
    public static class QuestLinkMath
    {
        // Off-axis projection (OpenGL convention, as Camera.projectionMatrix expects) from FOV tangents.
        public static Matrix4x4 Projection(EyeFov fov, float near, float far) =>
            Matrix4x4.Frustum(-fov.left * near, fov.right * near, -fov.down * near, fov.up * near, near, far);

        // Stream layout: one encode-sized texture, left eye in the left half, right eye in the right half.
        public static Rect EyeViewport(QuestLinkEye eye) =>
            eye == QuestLinkEye.Left ? new Rect(0f, 0f, 0.5f, 1f) : new Rect(0.5f, 0f, 0.5f, 1f);

        public static RectInt EyePixelRect(QuestLinkEye eye, int width, int height)
        {
            var half = width / 2;
            return new RectInt(eye == QuestLinkEye.Left ? 0 : half, 0, half, height);
        }

        // Eye position in head space; Link IPD is centred on the head pose.
        public static Vector3 EyeOffset(QuestLinkEye eye, float ipd) =>
            new Vector3(eye == QuestLinkEye.Left ? -ipd * 0.5f : ipd * 0.5f, 0f, 0f);

        // Link (right-handed, -Z forward) <-> Unity (left-handed, +Z forward): mirror Z.
        public static Vector3 LinkToUnity(Vector3 p) => new Vector3(p.x, p.y, -p.z);
        public static Quaternion LinkToUnity(Quaternion q) => new Quaternion(-q.x, -q.y, q.z, q.w);
        public static Vector3 UnityToLink(Vector3 p) => LinkToUnity(p);
        public static Quaternion UnityToLink(Quaternion q) => LinkToUnity(q);
    }
}
