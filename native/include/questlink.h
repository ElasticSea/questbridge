/* questlink: Meta Link (XRSP over USB) host library.
 *
 * Speaks the Quest's built-in Link protocol: waits for the headset's HELLO, runs the
 * two-phase handshake (the user presses "Enable Link" in between), keeps the clock in sync,
 * streams H.264 slices and reports head pose.
 *
 * Two layers:
 *  - Session API (ql_open, ql_poll, ql_get_head_pose, ql_submit_frame_texture, ...): what the
 *    Unity package (unity/com.elasticsea.questlink, Runtime/Native/QuestLinkNative.cs) calls.
 *    One process-wide Link runtime does USB, handshake and encoding on background threads and
 *    outlives every ql_session, so the headset session survives Play-mode exits.
 *  - Host API (ql_host_*): one connection driven by the caller. Used by qlhost and the tests.
 *
 * Poses are raw Link convention: right-handed, +Y up, -Z forward, metres, Quest-clock ns.
 * Return values: 0 = success unless noted.
 */
#ifndef QUESTLINK_H
#define QUESTLINK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(QL_BUILD_SHARED)
#    define QL_API __declspec(dllexport)
#  else
#    define QL_API __declspec(dllimport)
#  endif
#  define QL_UNITY_CALL __stdcall
#else
#  define QL_API __attribute__((visibility("default")))
#  define QL_UNITY_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QL_API_VERSION 3

typedef enum ql_status {
    QL_OK = 0,
    QL_ERR_ARGUMENT = -1,
    QL_ERR_NO_DEVICE = -2,      /* Quest XRSP interface not found or busy (OVRService running?) */
    QL_ERR_TRANSPORT = -3,      /* USB I/O failed: unplugged or reset; close and reopen */
    QL_ERR_NOT_STREAMING = -4,  /* handshake / stream setup not finished yet */
    QL_ERR_NO_CLOCK = -5,       /* no Quest clock ping received yet */
    QL_ERR_FAILED = -6,         /* session failed (see ql_host_get_error) */
    QL_ERR_INTERNAL = -7,
    QL_ERR_NO_DATA = -8,        /* nothing received yet (pose, HELLO) */
    QL_ERR_UNSUPPORTED = -9     /* not decoded / not implemented on this platform yet */
} ql_status;

typedef struct ql_pose {
    float qx, qy, qz, qw; /* orientation quaternion */
    float px, py, pz;     /* position, metres */
    uint64_t time_ns;     /* Quest clock (sample time); ignored when submitting */
} ql_pose;

typedef struct ql_fov {
    float up, down, left, right; /* positive tangents of the half-angles (OVR FovPort order) */
} ql_fov;

/* From the headset's HELLO device description (docs/PROTOCOL.md §8). */
#define QL_MAX_REFRESH_RATES 8

typedef struct ql_device_info {
    uint32_t encode_width, encode_height; /* video stream size (4096x2240 on Quest 3) */
    float refresh_hz;                     /* the panel's default rate */
    float ipd_m;
    ql_fov fov_left, fov_right;           /* each eye's FOV as the headset reports it */
    /* API version 3 */
    uint32_t panel_width, panel_height;   /* both eyes (4128x2208 on Quest 3) */
    uint32_t refresh_count;
    uint32_t refresh_rates[QL_MAX_REFRESH_RATES]; /* offered rates, Hz, ascending */
    uint32_t stream_refresh_hz;           /* the rate our CONFIG chose; 0 before phase 2 */
    uint32_t codecs;                      /* bit n: headset lists video codec n (0 AVC, 1 HEVC?); 0 on Quest 3 */
    uint32_t hello_version;               /* 1 (phase 1) or 3 (Link session) */
    uint64_t features;                    /* bit n: headset offers session feature n (HELLO v3) */
    char product[32];                     /* "Meta Quest 3" */
    char manufacturer[32];                /* "Oculus VR" */
    char firmware[64];                    /* "releases-oculus-14.0-v207" */
} ql_device_info;

/* ql_controller.buttons / touches bits */
#define QL_BUTTON_PRIMARY (1u << 0)    /* A / X */
#define QL_BUTTON_SECONDARY (1u << 1)  /* B / Y */
#define QL_BUTTON_THUMBSTICK (1u << 2)
#define QL_BUTTON_MENU (1u << 3)
#define QL_TOUCH_TRIGGER (1u << 4) /* touches only */
/* ql_controller.flags bits */
#define QL_CONTROLLER_CONNECTED (1u << 0)
#define QL_CONTROLLER_POSITION_TRACKED (1u << 1)
#define QL_CONTROLLER_ROTATION_TRACKED (1u << 2)

