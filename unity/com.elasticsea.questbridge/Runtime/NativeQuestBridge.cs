// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
using System;
using System.Runtime.InteropServices;
using ElasticSea.QuestBridge.Native;
using UnityEngine;
using UnityEngine.Rendering;
using static ElasticSea.QuestBridge.Native.QuestBridgeNative;

namespace ElasticSea.QuestBridge
{
    // Backend over libquestbridge. The native session outlives Play mode in the Editor
    // (native plugins stay loaded); this object only owns one qb_open/qb_close pair.
    public sealed class NativeQuestBridge : IQuestBridgeBackend, IQuestBridgeStatusSource
    {
        // Submit payloads in flight to the render thread; a ring deep enough for a few queued frames.
        // Process-lifetime on purpose: a queued render event may still read one after Dispose.
        const int RingSize = 4;
        static readonly IntPtr[] ring = AllocRing();
        static int ringNext;

        volatile IntPtr session;  // also read on the audio thread
        volatile bool audioAvailable = true;
        bool statusAvailable = true;
        readonly object micScratchLock = new object();
        short[] micScratch = new short[4800];
        readonly IntPtr renderEvent;
        readonly CommandBuffer commands = new CommandBuffer { name = "QuestBridge Submit" };
        // Top-down copy of the eye texture handed to the encoder (see SubmitFrame)
        RenderTexture upright;
        IntPtr uprightPtr;

        public string Name => "Native";

        NativeQuestBridge(IntPtr session, IntPtr renderEvent)
        {
            this.session = session;
            this.renderEvent = renderEvent;
        }

        static IntPtr[] AllocRing()
        {
            var r = new IntPtr[RingSize];
            for (var i = 0; i < RingSize; i++)
                r[i] = Marshal.AllocHGlobal(Marshal.SizeOf<qb_frame_submit>());
            return r;
        }

        // Null when the library is missing or qb_open fails.
        public static NativeQuestBridge TryCreate()
        {
            // The native texture path takes ID3D11Texture2D on Windows, id<MTLTexture> on macOS
            var api = SystemInfo.graphicsDeviceType;
            if (api != GraphicsDeviceType.Direct3D11 && api != GraphicsDeviceType.Metal)
            {
                Debug.LogWarning($"[QuestBridge] libquestbridge needs Direct3D11 (Windows) or Metal, not {api}.");
                return null;
            }
            try
            {
                var s = qb_open();
                if (s == IntPtr.Zero)
                    return null;
                return new NativeQuestBridge(s, qb_get_render_event_func());
            }
            catch (DllNotFoundException) { return null; }
            catch (EntryPointNotFoundException e)
            {
                Debug.LogWarning($"[QuestBridge] libquestbridge does not match the expected C API: {e.Message}");
                return null;
            }
        }

        public QuestBridgeState Poll()
        {
            switch (qb_poll(session))
            {
                case StateStreaming: return QuestBridgeState.Streaming;
                case StateConnecting: return QuestBridgeState.Connecting;
                default: return QuestBridgeState.Idle;
            }
        }

        public bool TryGetStatus(out QuestBridgeStatus status)
        {
            status = default;
            if (!statusAvailable)
                return false;
            qb_link_status s;
            try
            {
                if (qb_get_link_status(session, out s) != 0)
                    return false;
            }
            catch (EntryPointNotFoundException) { statusAvailable = false; return false; }  // older libquestbridge
            status = new QuestBridgeStatus
            {
                state = (QuestBridgeLinkState)s.state,
                lastLoss = (QuestBridgeLossReason)s.last_loss,
                connects = (int)s.connects,
                disconnects = (int)s.disconnects,
                resumes = (int)s.resumes,
                encoder = s.encoder ?? "",
            };
            return true;
        }

        public bool TryGetDeviceInfo(out QuestBridgeDeviceInfo info)
        {
            info = default;
            if (qb_get_device_info(session, out var d) != 0)
                return false;
            info = new QuestBridgeDeviceInfo
            {
                encodeWidth = (int)d.encode_width,
                encodeHeight = (int)d.encode_height,
                refreshHz = d.stream_refresh_hz != 0 ? d.stream_refresh_hz : d.refresh_hz,
                ipd = d.ipd_m,
                leftFov = ToFov(d.fov_left),
                rightFov = ToFov(d.fov_right),
            };
            return true;
        }

