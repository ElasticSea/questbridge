/* questbridge: Meta Link (XRSP over USB) host library.
 *
 * Speaks the Quest's built-in Link protocol: waits for the headset's HELLO, runs the
 * two-phase handshake (the user presses "Enable Link" in between), keeps the clock in sync,
 * streams H.264 slices and reports head pose.
 *
 * Two layers:
 *  - Session API (qb_open, qb_poll, qb_get_head_pose, qb_submit_frame_texture, ...): what the
 *    Unity package (unity/com.elasticsea.questbridge, Runtime/Native/QuestBridgeNative.cs) calls.
 *    One process-wide Link runtime does USB, handshake and encoding on background threads and
 *    outlives every qb_session, so the headset session survives Play-mode exits.
 *  - Host API (qb_host_*): one connection driven by the caller. Used by qbhost and the tests.
 *
 * Poses are raw Link convention: right-handed, +Y up, -Z forward, metres, Quest-clock ns.
 * Return values: 0 = success unless noted.
 */
#ifndef QUESTBRIDGE_H
#define QUESTBRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(QB_BUILD_SHARED)
#    define QB_API __declspec(dllexport)
#  else
#    define QB_API __declspec(dllimport)
#  endif
#  define QB_UNITY_CALL __stdcall
#else
#  define QB_API __attribute__((visibility("default")))
#  define QB_UNITY_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QB_API_VERSION 6 /* 4: recovery (qb_config, qb_info losses/resumes); 5: qb_info.encoder; 6: qb_device_info caps */

typedef enum qb_status {
    QB_OK = 0,
    QB_ERR_ARGUMENT = -1,
    QB_ERR_NO_DEVICE = -2,      /* Quest XRSP interface not found or busy (OVRService running?) */
    QB_ERR_TRANSPORT = -3,      /* USB I/O failed: unplugged or reset; close and reopen */
    QB_ERR_NOT_STREAMING = -4,  /* handshake / stream setup not finished yet */
    QB_ERR_NO_CLOCK = -5,       /* no Quest clock ping received yet */
    QB_ERR_FAILED = -6,         /* session failed (see qb_host_get_error) */
    QB_ERR_INTERNAL = -7,
    QB_ERR_NO_DATA = -8,        /* nothing received yet (pose, HELLO) */
    QB_ERR_UNSUPPORTED = -9     /* not decoded / not implemented on this platform yet */
} qb_status;

typedef struct qb_pose {
    float qx, qy, qz, qw; /* orientation quaternion */
    float px, py, pz;     /* position, metres */
    uint64_t time_ns;     /* Quest clock (sample time); ignored when submitting */
} qb_pose;

typedef struct qb_fov {
    float up, down, left, right; /* positive tangents of the half-angles (OVR FovPort order) */
} qb_fov;

/* From the headset's HELLO device description (docs/protocol/spec.md §8). */
#define QB_MAX_REFRESH_RATES 8

typedef struct qb_device_info {
    uint32_t encode_width, encode_height; /* video stream size (4096x2240 on Quest 3) */
    float refresh_hz;                     /* the panel's default rate */
    float ipd_m;
    qb_fov fov_left, fov_right;           /* each eye's FOV as the headset reports it */
    /* API version 3 */
    uint32_t panel_width, panel_height;   /* both eyes (4128x2208 on Quest 3) */
    uint32_t refresh_count;
    uint32_t refresh_rates[QB_MAX_REFRESH_RATES]; /* offered rates, Hz, ascending */
    uint32_t stream_refresh_hz;           /* the rate our CONFIG chose; 0 before phase 2 */
    uint32_t codecs;                      /* bit n: headset lists video codec n (qb_codec); 0 on Quest 3 */
    uint32_t hello_version;               /* 1 (phase 1) or 3 (Link session) */
    uint64_t features;                    /* bit n: headset offers session feature n (HELLO v3) */
    char product[32];                     /* "Meta Quest 3" */
    char manufacturer[32];                /* "Oculus VR" */
    char firmware[64];                    /* "releases-oculus-14.0-v207" */
    uint32_t stream_codec;                /* qb_codec our CONFIG chose (with stream_refresh_hz) */
    /* API version 6 */
    uint32_t max_slice_bytes;   /* largest slice NAL / csd we send this headset (Quest 3 120,000; others 100,000) */
    uint32_t max_message_bytes; /* largest XRSP message on any topic (Quest 3 120 KiB; others 100 KiB) */
    uint32_t default_bitrate;   /* bit/s at stream_refresh_hz: Quest 3 100M; others 70% of the HELLO mode's, at most 100M */
} qb_device_info;