typedef struct ql_controller {
    ql_pose pose;
    uint32_t buttons, touches;
    float trigger, grip;
    float stick_x, stick_y;
    uint32_t flags;
} ql_controller;

/* ======================= Session API (Unity) ============================================= */

typedef struct ql_session ql_session;

/* Payload of render event QL_EVENT_SUBMIT_FRAME; must stay valid until the event has run. */
typedef struct ql_frame_submit {
    ql_session* session;
    void* native_texture; /* ID3D11Texture2D* on D3D11, id<MTLTexture> on Metal */
    ql_pose render_pose;  /* the exact pose the frame was rendered with */
    uint64_t frame_index; /* caller's counter; may restart per ql_session */
} ql_frame_submit;

#define QL_EVENT_SUBMIT_FRAME 1

/* Non-blocking; starts the Link runtime on first use (it then waits for the Quest, does the
 * handshake and keeps the headset session up for later sessions). NULL only on failure. */
QL_API ql_session* ql_open(void);
/* Ends this handle; the runtime and headset session stay up. Submit events for the handle
 * still queued on the render thread are ignored. */
QL_API void ql_close(ql_session* session);
/* Stop sending (frames, audio, clock replies) and close USB, leaving the headset's Link session
 * up for another host: logs "resume with XRSP_FRAME_START=... XRSP_CONTROL_SEQ=...
 * XRSP_VIDEO_SEQ=... XRSP_STREAM_SEQ=..." (host/handshake.py XRSP_ATTACH) to QL_LOG_FILE or
 * stderr. Final for this process. */
QL_API void ql_shutdown(void);
/* 0 idle (no headset), 1 connecting (USB open, handshake or setup running), 2 streaming. */
QL_API int ql_poll(ql_session* session);
/* Nonzero until the HELLO arrived. */
QL_API int ql_get_device_info(ql_session* session, ql_device_info* out);
/* Newest head pose; nonzero until one arrived. */
QL_API int ql_get_head_pose(ql_session* session, ql_pose* out);
/* hand 0 = left, 1 = right. Not decoded yet: returns QL_ERR_UNSUPPORTED, flags = 0. */
QL_API int ql_get_controller(ql_session* session, int hand, ql_controller* out);
/* Not decoded yet: returns QL_ERR_UNSUPPORTED. */
QL_API int ql_haptic(ql_session* session, int hand, float amplitude, float duration_s);
/* Copy native_texture now (call on the graphics/render thread), encode and send it
 * asynchronously. Frames whose frame_index does not increase for this session are dropped;
 * the headset's frame.index is a separate session-wide counter. */
QL_API int ql_submit_frame_texture(ql_session* session, void* native_texture, const ql_pose* render_pose,
                                   uint64_t frame_index);
/* UnityRenderingEventAndData: event QL_EVENT_SUBMIT_FRAME, data = ql_frame_submit*. */
typedef void(QL_UNITY_CALL* ql_render_event_fn)(int event_id, void* data);
QL_API ql_render_event_fn ql_get_render_event_func(void);

/* ======================= Host API (one connection) ======================================= */

typedef struct ql_host ql_host;

typedef enum ql_state {
    QL_STATE_WAIT_HELLO = 0,
    QL_STATE_PHASE1 = 1,
    QL_STATE_WAIT_LINK_REQUEST = 2, /* waiting for the user to press "Enable Link" */
    QL_STATE_WAIT_HELLO_V3 = 3,
    QL_STATE_PHASE2 = 4,
    QL_STATE_SETUP = 5,             /* sending stream setup */
    QL_STATE_STREAMING = 6,         /* frames can be submitted */
    QL_STATE_FAILED = 7
} ql_state;

/* Log line from the protocol: direction "PC->Q", "Q->PC" or "info"; seq -1 = none. */
typedef void (*ql_log_fn)(void* user, const char* direction, const char* what, int seq);

typedef struct ql_config {
    /* Attach to a headset session an earlier host left running (no handshake, no setup);
     * continue its numbering. */
    int attach;
    uint16_t attach_control_seq; /* default 16 */
    uint16_t attach_video_seq;   /* default 0 */
    uint64_t first_frame_index;  /* default 1; frame.index must increase across the session */
    int send_setup;              /* send Meta's recorded stream setup after the handshake (default 1) */
    int hands;                   /* turn hand tracking on once streaming (InputControl, default 1) */
    int hello_timeout_ms;        /* 0 = wait forever (default) */
    int link_request_timeout_ms; /* waiting for "Enable Link"; 0 = forever (default) */
    int step_timeout_ms;         /* per handshake step; a stalled step restarts pairing (default 3000) */
    ql_log_fn log;
    void* log_user;
    /* While waiting for HELLO, send a BYE so a headset whose earlier HELLO went unanswered
     * sends a new one; repeat every bye_retry_ms (default 3000; 0 = once at open, < 0 = never). */
    int bye_retry_ms;
    uint32_t refresh_hz;         /* Link session rate; 0 = the headset's default (default 0) */
} ql_config;

