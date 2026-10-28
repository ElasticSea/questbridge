/* questlink: Meta Link (XRSP over USB) host library.
 *
 * Speaks the Quest's built-in Link protocol: waits for the headset's HELLO, runs the
 * two-phase handshake (the user presses "Enable Link" in between), keeps the clock in sync,
 * streams H.264 slices and reports head pose.
 *
 * Threading: ql_poll() does all reading and protocol work; call it in a loop on one thread
 * (e.g. a dedicated I/O thread). ql_submit_frame_slices() may be called from one other
 * thread (e.g. the render or encoder thread). Getters may be called from any thread.
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
#else
#  define QL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QL_API_VERSION 1

typedef struct ql_host ql_host;

typedef enum ql_status {
    QL_OK = 0,
    QL_ERR_ARGUMENT = -1,
    QL_ERR_NO_DEVICE = -2,      /* Quest XRSP interface not found or busy (OVRService running?) */
    QL_ERR_TRANSPORT = -3,      /* USB I/O failed: unplugged or reset; close and reopen */
    QL_ERR_NOT_STREAMING = -4,  /* handshake / stream setup not finished yet */
    QL_ERR_NO_CLOCK = -5,       /* no Quest clock ping received yet */
    QL_ERR_FAILED = -6,         /* session failed (see ql_get_error) */
    QL_ERR_INTERNAL = -7
} ql_status;

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

/* Byte transport for ql_open_transport (tests, other backends). read returns bytes read,
 * 0 on timeout, < 0 on error; write returns 0 on success, < 0 on error. */
typedef struct ql_transport {
    void* user;
    int (*write)(void* user, const uint8_t* data, size_t len, int timeout_ms);
    int (*read)(void* user, uint8_t* buf, size_t cap, int timeout_ms);
    void (*close)(void* user); /* may be NULL */
} ql_transport;

typedef struct ql_pose {
    float orientation[4]; /* quaternion x, y, z, w */
    float position[3];    /* metres */
} ql_pose;

typedef struct ql_head_pose {
    ql_pose pose;
    float angular_velocity[3];
    float linear_velocity[3];
    float angular_acceleration[3];
    float linear_acceleration[3];
    uint64_t sample_time_ns; /* Quest clock */
    uint64_t vsync_ns;       /* Quest clock */
    uint64_t count;          /* poses received so far */
} ql_head_pose;

typedef struct ql_slice {
    const uint8_t* csd; /* Annex-B SPS+PPS for this slice, or NULL */
    size_t csd_len;
    const uint8_t* video; /* Annex-B slice NAL(s) */
    size_t video_len;
} ql_slice;

typedef struct ql_frame_info {
    uint64_t frame_index;          /* 0 = next */
    int64_t target_vsync_quest_ns; /* 0 = now + 20 ms in the Quest clock */
    int has_render_pose;           /* 0 = keep the template's identity pose */
    ql_pose render_pose;           /* the pose the frame was rendered with */
    int is_intra;
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
 * elsewhere) and start the session. */
QL_API ql_status ql_open(const ql_config* cfg, ql_host** out);
/* Same over a caller-provided transport (copied; close is called by ql_close). */
QL_API ql_status ql_open_transport(const ql_config* cfg, const ql_transport* transport, ql_host** out);
QL_API void ql_close(ql_host* host);

/* Read once (up to timeout_ms) and process everything received: handshake, clock sync,
 * poses, stream setup pacing. Returns QL_OK, QL_ERR_TRANSPORT or QL_ERR_FAILED. */
QL_API ql_status ql_poll(ql_host* host, int timeout_ms);

QL_API ql_state ql_get_state(ql_host* host);
QL_API const char* ql_state_name(ql_state state);
QL_API const char* ql_get_error(ql_host* host); /* "" if none; valid until the next call */
QL_API ql_status ql_get_info(ql_host* host, ql_info* out);
/* Latest head pose; returns 1 if one has arrived, else 0. */
QL_API int ql_get_head_pose(ql_host* host, ql_head_pose* out);

/* Send one video frame: count slices (1..5) on topics 10.., in one USB transfer.
 * info may be NULL (next index, now + 20 ms, template pose, intra). */
QL_API ql_status ql_submit_frame_slices(ql_host* host, const ql_slice* slices, uint32_t count,
                                        const ql_frame_info* info);

/* The host's monotonic clock in ns (what clock offsets are relative to). */
QL_API int64_t ql_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* QUESTLINK_H */