/* qb_controller.buttons / touches bits */
#define QB_BUTTON_PRIMARY (1u << 0)    /* A / X */
#define QB_BUTTON_SECONDARY (1u << 1)  /* B / Y */
#define QB_BUTTON_THUMBSTICK (1u << 2)
#define QB_BUTTON_MENU (1u << 3)       /* left controller only (the right one's is the system button) */
#define QB_TOUCH_TRIGGER (1u << 4)     /* touches only */
#define QB_TOUCH_THUMBREST (1u << 5)   /* touches only */
/* qb_controller.flags bits */
#define QB_CONTROLLER_CONNECTED (1u << 0)
#define QB_CONTROLLER_POSITION_TRACKED (1u << 1)
#define QB_CONTROLLER_ROTATION_TRACKED (1u << 2)

/* Touch controller from topics 34 (pose) and 35 (input). Not connected (flags 0) while the
 * controllers are put down and hands are tracked instead. Same raw Link convention as qb_pose. */
typedef struct qb_controller {
    qb_pose pose; /* grip pose; time_ns = sample time */
    uint32_t buttons, touches;
    float trigger, grip;
    float stick_x, stick_y;
    uint32_t flags;
} qb_controller;

/* ======================= Session API (Unity) ============================================= */

typedef struct qb_session qb_session;

/* Payload of render event QB_EVENT_SUBMIT_FRAME; must stay valid until the event has run. */
typedef struct qb_frame_submit {
    qb_session* session;
    void* native_texture; /* ID3D11Texture2D* on D3D11, id<MTLTexture> on Metal */
    qb_pose render_pose;  /* the exact pose the frame was rendered with */
    uint64_t frame_index; /* caller's counter; may restart per qb_session */
} qb_frame_submit;

#define QB_EVENT_SUBMIT_FRAME 1

/* Non-blocking; starts the Link runtime on first use (it then waits for the Quest, does the
 * handshake and keeps the headset session up for later sessions). NULL only on failure. */
QB_API qb_session* qb_open(void);
/* Ends this handle; the runtime and headset session stay up. Submit events for the handle
 * still queued on the render thread are ignored. */
QB_API void qb_close(qb_session* session);
/* Stop sending (frames, audio, clock replies) and close USB, leaving the headset's Link session
 * up for another host: logs "resume with XRSP_FRAME_START=... XRSP_CONTROL_SEQ=...
 * XRSP_VIDEO_SEQ=... XRSP_STREAM_SEQ=..." (tools/pyhost/handshake.py XRSP_ATTACH) to QB_LOG_FILE or
 * stderr. Final for this process. */
QB_API void qb_shutdown(void);
/* 0 idle (no headset), 1 connecting (USB open, handshake or setup running), 2 streaming. */
QB_API int qb_poll(qb_session* session);

/* API version 4: recovery. The runtime recovers from every disconnect by itself (USB error or
 * unplug, Link quit, headset BYE, a new HELLO, silence): it reopens USB and either resumes the
 * stream with the same numbering (USB back within QB_RESUME_MS, default 500, and the headset
 * still streaming) or runs a new handshake; the user then presses Launch again. */
typedef enum qb_link_state {
    QB_LINK_NO_DEVICE = 0,   /* XRSP interface not open: unplugged, asleep, Link quit, or held by another program */
    QB_LINK_CONNECTING = 1,  /* handshake or stream setup running */
    QB_LINK_WAIT_LAUNCH = 2, /* phase 1 done: the user presses Launch in the headset */
    QB_LINK_STREAMING = 3,
    QB_LINK_RESUMING = 4     /* USB reopened right after an error; continuing if the headset still streams */
} qb_link_state;

typedef enum qb_loss {
    QB_LOSS_NONE = 0,
    QB_LOSS_USB = 1,         /* USB read/write failed: cable, Link quit, headset reboot */
    QB_LOSS_HEADSET_BYE = 2, /* the headset ended the stream */
    QB_LOSS_NEW_HELLO = 3,   /* the headset restarted the handshake */
    QB_LOSS_SILENCE = 4      /* nothing from the headset for QB_SILENCE_MS (default 3000) */
} qb_loss;