        public bool TryGetHeadPose(out QuestBridgePose pose)
        {
            pose = QuestBridgePose.Identity;
            if (qb_get_head_pose(session, out var p) != 0)
                return false;
            pose = ToUnity(p);
            return true;
        }

        public bool TryGetController(QuestBridgeHand hand, out QuestBridgeControllerState state)
        {
            state = default;
            return qb_get_controller(session, (int)hand, out var c) == 0 && FromNative(c, out state);
        }

        // False (state default) while the controller is not connected (put down, hands tracked).
        internal static bool FromNative(in qb_controller c, out QuestBridgeControllerState state)
        {
            state = default;
            if ((c.flags & FlagConnected) == 0)
                return false;
            state = new QuestBridgeControllerState
            {
                connected = true,
                positionTracked = (c.flags & FlagPositionTracked) != 0,
                rotationTracked = (c.flags & FlagRotationTracked) != 0,
                pose = ToUnity(c.pose),
                trigger = c.trigger,
                grip = c.grip,
                thumbstick = new Vector2(c.stick_x, c.stick_y),
                primaryButton = (c.buttons & ButtonPrimary) != 0,
                secondaryButton = (c.buttons & ButtonSecondary) != 0,
                thumbstickClick = (c.buttons & ButtonThumbstick) != 0,
                menu = (c.buttons & ButtonMenu) != 0,
                primaryTouch = (c.touches & ButtonPrimary) != 0,
                secondaryTouch = (c.touches & ButtonSecondary) != 0,
                thumbstickTouch = (c.touches & ButtonThumbstick) != 0,
                triggerTouch = (c.touches & TouchTrigger) != 0,
                thumbrestTouch = (c.touches & TouchThumbrest) != 0,
            };
            return true;
        }

        public bool TryGetHand(QuestBridgeHand hand, QuestBridgeHandState state)
        {
            state.Clear();
            if (qb_get_hands(session, out var hands) != 0)
                return false;
            FromNative(hand == QuestBridgeHand.Left ? hands.left : hands.right, state);
            return true;
        }

        // By value: fixed buffers of a local need no pinning
        internal static unsafe void FromNative(qb_hand h, QuestBridgeHandState state)
        {
            state.Clear();
            state.tracked = (h.flags & HandTracked) != 0;
            if (!state.tracked)
                return;
            state.confidence = h.confidence;
            state.scale = h.scale;
            state.dominant = (h.flags & HandDominant) != 0;
            state.aimValid = (h.flags & HandAimValid) != 0;
            state.aim = ToUnity(h.aim);
            state.timeNs = (long)h.root.time_ns;
            for (var i = 0; i < 4; i++)
                state.pinch[i] = h.pinch[i];
            for (var i = 0; i < HandJointCount; i++)
            {
                var f = 8 * i;
                state.joints[i] = new QuestBridgeJointPose
                {
                    rotation = QuestBridgeMath.LinkToUnity(new Quaternion(h.joints[f], h.joints[f + 1], h.joints[f + 2], h.joints[f + 3])),
                    position = QuestBridgeMath.LinkToUnity(new Vector3(h.joints[f + 4], h.joints[f + 5], h.joints[f + 6])),
                    radius = h.joints[f + 7],
                };
            }
        }

        public void SendHaptic(QuestBridgeHand hand, float amplitude, float durationSeconds) =>
            qb_haptic(session, (int)hand, amplitude, durationSeconds);

