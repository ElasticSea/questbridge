using UnityEngine;

namespace ElasticSea.QuestLink
{
    public enum QuestLinkEye { Left = 0, Right = 1 }

    public enum QuestLinkHand { Left = 0, Right = 1 }

    public enum QuestLinkState { Idle, Connecting, Streaming }

    // Values of questlink.h ql_link_state
    public enum QuestLinkLinkState
    {
        NoDevice,          // no Link interface on USB: unplugged, asleep, Link quit, or Meta's service holds it
        Connecting,        // handshake or stream setup running
        WaitingForLaunch,  // the user presses Launch in the headset
        Streaming,
        Resuming,          // USB came back right after an error; continuing the stream if the headset still has it
    }

    // Why the latest stream ended (questlink.h ql_loss)
    public enum QuestLinkLossReason { None, Usb, HeadsetBye, NewHello, Silence }

    public struct QuestLinkStatus
    {
        public QuestLinkLinkState state;
        public QuestLinkLossReason lastLoss;
        public int connects;     // streams started or resumed
        public int disconnects;  // streams lost
        public int resumes;      // streams that continued after a USB error without a new handshake
        public string encoder;   // video encoder backend ("mf-nvidia: ...", "openh264: ..."); empty until the first frame

        public bool Reconnecting => disconnects > 0 && state != QuestLinkLinkState.Streaming;
    }

    // Pose in Unity convention (left-handed, +Y up, +Z forward), relative to the tracking origin.
    public struct QuestLinkPose
    {
        public Vector3 position;
        public Quaternion rotation;
        public long timeNs;  // headset clock; 0 when unknown

        public static QuestLinkPose Identity => new QuestLinkPose { rotation = Quaternion.identity };
    }

    // Positive tangents of the eye's half-angles (OVR FovPort).
    public struct EyeFov
    {
        public float up, down, left, right;

        public static EyeFov FromDegrees(float up, float down, float left, float right) => new EyeFov
        {
            up = Mathf.Tan(up * Mathf.Deg2Rad),
            down = Mathf.Tan(down * Mathf.Deg2Rad),
            left = Mathf.Tan(left * Mathf.Deg2Rad),
            right = Mathf.Tan(right * Mathf.Deg2Rad),
        };
    }

    public struct QuestLinkDeviceInfo
    {
        public int encodeWidth, encodeHeight;
        public float refreshHz;
        public float ipd;  // metres
        public EyeFov leftFov, rightFov;

        // Quest 3 values from its HELLO device description (PROTOCOL_NOTES.md). The angle order
        // in HELLO is up/down/left/right: left eye 44/55/54/40, right eye mirrored 44/55/40/54.
        public static QuestLinkDeviceInfo Quest3Default => new QuestLinkDeviceInfo
        {
            encodeWidth = 4096,
            encodeHeight = 2240,
            refreshHz = 72f,
            ipd = 0.0635f,
            leftFov = EyeFov.FromDegrees(44f, 55f, 54f, 40f),
            rightFov = EyeFov.FromDegrees(44f, 55f, 40f, 54f),
        };

        public EyeFov Fov(QuestLinkEye eye) => eye == QuestLinkEye.Left ? leftFov : rightFov;
    }

    public struct QuestLinkControllerState
    {
        public bool connected, positionTracked, rotationTracked;
        public QuestLinkPose pose;
        public float trigger, grip;
        public Vector2 thumbstick;
        public bool primaryButton, secondaryButton, thumbstickClick, menu;
        public bool primaryTouch, secondaryTouch, thumbstickTouch, triggerTouch, thumbrestTouch;
    }
}
