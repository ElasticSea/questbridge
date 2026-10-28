#include "session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "templates.hpp"

namespace ql {

namespace {

std::string hex2(uint8_t v) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%02x", v);
    return b;
}

const templates::Control& control_template(uint8_t type, uint16_t version) {
    for (const auto& t : templates::controls())
        if (t.type == type && t.version == version) return t;
    throw std::runtime_error("no PC template for " + hex2(type) + " v" + std::to_string(version));
}

}  // namespace

std::optional<DeviceInfo> parse_device_info(const capnp::Ptr& hello) {
    // HELLO root p3 -> p0: display struct; its p3: eye FOV in degrees
    const capnp::Node* desc = hello ? hello->child(3) : nullptr;
    const capnp::Node* d = desc ? desc->child(0) : nullptr;
    if (!d || d->data.size() < 6) return std::nullopt;
    auto f32 = [](const capnp::Node& n, size_t word, int half) {
        uint32_t u = n.u32(word, half);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    DeviceInfo info;
    info.panel_width = d->u32(0, 1);
    info.panel_height = d->u32(1, 0);
    info.refresh_hz = f32(*d, 1, 1);
    info.ipd_m = f32(*d, 2, 0);
    info.encode_width = d->u32(5, 0);
    info.encode_height = d->u32(5, 1);
    if (const capnp::Node* fov = d->child(3); fov && fov->data.size() >= 2)
        for (int i = 0; i < 4; ++i) info.fov_deg[i] = f32(*fov, size_t(i / 2), i % 2);
    return info;
}

const char* state_name(State s) {
    switch (s) {
        case State::WaitHello: return "wait_hello";
        case State::Phase1: return "phase1";
        case State::WaitLinkRequest: return "wait_link_request";
        case State::WaitHelloV3: return "wait_hello_v3";
        case State::Phase2: return "phase2";
        case State::Setup: return "setup";
        case State::Streaming: return "streaming";
        case State::Failed: return "failed";
    }
    return "?";
}

Session::Session(SessionConfig cfg, Clock clock, Log log)
    : cfg_(cfg), clock_(std::move(clock)), log_(std::move(log)), next_frame_index_(cfg.first_frame_index) {}

void Session::start() {
    if (cfg_.attach) {
        // Headset session is still up from an earlier run: continue its numbering
        control_seq_ = cfg_.attach_control_seq;
        for (int i = 0; i < 5; ++i) stream_seq_[uint8_t(topic::kVideoFirst + i)] = cfg_.attach_video_seq;
        log("info", "attaching to existing session");
        enter(State::Streaming, 0);  // the earlier host already sent InputControl
        return;
    }
    wait_hello(cfg_.bye_delay_ns);
}

void Session::enter(State s, int64_t timeout_ns) {
    state_ = s;
    deadline_ = timeout_ns > 0 ? clock_() + timeout_ns : 0;
    bye_due_ = 0;
}

// The headset sends HELLO (INVITE) once per pairing attempt; if nobody answered it, it waits
// forever ("Attempting to connect to PC"). A BYE with session id 0 (wildcard) makes it give up
// and send a fresh one, unless it sent its INVITE < 1 s ago (then that INVITE reaches us).
void Session::wait_hello(int64_t bye_in_ns) {
    enter(State::WaitHello, cfg_.hello_timeout_ns);
    if (cfg_.bye_delay_ns >= 0 && bye_in_ns >= 0) bye_due_ = clock_() + bye_in_ns;
}

void Session::send_bye() {
    // Meta's preluding BYE: an all-zero Bye root (2 data words: version, session id)
    Bytes body = {control::kBye, 0x8c, 2, 0, 1, 0, 0, 0,  // inner header
                  0, 0, 0, 0, 3, 0, 0, 0,                 // segment table: 1 segment, 3 words
                  0, 0, 0, 0, 2, 0, 0, 0};                // root pointer: struct, 2 data words
    body.resize(body.size() + 16, 0);
    ++byes_sent_;
    send_raw_control(std::move(body), "BYE (no HELLO yet)");
}

void Session::fail(const std::string& why) {
    error_ = why;
    state_ = State::Failed;
    deadline_ = 0;
    log("info", "failed: " + why);
}

void Session::log(const char* dir, const std::string& what, int seq) const {
    if (log_) log_(dir, what, seq);
}

std::vector<std::vector<Bytes>> Session::take_outgoing() { return std::exchange(outbox_, {}); }

// --- sending ---------------------------------------------------------------

void Session::send_control(uint8_t type, uint16_t version) {
    const auto& t = control_template(type, version);
    uint32_t words = 0;
    capnp::Ptr tree = capnp::decode(t.capnp, &words);
    if (version == 1) {
        // Phase 1 messages echo the session id from the Quest's HELLO
        if (type == control::kConfig) tree->data.at(10) = session_id_;
        else tree->data.at(1) = session_id_;
    }
    Bytes capnp = capnp::encode(tree, words);  // recorded offsets: Meta's allocation order
    Bytes body(8);
    body[0] = type;
    body[1] = t.inner_flags;
    wr16(&body[2], uint16_t((capnp.size() + 15) / 16));
    wr32(&body[4], 1);
    body.insert(body.end(), capnp.begin(), capnp.end());
    send_raw_control(std::move(body), hex2(type) + " v" + std::to_string(version));
}

void Session::send_clock(uint8_t type, const std::array<uint64_t, 4>& stamps) {
    ++clock_count_;
    Bytes body(8 + 32);
    body[0] = type;
    body[1] = control::kClockFlags;
    wr16(&body[2], 2);
    wr32(&body[4], clock_count_);
    for (int i = 0; i < 4; ++i) wr64(&body[8 + 8 * i], stamps[i]);
    send_raw_control(std::move(body), "clock " + hex2(type));
}

void Session::send_raw_control(Bytes body, const std::string& label) {
    outbox_.push_back({encode_message(kFlagsNormal, topic::kControl, control_seq_, body)});
    log("PC->Q", label, control_seq_);
    ++control_seq_;
}

void Session::send_stream(std::vector<StreamMessage> msgs) {
    std::vector<Bytes> raw;
    for (auto& m : msgs) {
        uint32_t& seq = stream_seq_[m.topic];
        raw.push_back(encode_message(m.flags, m.topic, uint16_t(seq), m.body));
        ++seq;
    }
    outbox_.push_back(std::move(raw));
}

uint16_t Session::stream_seq(uint8_t t) const {
    auto it = stream_seq_.find(t);
    return it == stream_seq_.end() ? 0 : uint16_t(it->second);
}

Session::Submit Session::submit_frame(std::span<const SliceData> slices, const FrameOptions& opt) {
    if (state_ != State::Streaming) return Submit::NotStreaming;
    if (!quest_offset_) return Submit::NoClock;  // need one clock ping to map our clock onto the Quest's
    if (slices.empty() || slices.size() > templates::slice_headers().size()) return Submit::BadSlices;
    if (!fits_message_cap(slices)) return Submit::TooLarge;
    FrameParams p;
    p.index = opt.index ? opt.index : next_frame_index_;
    p.target_vsync_quest_ns = opt.target_vsync_quest_ns ? opt.target_vsync_quest_ns
                                                        : clock_() + *quest_offset_ + opt.vsync_lead_ns;
    p.render_pose = opt.render_pose;
    p.rectify_mesh_id = opt.rectify_mesh_id;
    p.fov = opt.fov;
    p.intra = opt.intra;
    send_stream(build_frame(slices, p));
    next_frame_index_ = p.index + 1;
    return Submit::Ok;
}

Session::Submit Session::submit_audio(std::span<const float> stereo, int64_t ts_ns) {
    if (state_ != State::Streaming) return Submit::NotStreaming;
    if (stereo.size() != audio::kBlockFrames * audio::kChannels) return Submit::BadSlices;
    send_stream(audio::encode_playback(uint64_t(ts_ns ? ts_ns : clock_()), stereo));
    return Submit::Ok;
}

// --- receiving -------------------------------------------------------------

void Session::on_message(std::span<const uint8_t> raw) {
    if (raw.size() < kHeaderSize) return;
    uint8_t t = raw[1];
    int seq = rd16(&raw[4]);
    auto body = unpad_body(raw[0], raw.subspan(kHeaderSize));
    ++rx_counts_[t];
    if (t == topic::kHeadPose) {
        if (auto p = parse_head_pose(body)) {
            head_pose_ = *p;
            ++head_pose_count_;
        }
        return;
    }
    if (t == topic::kAudio) {
        // Mic: the segment (the 8-byte segment table before it doesn't parse)
        uint64_t ts;
        std::span<const uint8_t> pcm;
        if (audio::parse_block(body, ts, pcm)) {
            mic_.write_le(pcm);
            ++mic_blocks_;
        }
        return;
    }
    if (t == topic::kHands) {
        if (auto h = parse_hands(body)) {
            hands_ = *h;
            ++hands_count_;
        }
        return;
    }
    if (t == topic::kSkeleton) {
        // The bind pose, parents and capsules all sit in the first chunk of each hand's blob
        auto c = parse_skeleton_chunk(body);
        if (c && c->offset == 0)
            if (auto sk = parse_skeleton(c->data)) skeletons_[c->hand] = *sk;
        return;
    }
    if (t != topic::kControl) return;
    if (body.size() < 8) return;
    uint8_t type = body[0], flags = body[1];
    if (flags == control::kClockFlags && (type == control::kClockPing || type == control::kClockPong)) {
        on_clock(type, body);
        return;
    }
    capnp::Ptr tree;
    try {
        tree = capnp::decode(body.subspan(8));
    } catch (const std::exception& e) {
        log("Q->PC", hex2(type) + " undecodable: " + e.what(), seq);
        return;
    }
    on_control(type, tree, seq);
}

void Session::on_clock(uint8_t type, std::span<const uint8_t> body) {
    int64_t now = clock_();
    if (body.size() < 8 + 32) return;
    uint64_t stamps[4];
    for (int i = 0; i < 4; ++i) stamps[i] = rd64(&body[8 + 8 * i]);
    log("Q->PC", "clock " + hex2(type) + " " + std::to_string(stamps[2]));
    if (type != control::kClockPing) return;
    // Quest transmit time vs our receive time: good to ~1 ms (one-way USB latency)
    quest_offset_ = int64_t(stamps[2]) - now;
    // NTP-style reply: echo their transmit time, our receive and transmit times
    send_clock(control::kClockPong, {stamps[2], uint64_t(now), uint64_t(clock_()), 0});
}

void Session::on_control(uint8_t type, const capnp::Ptr& tree, int seq) {
    uint16_t version = tree ? uint16_t(tree->word(0) & 0xFFFF) : 0;
    log("Q->PC", hex2(type) + " v" + std::to_string(version), seq);
    if (type == control::kHello) {
        // A HELLO in any state starts over: the headset restarts pairing after Link is quit,
        // after a BYE, or when it never heard back from an earlier host
        if (auto info = parse_device_info(tree)) device_info_ = info;
        if (state_ != State::WaitHello && state_ != State::WaitHelloV3)
            log("info", std::string("headset restarted the handshake during ") + state_name(state_));
        if (version == 1) session_id_ = tree ? tree->word(6) : 0;
        begin_phase(version);
        return;
    }
    switch (state_) {
        case State::Phase1:
        case State::Phase2:
            if (type == expected_) advance_phase(type);
            break;
        case State::WaitLinkRequest:
            if (type != control::kLinkRequest) return;
            enter(State::WaitHelloV3, cfg_.hello_v3_timeout_ns);
            break;
        case State::Setup:
        case State::Streaming:
            if (type != control::kBye) return;
            // Protocol error or Link quit: its xrspd sends a new HELLO v1 soon; BYE if not
            log("info", "headset ended the session (BYE)");
            wait_hello(cfg_.bye_retry_ns);
            break;
        default:
            break;
    }
}

void Session::begin_phase(uint16_t version) {
    if (version != 1 && version != 3) {
        fail("unsupported HELLO version " + std::to_string(version));
        return;
    }
    version_ = version;
    control_seq_ = 0;  // each phase numbers control messages from 0
    if (version == 3) {
        // New streaming session: fresh per-topic seq, frame index and clock counter
        stream_seq_.clear();
        next_frame_index_ = cfg_.first_frame_index;
        clock_count_ = 0;
    }
    enter(version == 1 ? State::Phase1 : State::Phase2, cfg_.step_timeout_ns);
    expected_ = control::kAck;
    send_control(control::kConfig, version);
}

void Session::advance_phase(uint8_t type) {
    deadline_ = cfg_.step_timeout_ns > 0 ? clock_() + cfg_.step_timeout_ns : 0;
    switch (type) {
        case control::kAck:
            expected_ = control::kConfirmAck;
            send_control(control::kConfirm, version_);
            break;
        case control::kConfirmAck:
            expected_ = control::kReady;
            send_control(control::kStart, version_);
            break;
        case control::kReady:
            expected_ = 0;
            if (state_ == State::Phase1) {
                log("info", "phase 1 done; waiting for Enable Link (0x85)");
                enter(State::WaitLinkRequest, cfg_.link_request_timeout_ns);
            } else {
                finish_handshake();
            }
            break;
    }
}

void Session::finish_handshake() {
    log("info", "phase 2 done; starting clock sync");
    send_clock(control::kClockPing, {0, 0, uint64_t(clock_()), 0});
    if (cfg_.send_setup && !templates::stream_setup().empty()) {
        enter(State::Setup, 0);
        setup_next_ = 0;
        setup_due_ = clock_();
        tick();
    } else {
        on_streaming();
    }
}

void Session::on_streaming() {
    enter(State::Streaming, 0);
    ++streams_;
    if (cfg_.hands) set_hands(true);
}

void Session::set_hands(bool on) {
    // Segment table (1 segment, 2 words), then the segment, like Meta's PC
    send_stream({{topic::kInputControl, kFlagsNormal, Bytes{0, 0, 0, 0, 2, 0, 0, 0}},
                 {topic::kInputControl, kFlagsNormal, input_control_hands(on)}});
    log("PC->Q", std::string("InputControl hands ") + (on ? "on" : "off"));
}

void Session::tick() {
    int64_t now = clock_();
    if (deadline_ && now > deadline_) {
        // Our own limits end the session; a stalled headset step restarts pairing with a BYE
        if (state_ == State::WaitHello || state_ == State::WaitLinkRequest) {
            fail(state_ == State::WaitHello ? "no HELLO in time" : "no Enable Link (0x85) in time");
            return;
        }
        std::string what = state_ == State::WaitHelloV3 ? "no HELLO v3" : "no " + hex2(expected_);
        log("info", what + " in time; restarting the handshake");
        wait_hello(0);
    }
    if (state_ == State::WaitHello && bye_due_ && now >= bye_due_) {
        send_bye();
        bye_due_ = cfg_.bye_retry_ns > 0 ? now + cfg_.bye_retry_ns : 0;
    }
    if (state_ != State::Setup) return;
    // Replay Meta's post-handshake stream setup with the recorded gaps (capped)
    const auto& setup = templates::stream_setup();
    while (setup_next_ < setup.size() && now >= setup_due_) {
        const auto& m = setup[setup_next_];
        send_stream({{m.topic, m.flags, Bytes(m.body.begin(), m.body.end())}});
        if (++setup_next_ < setup.size())
            setup_due_ += std::min(setup[setup_next_].t_ns - m.t_ns, cfg_.setup_max_gap_ns);
    }
    if (setup_next_ == setup.size()) {
        log("info", "sent " + std::to_string(setup.size()) + " setup msgs");
        on_streaming();
    }
}

}  // namespace ql