/* Byte transport for ql_host_open_transport (tests, other backends). read returns bytes
 * read, 0 on timeout, < 0 on error; write returns 0 on success, < 0 on error. */
typedef struct ql_transport {
    void* user;
    int (*write)(void* user, const uint8_t* data, size_t len, int timeout_ms);
    int (*read)(void* user, uint8_t* buf, size_t cap, int timeout_ms);
    void (*close)(void* user); /* may be NULL */
} ql_transport;

typedef struct ql_head_pose {
    ql_pose pose; /* time_ns = sample time */
    float angular_velocity[3];
    float linear_velocity[3];
    float angular_acceleration[3];
    float linear_acceleration[3];
    uint64_t vsync_ns; /* Quest clock */
    uint64_t count;    /* poses received so far */
} ql_head_pose;

typedef struct ql_slice {
    const uint8_t* csd; /* Annex-B SPS+PPS for this slice, or NULL */
    size_t csd_len;
    const uint8_t* video; /* Annex-B slice NAL(s) */
    size_t video_len;
} ql_slice;

typedef struct ql_frame_info {
    uint64_t frame_index;          /* 0 = next (session-wide counter) */
    int64_t target_vsync_quest_ns; /* 0 = now + 20 ms in the Quest clock */
    int has_render_pose;           /* 0 = keep the template's identity pose */
    ql_pose render_pose;           /* the pose the frame was rendered with */
    int is_intra;
    /* Timewarp: the headset reprojects to render_pose only with rectify_mesh_id 1001 and the
     * real per-eye FOV (Meta's game frames). 0 / has_fov 0 keep the template's mesh 0 and
     * zero FOV (Meta's loading screen: shown head-locked, no reprojection). */
    uint32_t rectify_mesh_id;
    int has_fov;
    ql_fov fov_left, fov_right; /* the projections the eyes were rendered with */
    /* Picture height the slices split into equal macroblock-row strips; 0 = the HELLO's
     * encode height (or the recorded 2240 before a HELLO) */
    uint32_t picture_height;
} ql_frame_info;

typedef struct ql_info {
    ql_state state;
    uint64_t session_id;
    int has_clock_offset;
    int64_t quest_clock_offset_ns; /* Quest clock = ql_now_ns() + offset */
    uint64_t next_frame_index;
    uint16_t control_seq;
    uint16_t video_seq; /* next seq on topic 10 */
    uint64_t rx_counts[64]; /* messages received per topic */
    uint16_t tx_seq[64];    /* next seq per PC->Q stream topic, 0 = not used yet */
    uint32_t streams;       /* streaming sessions started on this host (a new one needs an IDR) */
    uint32_t byes_sent;     /* BYEs sent while waiting for HELLO */
} ql_info;

QL_API void ql_config_init(ql_config* cfg);

/* Open the Quest's XRSP USB interface (WinUSB on Windows, so adb keeps working; libusb
 * elsewhere) and start the handshake. */
QL_API ql_status ql_host_open(const ql_config* cfg, ql_host** out);
/* Same over a caller-provided transport (copied; close is called by ql_host_close). */
QL_API ql_status ql_host_open_transport(const ql_config* cfg, const ql_transport* transport, ql_host** out);
QL_API void ql_host_close(ql_host* host);

/* Read once (up to timeout_ms) and process everything received: handshake, clock sync,
 * poses, stream setup pacing. Call in a loop on one thread. Returns QL_OK, QL_ERR_TRANSPORT
 * or QL_ERR_FAILED. Other ql_host_* calls may come from other threads; frames from one. */
QL_API ql_status ql_host_poll(ql_host* host, int timeout_ms);

QL_API ql_state ql_host_get_state(ql_host* host);
QL_API const char* ql_state_name(ql_state state);
QL_API const char* ql_host_get_error(ql_host* host); /* "" if none; valid until the next call */
QL_API ql_status ql_host_get_info(ql_host* host, ql_info* out);
/* QL_ERR_NO_DATA until the HELLO arrived. */
QL_API ql_status ql_host_get_device_info(ql_host* host, ql_device_info* out);
/* Latest head pose; QL_ERR_NO_DATA until one arrived. */
QL_API ql_status ql_host_get_head_pose(ql_host* host, ql_head_pose* out);

/* Send one encoded frame: count slices (1..5) on topics 10.., in one USB transfer.
 * info may be NULL (next index, now + 20 ms, template pose, intra). Each slice's csd and
 * video must each fit one XRSP message (~120 KiB, the headset drops the session above
 * ~128 KiB); otherwise nothing is sent and QL_ERR_ARGUMENT is returned (see ql_host_get_error). */