typedef struct qb_link_status {
    int32_t state;          /* qb_link_state */
    int32_t last_loss;      /* qb_loss of the latest disconnect */
    uint32_t connects;      /* times streaming started or resumed */
    uint32_t disconnects;   /* streams lost; reconnecting = disconnects > 0 and state != STREAMING */
    uint32_t resumes;       /* streams that continued after a USB error without a new handshake */
    uint32_t reserved;
    int64_t state_since_ns; /* qb_now_ns() when state last changed */
    char encoder[160];      /* API version 5: video encoder backend, as qb_info.encoder */
} qb_link_status;

/* Runtime-wide link status (the same for every session). */
QB_API int qb_get_link_status(qb_session* session, qb_link_status* out);
/* Nonzero until the HELLO arrived. */
QB_API int qb_get_device_info(qb_session* session, qb_device_info* out);
/* Newest head pose; nonzero until one arrived. */
QB_API int qb_get_head_pose(qb_session* session, qb_pose* out);
/* hand 0 = left, 1 = right. Newest controller state; QB_ERR_NO_DATA (flags 0) until the
 * first topic 34/35 message. */
QB_API int qb_get_controller(qb_session* session, int hand, qb_controller* out);
/* Vibrate controller hand: amplitude 0..1 for duration_s seconds (RuntimeIPC HapticsServer call,
 * as Meta's PC sends for xrApplyHapticFeedback). QB_ERR_NOT_STREAMING before the stream setup. */
QB_API int qb_haptic(qb_session* session, int hand, float amplitude, float duration_s);
/* Copy native_texture now (call on the graphics/render thread), encode and send it
 * asynchronously. Frames whose frame_index does not increase for this session are dropped;
 * the headset's frame.index is a separate session-wide counter. */
QB_API int qb_submit_frame_texture(qb_session* session, void* native_texture, const qb_pose* render_pose,
                                   uint64_t frame_index);
/* UnityRenderingEventAndData: event QB_EVENT_SUBMIT_FRAME, data = qb_frame_submit*. */
typedef void(QB_UNITY_CALL* qb_render_event_fn)(int event_id, void* data);
QB_API qb_render_event_fn qb_get_render_event_func(void);

/* ======================= Host API (one connection) ======================================= */

typedef struct qb_host qb_host;

typedef enum qb_state {
    QB_STATE_WAIT_HELLO = 0,
    QB_STATE_PHASE1 = 1,
    QB_STATE_WAIT_LINK_REQUEST = 2, /* waiting for the user to press "Enable Link" */
    QB_STATE_WAIT_HELLO_V3 = 3,
    QB_STATE_PHASE2 = 4,
    QB_STATE_SETUP = 5,             /* sending stream setup */
    QB_STATE_STREAMING = 6,         /* frames can be submitted */
    QB_STATE_FAILED = 7
} qb_state;

/* Log line from the protocol: direction "PC->Q", "Q->PC" or "info"; seq -1 = none. */
typedef void (*qb_log_fn)(void* user, const char* direction, const char* what, int seq);

/* Video codec (CONFIG v3 u16 @ byte 6; 1 = HEVC is inferred from Meta's host) */
typedef enum qb_codec {
    QB_CODEC_AUTO = -1, /* HEVC where this platform encodes it faster (macOS), unless the headset
                         * lists codecs without HEVC; else H.264. Quest 3 / Pro list nothing
                         * (the Pro decodes HEVC): HEVC on macOS, H.264 on Windows */
    QB_CODEC_H264 = 0,
    QB_CODEC_HEVC = 1
} qb_codec;

typedef struct qb_config {
    /* Attach to a headset session an earlier host left running (no handshake, no setup);
     * continue its numbering. */
    int attach;
    uint16_t attach_control_seq; /* default 16 */
    uint16_t attach_video_seq;   /* default 0 */
    uint64_t first_frame_index;  /* default 1; frame.index must increase across the session */
    int send_setup;              /* send Meta's recorded stream setup after the handshake (default 1) */
    int hands;                   /* turn hand tracking on once streaming (InputControl, default 1) */
    int haptics;                 /* connect the RuntimeIPC HapticsServer once streaming (default 1) */
    int hello_timeout_ms;        /* 0 = wait forever (default) */
    int link_request_timeout_ms; /* waiting for "Enable Link"; 0 = forever (default) */
    int step_timeout_ms;         /* per handshake step; a stalled step restarts pairing (default 3000) */
    qb_log_fn log;
    void* log_user;
    /* While waiting for HELLO, send a BYE so a headset whose earlier HELLO went unanswered
     * sends a new one; repeat every bye_retry_ms (default 3000; 0 = once at open, < 0 = never). */
    int bye_retry_ms;
    uint32_t refresh_hz;         /* Link session rate; 0 = the headset's default (default 0) */
    int codec;                   /* qb_codec for CONFIG and the encoder (default QB_CODEC_AUTO) */
    /* API version 4 */
    int silence_timeout_ms;      /* setup/streaming: no Quest message this long ends the stream (default 3000; 0 = never) */
    int resume_timeout_ms;       /* qb_host_reopen: no Quest message this long means the stream is gone (default 1000) */
} qb_config;

