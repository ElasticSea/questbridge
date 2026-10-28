using System;
using System.Runtime.InteropServices;

namespace ElasticSea.QuestLink.Native
{
    // P/Invoke surface of libquestlink. Every assumption about the C API lives in this file
    // (see the package README, "Assumed C API"). Poses here are raw Link convention:
    // right-handed, +Y up, -Z forward, metres, Quest clock nanoseconds.
    public static class QuestLinkNative
    {
        // Resolves libquestlink.dylib (macOS), questlink.dll (Windows), libquestlink.so (Linux).
        public const string Library = "questlink";

        // Render-thread event ids passed to ql_get_render_event_func().
        public const int EventSubmitFrame = 1;

        // ql_poll() return values.
        public const int StateIdle = 0;        // no headset
        public const int StateConnecting = 1;  // USB open, handshake running
        public const int StateStreaming = 2;   // session up, poses arriving, frames accepted

        [StructLayout(LayoutKind.Sequential)]
        public struct ql_pose
        {
            public float qx, qy, qz, qw;
            public float px, py, pz;
            public ulong time_ns;  // Quest clock
        }

        // Positive tangents of the half-angles, OVR FovPort order.
        [StructLayout(LayoutKind.Sequential)]
        public struct ql_fov
        {
            public float up, down, left, right;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ql_device_info
        {
            public uint encode_width;
            public uint encode_height;
            public float refresh_hz;
            public float ipd_m;
            public ql_fov fov_left;
            public ql_fov fov_right;
        }

        // ql_controller.buttons / touches bits.
        public const uint ButtonPrimary = 1 << 0;    // A / X
        public const uint ButtonSecondary = 1 << 1;  // B / Y
        public const uint ButtonThumbstick = 1 << 2;
        public const uint ButtonMenu = 1 << 3;
        public const uint TouchTrigger = 1 << 4;     // touches only

        // ql_controller.flags bits.
        public const uint FlagConnected = 1 << 0;
        public const uint FlagPositionTracked = 1 << 1;
        public const uint FlagRotationTracked = 1 << 2;

        [StructLayout(LayoutKind.Sequential)]
        public struct ql_controller
        {
            public ql_pose pose;
            public uint buttons;
            public uint touches;
            public float trigger, grip;
            public float stick_x, stick_y;
            public uint flags;
        }

        // Hand tracking (questlink.h "Hand tracking"). Joints in XrHandJointEXT order with OpenXR axes.
        public const int HandJointCount = 26;
        public const uint HandTracked = 1 << 0;
        public const uint HandAimValid = 1 << 1;
        public const uint HandDominant = 1 << 2;
        public const uint HandHeadsetSkeleton = 1 << 3;

        // 8 floats per joint: qx qy qz qw px py pz radius
        [StructLayout(LayoutKind.Sequential)]
        public unsafe struct ql_hand
        {
            public uint flags;
            public uint input_status;
            public float confidence;
            public float scale;
            public fixed float pinch[4];  // index, middle, ring, little
            public ql_pose root;          // wrist, OVR bone frame
            public ql_pose aim;
            public fixed float joints[HandJointCount * 8];
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ql_hands
        {
            public ql_hand left, right;
            public ulong count;
        }

        // Payload of EventSubmitFrame. Must stay alive until the render thread has run the event.
        [StructLayout(LayoutKind.Sequential)]
        public struct ql_frame_submit
        {
            public IntPtr session;
            public IntPtr native_texture;  // id<MTLTexture> on Metal
            public ql_pose render_pose;
            public ulong frame_index;
        }

        [DllImport(Library)] public static extern IntPtr ql_open();
        [DllImport(Library)] public static extern void ql_close(IntPtr session);
        [DllImport(Library)] public static extern int ql_poll(IntPtr session);
        [DllImport(Library)] public static extern int ql_get_device_info(IntPtr session, out ql_device_info info);
        [DllImport(Library)] public static extern int ql_get_head_pose(IntPtr session, out ql_pose pose);
        [DllImport(Library)] public static extern int ql_get_controller(IntPtr session, int hand, out ql_controller controller);
        [DllImport(Library)] public static extern int ql_get_hands(IntPtr session, out ql_hands hands);
        [DllImport(Library)] public static extern int ql_haptic(IntPtr session, int hand, float amplitude, float duration_s);
        [DllImport(Library)] public static extern int ql_submit_frame_texture(IntPtr session, IntPtr native_texture, ref ql_pose render_pose, ulong frame_index);
        [DllImport(Library)] public static extern IntPtr ql_get_render_event_func();

        // Audio: game audio to the headset, headset microphone back (48 kHz mono int16).
        public const int MicSampleRate = 48000;
        [DllImport(Library)] public static extern int ql_submit_audio(IntPtr session, float[] interleaved_stereo, uint frames, uint sample_rate);
        [DllImport(Library)] public static extern int ql_read_mic(IntPtr session, short[] samples, uint max_frames);
    }
}
