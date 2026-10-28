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

#define QL_API_VERSION 2

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

typedef struct ql_device_info {
    uint32_t encode_width, encode_height; /* video stream size (4096x2240 on Quest 3) */
    float refresh_hz;
    float ipd_m;
    ql_fov fov_left, fov_right;
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
    int hello_timeout_ms;        /* 0 = wait forever (default) */
    int link_request_timeout_ms; /* waiting for "Enable Link"; 0 = forever (default) */
    int step_timeout_ms;         /* per handshake step (default 3000) */
    ql_log_fn log;
    void* log_user;
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

#ifdef __cplusplus
}
#endif

#endif /* QUESTLINK_H */
