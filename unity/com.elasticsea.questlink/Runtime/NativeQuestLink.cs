using System;
using System.Runtime.InteropServices;
using ElasticSea.QuestLink.Native;
using UnityEngine;
using UnityEngine.Rendering;
using static ElasticSea.QuestLink.Native.QuestLinkNative;

namespace ElasticSea.QuestLink
{
    // Backend over libquestlink. The native session outlives Play mode in the Editor
    // (native plugins stay loaded); this object only owns one ql_open/ql_close pair.
    public sealed class NativeQuestLink : IQuestLinkBackend
    {
        // Submit payloads in flight to the render thread; a ring deep enough for a few queued frames.
        // Process-lifetime on purpose: a queued render event may still read one after Dispose.
        const int RingSize = 4;
        static readonly IntPtr[] ring = AllocRing();
        static int ringNext;

        IntPtr session;
        readonly IntPtr renderEvent;
        readonly CommandBuffer commands = new CommandBuffer { name = "QuestLink Submit" };

        public string Name => "Native";

        NativeQuestLink(IntPtr session, IntPtr renderEvent)
        {
            this.session = session;
            this.renderEvent = renderEvent;
        }

        static IntPtr[] AllocRing()
        {
            var r = new IntPtr[RingSize];
            for (var i = 0; i < RingSize; i++)
                r[i] = Marshal.AllocHGlobal(Marshal.SizeOf<ql_frame_submit>());
            return r;
        }

        // Null when the library is missing or ql_open fails.
        public static NativeQuestLink TryCreate()
        {
            try
            {
                var s = ql_open();
                if (s == IntPtr.Zero)
                    return null;
                return new NativeQuestLink(s, ql_get_render_event_func());
            }
            catch (DllNotFoundException) { return null; }
            catch (EntryPointNotFoundException e)
            {
                Debug.LogWarning($"[QuestLink] libquestlink does not match the expected C API: {e.Message}");
                return null;
            }
        }

        public QuestLinkState Poll()
        {
            switch (ql_poll(session))
            {
                case StateStreaming: return QuestLinkState.Streaming;
                case StateConnecting: return QuestLinkState.Connecting;
                default: return QuestLinkState.Idle;
            }
        }

        public bool TryGetDeviceInfo(out QuestLinkDeviceInfo info)
        {
            info = default;
            if (ql_get_device_info(session, out var d) != 0)
                return false;
            info = new QuestLinkDeviceInfo
            {
                encodeWidth = (int)d.encode_width,
                encodeHeight = (int)d.encode_height,
                refreshHz = d.refresh_hz,
                ipd = d.ipd_m,
                leftFov = ToFov(d.fov_left),
                rightFov = ToFov(d.fov_right),
            };
            return true;
        }

        public bool TryGetHeadPose(out QuestLinkPose pose)
        {
            pose = QuestLinkPose.Identity;
            if (ql_get_head_pose(session, out var p) != 0)
                return false;
            pose = ToUnity(p);
            return true;
        }

        public bool TryGetController(QuestLinkHand hand, out QuestLinkControllerState state)
        {
            state = default;
            if (ql_get_controller(session, (int)hand, out var c) != 0 || (c.flags & FlagConnected) == 0)
                return false;
            state = new QuestLinkControllerState
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
            };
            return true;
        }

        public void SendHaptic(QuestLinkHand hand, float amplitude, float durationSeconds) =>
            ql_haptic(session, (int)hand, amplitude, durationSeconds);

        public void SubmitFrame(RenderTexture eyes, QuestLinkPose renderPose, ulong frameIndex)
        {
            var payload = ring[ringNext];
            ringNext = (ringNext + 1) % RingSize;
            Marshal.StructureToPtr(new ql_frame_submit
            {
                session = session,
                native_texture = eyes.GetNativeTexturePtr(),
                render_pose = ToLink(renderPose),
                frame_index = frameIndex,
            }, payload, false);

            commands.Clear();
            commands.IssuePluginEventAndData(renderEvent, EventSubmitFrame, payload);
            Graphics.ExecuteCommandBuffer(commands);
        }

        public void Dispose()
        {
            if (session == IntPtr.Zero)
                return;
            // ql_close must tolerate submit events for this session still queued on the render thread.
            ql_close(session);
            session = IntPtr.Zero;
            commands.Release();
        }

        static EyeFov ToFov(ql_fov f) => new EyeFov { up = f.up, down = f.down, left = f.left, right = f.right };

        static QuestLinkPose ToUnity(ql_pose p) => new QuestLinkPose
        {
            position = QuestLinkMath.LinkToUnity(new Vector3(p.px, p.py, p.pz)),
            rotation = QuestLinkMath.LinkToUnity(new Quaternion(p.qx, p.qy, p.qz, p.qw)),
            timeNs = (long)p.time_ns,
        };

        static ql_pose ToLink(QuestLinkPose p)
        {
            var pos = QuestLinkMath.UnityToLink(p.position);
            var rot = QuestLinkMath.UnityToLink(p.rotation);
            return new ql_pose { px = pos.x, py = pos.y, pz = pos.z, qx = rot.x, qy = rot.y, qz = rot.z, qw = rot.w, time_ns = (ulong)p.timeNs };
        }
    }
}
