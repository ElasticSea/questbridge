// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Runtime.InteropServices;

namespace ElasticSea.QuestBridge.Native
{
    // P/Invoke surface of libquestbridge. Every assumption about the C API lives in this file
    // (see the package README, "Assumed C API"). Poses here are raw Link convention:
    // right-handed, +Y up, -Z forward, metres, Quest clock nanoseconds.
    public static class QuestBridgeNative
    {
        // Resolves libquestbridge.dylib (macOS), questbridge.dll (Windows), libquestbridge.so (Linux).
        public const string Library = "questbridge";

        // Render-thread event ids passed to qb_get_render_event_func().
        public const int EventSubmitFrame = 1;

        // qb_poll() return values.
        public const int StateIdle = 0;        // no headset
        public const int StateConnecting = 1;  // USB open, handshake running
        public const int StateStreaming = 2;   // session up, poses arriving, frames accepted

        [StructLayout(LayoutKind.Sequential)]
        public struct qb_pose
        {
            public float qx, qy, qz, qw;
            public float px, py, pz;
            public ulong time_ns;  // Quest clock
        }

        // Positive tangents of the half-angles, OVR FovPort order.
        [StructLayout(LayoutKind.Sequential)]
        public struct qb_fov
        {
            public float up, down, left, right;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct qb_device_info
        {
            public uint encode_width;
            public uint encode_height;
            public float refresh_hz;
            public float ipd_m;
            public qb_fov fov_left;
            public qb_fov fov_right;
            // API version 3
            public uint panel_width;
            public uint panel_height;
            public uint refresh_count;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] refresh_rates;
            public uint stream_refresh_hz;  // the rate the Link session runs at; 0 before phase 2
            public uint codecs;             // bit n = video codec n
            public uint hello_version;
            public ulong features;          // bit n = session feature n
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string product;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string manufacturer;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string firmware;
            public uint stream_codec;       // the codec the Link session runs (0 H.264, 1 HEVC)
            public uint max_slice_bytes;    // API 6: largest slice we send this headset
            public uint max_message_bytes;  // API 6: largest XRSP message on any topic
            public uint default_bitrate;    // API 6: our default video bit/s for this headset
        }

        // qb_controller.buttons / touches bits.
        public const uint ButtonPrimary = 1 << 0;    // A / X
        public const uint ButtonSecondary = 1 << 1;  // B / Y
        public const uint ButtonThumbstick = 1 << 2;
        public const uint ButtonMenu = 1 << 3;
        public const uint TouchTrigger = 1 << 4;     // touches only
        public const uint TouchThumbrest = 1 << 5;   // touches only

        // qb_controller.flags bits.
        public const uint FlagConnected = 1 << 0;
        public const uint FlagPositionTracked = 1 << 1;
        public const uint FlagRotationTracked = 1 << 2;

        [StructLayout(LayoutKind.Sequential)]
        public struct qb_controller
        {
            public qb_pose pose;
            public uint buttons;
            public uint touches;
            public float trigger, grip;
            public float stick_x, stick_y;
            public uint flags;
        }

        // Hand tracking (questbridge.h "Hand tracking"). Joints in XrHandJointEXT order with OpenXR axes.
        public const int HandJointCount = 26;
        public const uint HandTracked = 1 << 0;
        public const uint HandAimValid = 1 << 1;
        public const uint HandDominant = 1 << 2;
        public const uint HandHeadsetSkeleton = 1 << 3;

        // 8 floats per joint: qx qy qz qw px py pz radius
        [StructLayout(LayoutKind.Sequential)]
        public unsafe struct qb_hand
        {
            public uint flags;
            public uint input_status;
            public float confidence;
            public float scale;
            public fixed float pinch[4];  // index, middle, ring, little
            public qb_pose root;          // wrist, OVR bone frame
            public qb_pose aim;
            public fixed float joints[HandJointCount * 8];
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct qb_hands
        {
            public qb_hand left, right;
            public ulong count;
        }

        // Payload of EventSubmitFrame. Must stay alive until the render thread has run the event.
        [StructLayout(LayoutKind.Sequential)]
        public struct qb_frame_submit
        {
            public IntPtr session;
            public IntPtr native_texture;  // id<MTLTexture> on Metal
            public qb_pose render_pose;
            public ulong frame_index;
        }

        // API version 4: link status (questbridge.h qb_link_status)
        [StructLayout(LayoutKind.Sequential)]
        public struct qb_link_status
        {
            public int state;      // qb_link_state = QuestBridgeLinkState
            public int last_loss;  // qb_loss = QuestBridgeLossReason
            public uint connects;
            public uint disconnects;
            public uint resumes;
            public uint reserved;
            public long state_since_ns;
            // API version 5
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 160)] public string encoder;
        }

        [DllImport(Library)] public static extern IntPtr qb_open();
        [DllImport(Library)] public static extern void qb_close(IntPtr session);
        [DllImport(Library)] public static extern void qb_shutdown();
        [DllImport(Library)] public static extern int qb_poll(IntPtr session);
        [DllImport(Library)] public static extern int qb_get_link_status(IntPtr session, out qb_link_status status);
        [DllImport(Library)] public static extern int qb_get_device_info(IntPtr session, out qb_device_info info);
        [DllImport(Library)] public static extern int qb_get_head_pose(IntPtr session, out qb_pose pose);
        [DllImport(Library)] public static extern int qb_get_controller(IntPtr session, int hand, out qb_controller controller);
        [DllImport(Library)] public static extern int qb_get_hands(IntPtr session, out qb_hands hands);
        [DllImport(Library)] public static extern int qb_haptic(IntPtr session, int hand, float amplitude, float duration_s);
        [DllImport(Library)] public static extern int qb_submit_frame_texture(IntPtr session, IntPtr native_texture, ref qb_pose render_pose, ulong frame_index);
        [DllImport(Library)] public static extern IntPtr qb_get_render_event_func();
        // Monotonic clock; touches nothing else (load checks)
        [DllImport(Library)] public static extern long qb_now_ns();

        // Audio: game audio to the headset, headset microphone back (48 kHz mono int16).
        public const int MicSampleRate = 48000;
        [DllImport(Library)] public static extern int qb_submit_audio(IntPtr session, float[] interleaved_stereo, uint frames, uint sample_rate);
        [DllImport(Library)] public static extern int qb_read_mic(IntPtr session, short[] samples, uint max_frames);
    }
}
