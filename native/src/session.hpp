// Link host session, sans-I/O (port of host/handshake.py).
//
// Feed it every Quest->PC message and call tick() regularly; it queues the PC->Quest
// transfers to write. Not thread-safe: the C API wraps it in a mutex.
//
//   WaitHello --HELLO v1--> Phase1 (CONFIG, CONFIRM, START) --> WaitLinkRequest
//   --BYE 0x85 (user pressed Enable Link)--> WaitHelloV3 --HELLO v3--> Phase2 (same, version 3)
//   --> clock ping --> Setup (recorded stream setup, paced) --> Streaming
//
// Recovery (PROTOCOL.md §4.6): in WaitHello we send a "preluding BYE" like Meta's host, so a
// headset whose INVITE (HELLO) went unanswered restarts and sends a fresh one; a HELLO in any
// state restarts the handshake; a BYE from the headset mid-session returns to WaitHello.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "audio.hpp"
#include "capnp.hpp"
#include "frame.hpp"
#include "hands.hpp"
#include "pose.hpp"
#include "xrsp.hpp"

namespace ql {

namespace topic {
constexpr uint8_t kControl = 1;
constexpr uint8_t kAudio = audio::kTopic;
constexpr uint8_t kHands = 8;
constexpr uint8_t kSkeleton = 9;
constexpr uint8_t kVideoFirst = 10;  // slice i on topic 10 + i
constexpr uint8_t kInputControl = 28;
constexpr uint8_t kHeadPose = 34;
}  // namespace topic

namespace control {
// Headset names (xrspd): PAIRING_ACK, INVITE, INVITE_OK, INVITE_ACK, BYE, PAIRING,
// CODE_GENERATION, CODE_GENERATION_ACK. The headset ends the phase-1 session with BYE when the
// user presses Enable Link (our "link request").
constexpr uint8_t kReady = 0x80, kHello = 0x81, kConfig = 0x82, kAck = 0x83, kBye = 0x85, kLinkRequest = kBye,
                  kStart = 0x87, kConfirm = 0x89, kConfirmAck = 0x8A;
constexpr uint8_t kClockPing = 0x06, kClockPong = 0x16, kClockFlags = 0x80;
}  // namespace control

enum class State {
    WaitHello,
    Phase1,
    WaitLinkRequest,
    WaitHelloV3,
    Phase2,
    Setup,
    Streaming,
    Failed,
};
const char* state_name(State s);

// From the HELLO device description (PROTOCOL_NOTES "Device description")
struct DeviceInfo {
    uint32_t panel_width = 0, panel_height = 0;
    uint32_t encode_width = 0, encode_height = 0;
    float refresh_hz = 0;
    float ipd_m = 0;
    float fov_deg[4] = {};  // left eye up, down, left, right; the right eye mirrors left/right
};
std::optional<DeviceInfo> parse_device_info(const capnp::Ptr& hello);

struct SessionConfig {
    // Attach to a headset session an earlier host left running: skip handshake and setup,
    // continue control seq, per-topic video seq and frame index (XRSP_ATTACH in handshake.py).
    bool attach = false;
    uint16_t attach_control_seq = 16;
    uint16_t attach_video_seq = 0;
    uint64_t first_frame_index = 1;
    bool send_setup = true;
    bool hands = true;  // send InputControl hands-on once streaming (Meta's setup turns them off)
    int64_t hello_timeout_ns = 0;  // 0 = wait forever
    int64_t step_timeout_ns = 3'000'000'000;
    int64_t link_request_timeout_ns = 0;  // waiting for the user to press Enable Link
    int64_t hello_v3_timeout_ns = 10'000'000'000;
    int64_t setup_max_gap_ns = 500'000'000;
    // WaitHello: preluding BYE when no HELLO arrived this long after start (Meta: 50 ms), then
    // again every bye_retry_ns while none comes (0 = once). bye_delay_ns < 0 = never.
    int64_t bye_delay_ns = 50'000'000;
    int64_t bye_retry_ns = 3'000'000'000;
};

class Session {
public:
    using Clock = std::function<int64_t()>;  // monotonic ns (QPC on Windows)
    using Log = std::function<void(const char* dir, const std::string& what, int seq)>;  // seq -1 = none

    Session(SessionConfig cfg, Clock clock, Log log = {});

    void start();
    void on_message(std::span<const uint8_t> raw);
    void tick();

    // Transfers to write, each a list of raw messages (pack with pack_transfer)
    std::vector<std::vector<Bytes>> take_outgoing();