QL_API ql_status ql_host_submit_frame_slices(ql_host* host, const ql_slice* slices, uint32_t count,
                                             const ql_frame_info* info);

/* The host's monotonic clock in ns (what clock offsets are relative to). */
QL_API int64_t ql_now_ns(void);

/* ======================= Audio =========================================================== */
/* Link carries raw PCM on topic 6 (PROTOCOL_NOTES "Audio"): game audio to the headset as
 * 48 kHz stereo float32 in 10 ms blocks, the headset microphone back as 48 kHz mono int16. */

#define QL_AUDIO_RATE 48000
#define QL_AUDIO_BLOCK_FRAMES 480 /* 10 ms */

/* Queue game audio for the headset (any thread, e.g. the engine's audio thread). Interleaved
 * L R float32 at sample_rate (resampled to 48 kHz). The runtime sends a 10 ms block every 10 ms
 * while streaming, silence when the queue runs dry; at most ~100 ms is kept (oldest dropped).
 * QL_ERR_NOT_STREAMING (audio dropped) until the headset session is up. QL_AUDIO=0 in the
 * environment turns the sender off. */
QL_API int ql_submit_audio(ql_session* session, const float* interleaved_stereo, uint32_t frames,
                           uint32_t sample_rate);
/* Headset microphone, 48 kHz mono int16, oldest first (the newest ~1 s is kept). Returns the
 * frames copied (0 if none) or a negative ql_status. */
QL_API int ql_read_mic(ql_session* session, int16_t* out, uint32_t max_frames);

/* Host API: send one block of QL_AUDIO_BLOCK_FRAMES stereo frames (48 kHz float32) now;
 * ts_ns = sender clock (ql_now_ns), 0 = now. The caller paces it every 10 ms. */
QL_API ql_status ql_host_send_audio(ql_host* host, const float* interleaved_stereo, int64_t ts_ns);
/* Host API: microphone samples received so far (as ql_read_mic). */
QL_API int ql_host_read_mic(ql_host* host, int16_t* out, uint32_t max_frames);

/* ======================= Hand tracking =================================================== */
/* Topic 8 Hands, enabled by ql_config.hands (InputControl on topic 28). The Quest sends ~60
 * messages/s; the joint data itself updates at ~25-30 Hz. Same raw Link convention as ql_pose.
 * Joints are XR_EXT_hand_tracking joints with OpenXR axes (-Z toward the fingertip, +Y out of
 * the back of the hand), computed from the 24 streamed bone rotations and the bind skeleton
 * (topic 9, or a built-in adult hand until it arrived). */

#define QL_HAND_JOINT_COUNT 26 /* XrHandJointEXT order: PALM, WRIST, THUMB_METACARPAL, ... */

/* ql_hand.flags bits */
#define QL_HAND_TRACKED (1u << 0)
#define QL_HAND_AIM_VALID (1u << 1)
#define QL_HAND_DOMINANT (1u << 2)
#define QL_HAND_HEADSET_SKELETON (1u << 3) /* bind skeleton came from the headset (topic 9) */

typedef struct ql_hand_joint {
    float qx, qy, qz, qw;
    float px, py, pz;
    float radius; /* metres */
} ql_hand_joint;

typedef struct ql_hand {
    uint32_t flags;        /* QL_HAND_* */
    uint32_t input_status; /* raw topic 8 input bits (0x02 aim valid, 0x80 dominant, ...) */
    float confidence;      /* 0..1 */
    float scale;           /* hand size relative to the bind skeleton */
    float pinch[4];        /* pinch strength 0..1: index, middle, ring, little */
    ql_pose root;          /* wrist in the OVR bone frame; time_ns = sample time (Quest clock) */
    ql_pose aim;           /* pointer pose (-Z = pointing direction) */
    ql_hand_joint joints[QL_HAND_JOINT_COUNT];
} ql_hand;

typedef struct ql_hands {
    ql_hand hands[2];      /* left, right; all zero (flags 0) when not tracked */
    uint64_t count;        /* topic 8 messages received so far */
} ql_hands;

/* Session API: newest hands; QL_ERR_NO_DATA until the first topic 8 message. */
QL_API int ql_get_hands(ql_session* session, ql_hands* out);
/* Host API: same. */
QL_API ql_status ql_host_get_hands(ql_host* host, ql_hands* out);
/* Host API: send InputControl hands on (1) or off (0) now; QL_ERR_NOT_STREAMING before setup. */
QL_API ql_status ql_host_set_hands(ql_host* host, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* QUESTLINK_H */