        public void SubmitFrame(RenderTexture eyes, QuestBridgePose renderPose, ulong frameIndex)
        {
            commands.Clear();
            var source = eyes;
            if (NeedsFlip)
            {
                // Unity stores render textures bottom row first on D3D/Metal; the encoder reads
                // rows top-down, so hand it a flipped copy (this also resolves MSAA).
                if (upright == null || upright.width != eyes.width || upright.height != eyes.height)
                {
                    ReleaseUpright();
                    upright = new RenderTexture(eyes.width, eyes.height, 0, eyes.format,
                        eyes.sRGB ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear) { name = "QuestBridge Upright" };
                    upright.Create();
                    uprightPtr = upright.GetNativeTexturePtr();
                }
                commands.Blit(eyes, upright, new Vector2(1f, -1f), new Vector2(0f, 1f));
                source = upright;
            }

            var payload = ring[ringNext];
            ringNext = (ringNext + 1) % RingSize;
            Marshal.StructureToPtr(new qb_frame_submit
            {
                session = session,
                native_texture = source == upright ? uprightPtr : source.GetNativeTexturePtr(),
                render_pose = ToLink(renderPose),
                frame_index = frameIndex,
            }, payload, false);

            commands.IssuePluginEventAndData(renderEvent, EventSubmitFrame, payload);
            Graphics.ExecuteCommandBuffer(commands);
        }

        public void SubmitAudio(float[] stereo, int frames, int sampleRate)
        {
            var s = session;
            if (s == IntPtr.Zero || !audioAvailable || frames <= 0)
                return;
            try { qb_submit_audio(s, stereo, (uint)frames, (uint)sampleRate); }
            catch (EntryPointNotFoundException) { audioAvailable = false; }  // older libquestbridge
        }

        public int ReadMicrophone(float[] samples, int offset, int count)
        {
            var s = session;
            if (s == IntPtr.Zero || !audioAvailable || count <= 0)
                return 0;
            lock (micScratchLock)
            {
                if (micScratch.Length < count)
                    micScratch = new short[count];
                int n;
                try { n = qb_read_mic(s, micScratch, (uint)count); }
                catch (EntryPointNotFoundException) { audioAvailable = false; return 0; }
                for (var i = 0; i < n; i++)
                    samples[offset + i] = micScratch[i] / 32768f;
                return Math.Max(n, 0);
            }
        }

        // Hand the headset's Link session to another host: stop sending, log the resume numbers
        // (handshake.py XRSP_ATTACH) to the native log, close USB. Final for this process.
        public static void Shutdown()
        {
            try { qb_shutdown(); }
            catch (Exception e) when (e is EntryPointNotFoundException || e is DllNotFoundException) { }
        }

        // True when libquestbridge loads and exports the C API; doesn't start the runtime or open USB.
        public static bool LibraryLoads(out string error)
        {
            error = null;
            try
            {
                qb_now_ns();
                qb_get_render_event_func();
                return true;
            }
            catch (Exception e) when (e is EntryPointNotFoundException || e is DllNotFoundException)
            {
                error = e.Message;
                return false;
            }
        }

        public void Dispose()
        {
            if (session == IntPtr.Zero)
                return;
            // qb_close must tolerate submit events for this session still queued on the render thread.
            qb_close(session);
            session = IntPtr.Zero;
            commands.Release();
            ReleaseUpright();
        }

        // Render textures are stored upside down where UV v=0 is the top row of memory.
        internal static bool NeedsFlip => SystemInfo.graphicsUVStartsAtTop;

        void ReleaseUpright()
        {
            if (upright == null)
                return;
            upright.Release();
            if (Application.isPlaying) UnityEngine.Object.Destroy(upright);
            else UnityEngine.Object.DestroyImmediate(upright);
            upright = null;
            uprightPtr = IntPtr.Zero;
        }

        static EyeFov ToFov(qb_fov f) => new EyeFov { up = f.up, down = f.down, left = f.left, right = f.right };

        static QuestBridgePose ToUnity(qb_pose p) => new QuestBridgePose
        {
            position = QuestBridgeMath.LinkToUnity(new Vector3(p.px, p.py, p.pz)),
            rotation = QuestBridgeMath.LinkToUnity(new Quaternion(p.qx, p.qy, p.qz, p.qw)),
            timeNs = (long)p.time_ns,
        };

        static qb_pose ToLink(QuestBridgePose p)
        {
            var pos = QuestBridgeMath.UnityToLink(p.position);
            var rot = QuestBridgeMath.UnityToLink(p.rotation);
            return new qb_pose { px = pos.x, py = pos.y, pz = pos.z, qx = rot.x, qy = rot.y, qz = rot.z, qw = rot.w, time_ns = (ulong)p.timeNs };
        }
    }
}