/* Byte transport for qb_host_open_transport (tests, other backends). read returns bytes
 * read, 0 on timeout, < 0 on error; write returns 0 on success, < 0 on error. */
typedef struct qb_transport {
    void* user;
    int (*write)(void* user, const uint8_t* data, size_t len, int timeout_ms);
    int (*read)(void* user, uint8_t* buf, size_t cap, int timeout_ms);
    void (*close)(void* user); /* may be NULL */
} qb_transport;

typedef struct qb_head_pose {
    qb_pose pose; /* time_ns = sample time */
    float angular_velocity[3];
    float linear_velocity[3];
    float angular_acceleration[3];
    float linear_acceleration[3];
    uint64_t vsync_ns; /* Quest clock */
    uint64_t count;    /* poses received so far */
} qb_head_pose;

typedef struct qb_slice {
    const uint8_t* csd; /* Annex-B SPS+PPS for this slice, or NULL */
    size_t csd_len;
    const uint8_t* video; /* Annex-B slice NAL(s) */
    size_t video_len;
} qb_slice;

typedef struct qb_frame_info {
    uint64_t frame_index;          /* 0 = next (session-wide counter) */
    int64_t target_vsync_quest_ns; /* 0 = now + 20 ms in the Quest clock */
    int has_render_pose;           /* 0 = keep the template's identity pose */
    qb_pose render_pose;           /* the pose the frame was rendered with */
    int is_intra;
    /* Timewarp: the headset reprojects to render_pose only with rectify_mesh_id 1001 and the
     * real per-eye FOV (Meta's game frames). 0 / has_fov 0 keep the template's mesh 0 and
     * zero FOV (Meta's loading screen: shown head-locked, no reprojection). */
    uint32_t rectify_mesh_id;
    int has_fov;
    qb_fov fov_left, fov_right; /* the projections the eyes were rendered with */
    /* Picture height the slices split into equal macroblock-row strips; 0 = the HELLO's
     * encode height (or the recorded 2240 before a HELLO) */
    uint32_t picture_height;
} qb_frame_info;

typedef struct qb_info {
    qb_state state;
    uint64_t session_id;
    int has_clock_offset;
    int64_t quest_clock_offset_ns; /* Quest clock = qb_now_ns() + offset */
    uint64_t next_frame_index;
    uint16_t control_seq;
    uint16_t video_seq; /* next seq on topic 10 */
    uint64_t rx_counts[64]; /* messages received per topic */
    uint16_t tx_seq[64];    /* next seq per PC->Q stream topic, 0 = not used yet */
    uint32_t streams;       /* streaming sessions started on this host (a new one needs an IDR) */
    uint32_t byes_sent;     /* BYEs sent while waiting for HELLO */
    /* API version 4 */
    uint32_t losses;        /* streams ended by the headset (BYE, new HELLO) or by silence */
    int32_t last_loss;      /* qb_loss */
    uint32_t resumes;       /* qb_host_reopen()s the headset confirmed */
    int32_t resuming;       /* 1 after qb_host_reopen until the headset's first message */
    /* API version 5. Video encoder backend of qb_submit_frame_texture, e.g. "mf-nvidia: NVIDIA
     * H.264 Encoder MFT (hardware), ..." or "openh264: ..."; "" until the first frame was encoded */
    char encoder[160];
} qb_info;

QB_API void qb_config_init(qb_config* cfg);

/* Open the Quest's XRSP USB interface (WinUSB on Windows, so adb keeps working; libusb
 * elsewhere) and start the handshake. */