    State state() const { return state_; }
    const std::string& error() const { return error_; }
    uint64_t session_id() const { return session_id_; }
    uint32_t streams() const { return streams_; }       // times Streaming was entered
    uint32_t byes_sent() const { return byes_sent_; }   // preluding BYEs
    uint16_t control_seq() const { return control_seq_; }
    std::optional<int64_t> quest_clock_offset_ns() const { return quest_offset_; }
    const std::map<uint8_t, uint64_t>& rx_counts() const { return rx_counts_; }
    const std::optional<HeadPose>& head_pose() const { return head_pose_; }  // latest
    const std::optional<DeviceInfo>& device_info() const { return device_info_; }
    uint64_t head_pose_count() const { return head_pose_count_; }
    const std::optional<HandsFrame>& hands() const { return hands_; }  // latest topic 8
    uint64_t hands_count() const { return hands_count_; }
    // Bind skeleton from topic 9, or nullopt (use default_skeleton)
    const std::optional<HandSkeleton>& skeleton(int hand) const { return skeletons_[hand ? 1 : 0]; }

    // Topic 28 InputControl: turn hand tracking (topics 8/9) on or off
    void set_hands(bool on);

    // Raw stream send: numbers seq per topic, all messages in one transfer
    void send_stream(std::vector<StreamMessage> msgs);
    uint16_t stream_seq(uint8_t topic) const;

    // One video frame as a single transfer. Needs the Streaming state and a clock offset
    // (at least one Quest clock ping), like the replay loop in handshake.py.
    enum class Submit { Ok, NotStreaming, NoClock, BadSlices, TooLarge };
    struct FrameOptions {
        uint64_t index = 0;                 // 0 = next index (session-wide, increasing)
        int64_t target_vsync_quest_ns = 0;  // 0 = now (Quest clock) + vsync_lead_ns
        int64_t vsync_lead_ns = 20'000'000;
        std::optional<Pose> render_pose;
        std::optional<uint32_t> rectify_mesh_id;
        std::optional<std::array<FovPort, 2>> fov;
        bool intra = true;
    };
    // TooLarge: a slice's csd or video exceeds kMaxStreamMessage; nothing is sent.
    Submit submit_frame(std::span<const SliceData> slices, const FrameOptions& opt);
    uint64_t next_frame_index() const { return next_frame_index_; }

    // One 10 ms block of game audio (audio::kBlockFrames stereo frames), stamped with our
    // clock (ts 0 = now). NotStreaming before the stream setup is done.
    Submit submit_audio(std::span<const float> stereo, int64_t ts_ns = 0);
    // Quest microphone, 48 kHz mono int16, oldest first
    size_t read_mic(int16_t* out, size_t max_frames) { return mic_.read(out, max_frames); }
    uint64_t mic_blocks() const { return mic_blocks_; }

private:
    void send_control(uint8_t type, uint16_t version);
    void send_clock(uint8_t type, const std::array<uint64_t, 4>& stamps);
    void send_raw_control(Bytes body, const std::string& label);
    void on_control(uint8_t type, const capnp::Ptr& tree, int seq);
    void on_clock(uint8_t type, std::span<const uint8_t> body);
    void send_bye();
    void wait_hello(int64_t bye_in_ns);
    void begin_phase(uint16_t version);
    void advance_phase(uint8_t type);
    void finish_handshake();
    void enter(State s, int64_t timeout_ns);
    void on_streaming();
    void fail(const std::string& why);
    void log(const char* dir, const std::string& what, int seq = -1) const;

    SessionConfig cfg_;
    Clock clock_;
    Log log_;
    State state_ = State::WaitHello;
    std::string error_;
    int64_t deadline_ = 0;  // 0 = none
    uint8_t expected_ = 0;  // control type awaited in a phase
    uint16_t version_ = 0;
    uint64_t session_id_ = 0;
    uint16_t control_seq_ = 0;
    uint32_t clock_count_ = 0;
    int64_t bye_due_ = 0;  // 0 = none
    uint32_t byes_sent_ = 0;
    uint32_t streams_ = 0;
    std::optional<int64_t> quest_offset_;
    std::optional<HeadPose> head_pose_;
    std::optional<DeviceInfo> device_info_;
    uint64_t head_pose_count_ = 0;
    std::optional<HandsFrame> hands_;
    uint64_t hands_count_ = 0;
    std::optional<HandSkeleton> skeletons_[2];
    std::map<uint8_t, uint64_t> rx_counts_;
    std::map<uint8_t, uint32_t> stream_seq_;
    uint64_t next_frame_index_ = 1;
    size_t setup_next_ = 0;
    int64_t setup_due_ = 0;
    std::vector<std::vector<Bytes>> outbox_;
    audio::MicBuffer mic_;
    uint64_t mic_blocks_ = 0;
};

}  // namespace ql