QB_API qb_status qb_host_open(const qb_config* cfg, qb_host** out);
/* Same over a caller-provided transport (copied; close is called by qb_host_close). */
QB_API qb_status qb_host_open_transport(const qb_config* cfg, const qb_transport* transport, qb_host** out);
QB_API void qb_host_close(qb_host* host);
/* After QB_ERR_TRANSPORT: close the broken transport, open a new one (transport NULL = USB) and
 * continue the same session. A stream keeps its frame index and per-topic seq while the headset
 * still has it (any message within resume_timeout_ms; qb_info.resuming until then), otherwise
 * the host sends a BYE and starts over. QB_ERR_NO_DEVICE: not back yet, call again. */
QB_API qb_status qb_host_reopen(qb_host* host, const qb_transport* transport);

/* Read once (up to timeout_ms) and process everything received: handshake, clock sync,
 * poses, stream setup pacing. Call in a loop on one thread. Returns QB_OK, QB_ERR_TRANSPORT
 * or QB_ERR_FAILED. Other qb_host_* calls may come from other threads; frames from one. */
QB_API qb_status qb_host_poll(qb_host* host, int timeout_ms);

QB_API qb_state qb_host_get_state(qb_host* host);
QB_API const char* qb_state_name(qb_state state);
QB_API const char* qb_host_get_error(qb_host* host); /* "" if none; valid until the next call */
QB_API qb_status qb_host_get_info(qb_host* host, qb_info* out);
/* QB_ERR_NO_DATA until the HELLO arrived. */
QB_API qb_status qb_host_get_device_info(qb_host* host, qb_device_info* out);
/* Latest head pose; QB_ERR_NO_DATA until one arrived. */
QB_API qb_status qb_host_get_head_pose(qb_host* host, qb_head_pose* out);

/* Send one encoded frame: count slices (1..5) on topics 10.., in one USB transfer.
 * info may be NULL (next index, now + 20 ms, template pose, intra). Each slice's csd and
 * video must each fit one XRSP message (~120 KiB, the headset drops the session above
 * ~128 KiB); otherwise nothing is sent and QB_ERR_ARGUMENT is returned (see qb_host_get_error). */
QB_API qb_status qb_host_submit_frame_slices(qb_host* host, const qb_slice* slices, uint32_t count,
                                             const qb_frame_info* info);

/* The host's monotonic clock in ns (what clock offsets are relative to). */
QB_API int64_t qb_now_ns(void);

/* ======================= Audio =========================================================== */
/* Link carries raw PCM on topic 6 (docs/protocol/spec.md): game audio to the headset as
 * 48 kHz stereo float32 in 10 ms blocks, the headset microphone back as 48 kHz mono int16. */

#define QB_AUDIO_RATE 48000
#define QB_AUDIO_BLOCK_FRAMES 480 /* 10 ms */

/* Queue game audio for the headset (any thread, e.g. the engine's audio thread). Interleaved
 * L R float32 at sample_rate (resampled to 48 kHz). The runtime sends a 10 ms block every 10 ms
 * while streaming, silence when the queue runs dry; at most ~100 ms is kept (oldest dropped).
 * QB_ERR_NOT_STREAMING (audio dropped) until the headset session is up. QB_AUDIO=0 in the
 * environment turns the sender off. */
QB_API int qb_submit_audio(qb_session* session, const float* interleaved_stereo, uint32_t frames,
                           uint32_t sample_rate);
/* Headset microphone, 48 kHz mono int16, oldest first (the newest ~1 s is kept). Returns the
 * frames copied (0 if none) or a negative qb_status. */
QB_API int qb_read_mic(qb_session* session, int16_t* out, uint32_t max_frames);

/* Host API: send one block of QB_AUDIO_BLOCK_FRAMES stereo frames (48 kHz float32) now;
 * ts_ns = sender clock (qb_now_ns), 0 = now. The caller paces it every 10 ms. */
QB_API qb_status qb_host_send_audio(qb_host* host, const float* interleaved_stereo, int64_t ts_ns);
/* Host API: microphone samples received so far (as qb_read_mic). */
QB_API int qb_host_read_mic(qb_host* host, int16_t* out, uint32_t max_frames);

/* ======================= Hand tracking =================================================== */
/* Topic 8 Hands, enabled by qb_config.hands (InputControl on topic 28). The Quest sends ~60
 * messages/s; the joint data itself updates at ~25-30 Hz. Same raw Link convention as qb_pose.
 * Joints are XR_EXT_hand_tracking joints with OpenXR axes (-Z toward the fingertip, +Y out of
 * the back of the hand), computed from the 24 streamed bone rotations and the bind skeleton
 * (topic 9, or a built-in adult hand until it arrived). */

#define QB_HAND_JOINT_COUNT 26 /* XrHandJointEXT order: PALM, WRIST, THUMB_METACARPAL, ... */

/* qb_hand.flags bits */
#define QB_HAND_TRACKED (1u << 0)
#define QB_HAND_AIM_VALID (1u << 1)
#define QB_HAND_DOMINANT (1u << 2)
#define QB_HAND_HEADSET_SKELETON (1u << 3) /* bind skeleton came from the headset (topic 9) */

typedef struct qb_hand_joint {
    float qx, qy, qz, qw;
    float px, py, pz;
    float radius; /* metres */
} qb_hand_joint;

typedef struct qb_hand {
    uint32_t flags;        /* QB_HAND_* */
    uint32_t input_status; /* raw topic 8 input bits (0x02 aim valid, 0x80 dominant, ...) */
    float confidence;      /* 0..1 */
    float scale;           /* hand size relative to the bind skeleton */
    float pinch[4];        /* pinch strength 0..1: index, middle, ring, little */
    qb_pose root;          /* wrist in the OVR bone frame; time_ns = sample time (Quest clock) */
    qb_pose aim;           /* pointer pose (-Z = pointing direction) */
    qb_hand_joint joints[QB_HAND_JOINT_COUNT];
} qb_hand;

typedef struct qb_hands {
    qb_hand hands[2];      /* left, right; all zero (flags 0) when not tracked */
    uint64_t count;        /* topic 8 messages received so far */
} qb_hands;

/* Session API: newest hands; QB_ERR_NO_DATA until the first topic 8 message. */
QB_API int qb_get_hands(qb_session* session, qb_hands* out);
/* Host API: same. */
QB_API qb_status qb_host_get_hands(qb_host* host, qb_hands* out);
/* Host API: send InputControl hands on (1) or off (0) now; QB_ERR_NOT_STREAMING before setup. */
QB_API qb_status qb_host_set_hands(qb_host* host, int enabled);

/* ======================= Controllers ===================================================== */

/* Host API: as qb_get_controller. */
QB_API qb_status qb_host_get_controller(qb_host* host, int hand, qb_controller* out);
/* Host API: as qb_haptic. */
QB_API qb_status qb_host_haptic(qb_host* host, int hand, float amplitude, float duration_s);

/* ======================= Setup check ===================================================== */
/* First-run diagnostic behind Unity's QuestBridge > Check Setup and `qbhost --check`. Checks, in
 * order (id): "os", "link_app" (Windows: Meta Horizon Link's USB driver), "quest_usb",
 * "xrsp_interface", "ovr_service" (Windows), "xrsp_claim" (opened and released again unless
 * QB_CHECK_NO_CLAIM or QB_USB=0; never prompts), "gpu_encoder" (opens a hardware H.264 encoder once).
 * Text is ASCII; fix is empty when nothing needs doing.
 *
 * OVRService (Windows) holds the Quest's Link interface while it runs. When opening the Quest
 * fails because of it, the library starts one elevated helper (one UAC prompt per process)
 * that stops OVRService and starts it again when the process exits. QB_OVR_STOP=0 in the
 * environment turns that off. */

typedef enum qb_check_status {
    QB_CHECK_OK = 0,
    QB_CHECK_WARN = 1, /* works, but read the fix */
    QB_CHECK_FAIL = 2,
    QB_CHECK_SKIP = 3  /* not applicable here, or needs an earlier check to pass */
} qb_check_status;

#define QB_CHECK_NO_CLAIM 1u /* qb_check_setup flags: don't open the Link interface */
#define QB_MAX_CHECKS 16

typedef struct qb_check {
    int32_t status; /* qb_check_status */
    char id[32];
    char title[64];
    char detail[256]; /* what was found */
    char fix[512];    /* what to do */
} qb_check;

/* Runs every check (up to ~1 s, mostly the encoder). Writes min(cap, n) checks and returns n. */
QB_API int qb_check_setup(uint32_t flags, qb_check* out, int cap);
/* Windows: stop OVRService now through the helper (one UAC prompt; it starts again when this
 * process exits). 1 = prompt shown, 0 = OVRService isn't running, QB_ERR_UNSUPPORTED elsewhere. */
QB_API int qb_stop_ovr_service(void);

#ifdef __cplusplus
}
#endif

#endif /* QUESTBRIDGE_H */
