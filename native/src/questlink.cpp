// Host API (ql_host_*): one connection, Session + Transport.
#include "questlink.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "session.hpp"
#include "transport.hpp"

namespace {

using namespace ql;

constexpr int kWriteTimeoutMs = 1000;
static_assert(int(State::Streaming) == QL_STATE_STREAMING && int(State::Failed) == QL_STATE_FAILED);

// Adapts a caller-provided ql_transport
class CallbackTransport final : public Transport {
public:
    explicit CallbackTransport(const ql_transport& t) : t_(t) {}
    ~CallbackTransport() override {
        if (t_.close) t_.close(t_.user);
    }
    bool write(const uint8_t* data, size_t len, int timeout_ms) override {
        return t_.write(t_.user, data, len, timeout_ms) == 0;
    }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override { return t_.read(t_.user, buf, cap, timeout_ms); }

private:
    ql_transport t_;
};

SessionConfig to_session_config(const ql_config& c) {
    SessionConfig s;
    s.attach = c.attach != 0;
    s.attach_control_seq = c.attach_control_seq;
    s.attach_video_seq = c.attach_video_seq;
    s.first_frame_index = c.first_frame_index ? c.first_frame_index : 1;
    s.send_setup = c.send_setup != 0;
    s.hands = c.hands != 0;
    s.haptics = c.haptics != 0;
    s.hello_timeout_ns = int64_t(c.hello_timeout_ms) * 1'000'000;
    s.link_request_timeout_ns = int64_t(c.link_request_timeout_ms) * 1'000'000;
    s.step_timeout_ns = int64_t(c.step_timeout_ms) * 1'000'000;
    if (c.bye_retry_ms < 0) s.bye_delay_ns = -1;
    s.bye_retry_ns = c.bye_retry_ms > 0 ? int64_t(c.bye_retry_ms) * 1'000'000 : 0;
    s.refresh_hz = c.refresh_hz;
    return s;
}

// Raw topic 35 bits -> QL_BUTTON_* / QL_TOUCH_*: the left controller's X/Y/stick sit 8 bits up
void to_ql_controller(const ControllerPose& p, const ControllerInput* in, ql_controller& out) {
    std::memset(&out, 0, sizeof out);
    if (p.has_pose) {
        out.pose = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3],
                    p.position[0],    p.position[1],    p.position[2],    p.sample_time_ns};
        out.flags |= (p.status & pose_status::kOrientationTracked ? QL_CONTROLLER_ROTATION_TRACKED : 0u) |
                     (p.status & pose_status::kPositionTracked ? QL_CONTROLLER_POSITION_TRACKED : 0u);
    } else {
        out.pose.qw = 1;
    }
    if (!in || !in->present) {
        out.flags = 0;  // put down: hands are tracked instead
        return;
    }
    out.flags |= QL_CONTROLLER_CONNECTED;
    uint32_t b = in->buttons | (in->buttons >> 8);
    out.buttons = (b & raw_button::kA ? QL_BUTTON_PRIMARY : 0u) | (b & raw_button::kB ? QL_BUTTON_SECONDARY : 0u) |
                  (b & raw_button::kRightStick ? QL_BUTTON_THUMBSTICK : 0u) |
                  (in->buttons & raw_button::kMenu ? QL_BUTTON_MENU : 0u);
    uint32_t t = in->touches;
    out.touches = (t & raw_touch::kPrimary ? QL_BUTTON_PRIMARY : 0u) | (t & raw_touch::kSecondary ? QL_BUTTON_SECONDARY : 0u) |
                  (t & raw_touch::kStick ? QL_BUTTON_THUMBSTICK : 0u) | (t & raw_touch::kTrigger ? QL_TOUCH_TRIGGER : 0u) |
                  (t & raw_touch::kThumbRest ? QL_TOUCH_THUMBREST : 0u);
    out.trigger = in->trigger;
    out.grip = in->grip;
    out.stick_x = in->stick[0];
    out.stick_y = in->stick[1];
}

void to_ql_hand(int hand, const HandRecord& r, const HandSkeleton* headset, ql_hand& out) {
    std::memset(&out, 0, sizeof out);
    if (r.status != 1) return;
    const HandSkeleton& sk = headset ? *headset : default_skeleton(hand);
    out.flags = QL_HAND_TRACKED | (headset ? QL_HAND_HEADSET_SKELETON : 0) |
                (r.input_status & hand_input::kPointerValid ? QL_HAND_AIM_VALID : 0) |
                (r.input_status & hand_input::kDominant ? QL_HAND_DOMINANT : 0);
    out.input_status = r.input_status;
    out.confidence = r.confidence;
    out.scale = r.scale;
    std::memcpy(out.pinch, r.pinch, sizeof out.pinch);
    uint64_t t = r.time_s > 0 ? uint64_t(r.time_s * 1e9 + 0.5) : 0;
    out.root = {r.root_rot[0], r.root_rot[1], r.root_rot[2], r.root_rot[3], r.root_pos[0], r.root_pos[1], r.root_pos[2], t};
    out.aim = {r.pointer_rot[0], r.pointer_rot[1], r.pointer_rot[2], r.pointer_rot[3],
               r.pointer_pos[0], r.pointer_pos[1], r.pointer_pos[2], t};
    auto joints = hand_joints(hand, r, sk);
    for (size_t j = 0; j < kHandJoints; ++j) {
        const JointPose& p = joints[j];
        out.joints[j] = {p.rot[0], p.rot[1], p.rot[2], p.rot[3], p.pos[0], p.pos[1], p.pos[2], p.radius};
    }
}

}  // namespace

struct ql_host {
    std::unique_ptr<Transport> transport;
    std::mutex mu;       // session, reassembler
    std::mutex send_mu;  // transport writes, in the order the session queued them
    std::unique_ptr<Session> session;
    Reassembler rx;
    bool broken = false;
    std::string error;
    std::string error_out;  // returned by ql_host_get_error

    // Called with mu held; releases it. Writes queued transfers in queue order.
    ql_status flush(std::unique_lock<std::mutex>& lock) {
        auto out = session->take_outgoing();
        if (out.empty()) {
            lock.unlock();
            return QL_OK;
        }
        std::lock_guard<std::mutex> send(send_mu);
        lock.unlock();
        for (const auto& msgs : out) {
            Bytes data = pack_transfer(msgs);
            if (!transport->write(data.data(), data.size(), kWriteTimeoutMs)) {
                std::lock_guard<std::mutex> again(mu);
                broken = true;
                error = "USB write failed";
                return QL_ERR_TRANSPORT;
            }
        }
        return QL_OK;
    }
};

extern "C" {

int64_t ql_now_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ql_config_init(ql_config* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof *cfg);
    cfg->attach_control_seq = 16;
    cfg->first_frame_index = 1;
    cfg->send_setup = 1;
    cfg->hands = 1;
    cfg->haptics = 1;
    cfg->step_timeout_ms = 3000;
    cfg->bye_retry_ms = 3000;
}

static ql_status open_with(const ql_config* cfg, std::unique_ptr<Transport> t, ql_host** out) {
    ql_config defaults;
    ql_config_init(&defaults);
    const ql_config& c = cfg ? *cfg : defaults;
    try {
        auto h = std::make_unique<ql_host>();
        h->transport = std::move(t);
        ql_log_fn log = c.log;
        void* user = c.log_user;
        Session::Log logger;
        if (log)
            logger = [log, user](const char* dir, const std::string& what, int seq) { log(user, dir, what.c_str(), seq); };
        h->session = std::make_unique<Session>(to_session_config(c), [] { return ql_now_ns(); }, logger);
        std::unique_lock<std::mutex> lock(h->mu);
        h->session->start();
        ql_status st = h->flush(lock);
        if (st != QL_OK) return st;
        *out = h.release();
        return QL_OK;
    } catch (...) {
        return QL_ERR_INTERNAL;
    }
}

ql_status ql_host_open(const ql_config* cfg, ql_host** out) {
    if (!out) return QL_ERR_ARGUMENT;
    *out = nullptr;
    OpenError err;
    std::string detail;
    auto t = open_usb(err, detail);
    {
        // Callers retry every few ms while waiting for the Quest: log each new reason once
        static std::mutex mu;
        static std::string last;
        std::lock_guard<std::mutex> l(mu);
        if (detail != last && !detail.empty() && cfg && cfg->log) cfg->log(cfg->log_user, "usb", detail.c_str(), -1);
        last = detail;
    }
    if (!t) return err == OpenError::Busy || err == OpenError::NotFound ? QL_ERR_NO_DEVICE : QL_ERR_UNSUPPORTED;
    return open_with(cfg, std::move(t), out);
}

ql_status ql_host_open_transport(const ql_config* cfg, const ql_transport* transport, ql_host** out) {
    if (!out || !transport || !transport->read || !transport->write) return QL_ERR_ARGUMENT;
    *out = nullptr;
    return open_with(cfg, std::make_unique<CallbackTransport>(*transport), out);
}

void ql_host_close(ql_host* host) { delete host; }

ql_status ql_host_poll(ql_host* host, int timeout_ms) {
    if (!host) return QL_ERR_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(host->mu);
        if (host->broken) return QL_ERR_TRANSPORT;
    }
    uint8_t buf[kReadSize];
    long n = host->transport->read(buf, sizeof buf, timeout_ms);
    std::unique_lock<std::mutex> lock(host->mu);
    if (n < 0) {
        host->broken = true;
        host->error = "USB read failed";
        return QL_ERR_TRANSPORT;
    }
    try {
        if (n > 0) {
            host->rx.feed(std::span<const uint8_t>(buf, size_t(n)));
            Bytes msg;
            while (host->rx.next(msg)) host->session->on_message(msg);
        }
        host->session->tick();
    } catch (const std::exception& e) {
        host->error = e.what();
        lock.unlock();
        return QL_ERR_INTERNAL;
    }
    bool failed = host->session->state() == State::Failed;
    ql_status st = host->flush(lock);
    if (st != QL_OK) return st;
    return failed ? QL_ERR_FAILED : QL_OK;
}

ql_state ql_host_get_state(ql_host* host) {
    if (!host) return QL_STATE_FAILED;
    std::lock_guard<std::mutex> lock(host->mu);
    return ql_state(int(host->session->state()));
}

const char* ql_state_name(ql_state state) { return state_name(State(int(state))); }

const char* ql_host_get_error(ql_host* host) {
    if (!host) return "no host";
    std::lock_guard<std::mutex> lock(host->mu);
    host->error_out = !host->error.empty() ? host->error : host->session->error();
    return host->error_out.c_str();
}

ql_status ql_host_get_info(ql_host* host, ql_info* out) {
    if (!host || !out) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    const Session& s = *host->session;
    std::memset(out, 0, sizeof *out);
    out->state = ql_state(int(s.state()));
    out->session_id = s.session_id();
    if (auto off = s.quest_clock_offset_ns()) {
        out->has_clock_offset = 1;
        out->quest_clock_offset_ns = *off;
    }
    out->next_frame_index = s.next_frame_index();
    out->control_seq = s.control_seq();
    out->video_seq = s.stream_seq(topic::kVideoFirst);
    for (int t = 0; t < 64; ++t) out->tx_seq[t] = s.stream_seq(uint8_t(t));
    for (auto [t, n] : s.rx_counts())
        if (t < 64) out->rx_counts[t] = n;
    out->streams = s.streams();
    out->byes_sent = s.byes_sent();
    return QL_OK;
}

ql_status ql_host_get_device_info(ql_host* host, ql_device_info* out) {
    if (!host || !out) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    const auto& d = host->session->device_info();
    if (!d) return QL_ERR_NO_DATA;
    auto tan_deg = [](float deg) { return float(std::tan(double(deg) * 3.14159265358979323846 / 180.0)); };
    auto fov = [&](const float* deg) { return ql_fov{tan_deg(deg[0]), tan_deg(deg[1]), tan_deg(deg[2]), tan_deg(deg[3])}; };
    auto copy = [](char* dst, size_t cap, const std::string& src) {
        size_t n = std::min(cap - 1, src.size());
        std::memcpy(dst, src.data(), n);
        dst[n] = 0;
    };
    std::memset(out, 0, sizeof *out);
    out->encode_width = d->encode_width;
    out->encode_height = d->encode_height;
    out->refresh_hz = d->refresh_hz;
    out->ipd_m = d->ipd_m;
    out->fov_left = fov(d->fov_deg[0]);
    out->fov_right = fov(d->fov_deg[1]);
    out->panel_width = d->panel_width;
    out->panel_height = d->panel_height;
    for (uint32_t hz : d->refresh_rates)
        if (out->refresh_count < QL_MAX_REFRESH_RATES) out->refresh_rates[out->refresh_count++] = hz;
    out->stream_refresh_hz = host->session->refresh_hz();
    for (uint16_t c : d->codecs)
        if (c < 32) out->codecs |= 1u << c;
    out->hello_version = d->version;
    for (uint16_t f : d->features)
        if (f < 64) out->features |= 1ull << f;
    copy(out->product, sizeof out->product, d->product);
    copy(out->manufacturer, sizeof out->manufacturer, d->manufacturer);
    copy(out->firmware, sizeof out->firmware, d->firmware);
    return QL_OK;
}

ql_status ql_host_get_head_pose(ql_host* host, ql_head_pose* out) {
    if (!host || !out) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    const auto& p = host->session->head_pose();
    if (!p) return QL_ERR_NO_DATA;
    out->pose = {p->orientation[0], p->orientation[1], p->orientation[2], p->orientation[3],
                 p->position[0],    p->position[1],    p->position[2],    p->sample_time_ns};
    std::memcpy(out->angular_velocity, p->angular_velocity, sizeof p->angular_velocity);
    std::memcpy(out->linear_velocity, p->linear_velocity, sizeof p->linear_velocity);
    std::memcpy(out->angular_acceleration, p->angular_acceleration, sizeof p->angular_acceleration);
    std::memcpy(out->linear_acceleration, p->linear_acceleration, sizeof p->linear_acceleration);
    out->vsync_ns = p->vsync_ns;
    out->count = host->session->head_pose_count();
    return QL_OK;
}

ql_status ql_host_get_hands(ql_host* host, ql_hands* out) {
    if (!host || !out) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    const Session& s = *host->session;
    const auto& f = s.hands();
    if (!f) return QL_ERR_NO_DATA;
    for (int h = 0; h < 2; ++h) {
        const auto& sk = s.skeleton(h);
        to_ql_hand(h, f->hands[h], sk ? &*sk : nullptr, out->hands[h]);
    }
    out->count = s.hands_count();
    return QL_OK;
}

ql_status ql_host_set_hands(ql_host* host, int enabled) {
    if (!host) return QL_ERR_ARGUMENT;
    std::unique_lock<std::mutex> lock(host->mu);
    if (host->broken) return QL_ERR_TRANSPORT;
    if (host->session->state() != State::Streaming) return QL_ERR_NOT_STREAMING;
    host->session->set_hands(enabled != 0);
    return host->flush(lock);
}

ql_status ql_host_get_controller(ql_host* host, int hand, ql_controller* out) {
    if (!host || !out || (hand != 0 && hand != 1)) return QL_ERR_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    std::lock_guard<std::mutex> lock(host->mu);
    const Session& s = *host->session;
    if (!s.controller_count()) return QL_ERR_NO_DATA;
    to_ql_controller(s.controller_poses()[hand], s.device_input() ? &s.device_input()->controllers[hand] : nullptr,
                     *out);
    return QL_OK;
}

ql_status ql_host_haptic(ql_host* host, int hand, float amplitude, float duration_s) {
    if (!host || (hand != 0 && hand != 1) || !(amplitude >= 0) || !(duration_s >= 0)) return QL_ERR_ARGUMENT;
    std::unique_lock<std::mutex> lock(host->mu);
    if (host->broken) return QL_ERR_TRANSPORT;
    auto r = host->session->haptic(hand, amplitude, uint64_t(double(duration_s) * 1e9));
    if (r == Session::Submit::NotStreaming) return QL_ERR_NOT_STREAMING;
    return host->flush(lock);
}

ql_status ql_host_submit_frame_slices(ql_host* host, const ql_slice* slices, uint32_t count, const ql_frame_info* info) {
    if (!host || !slices || count == 0) return QL_ERR_ARGUMENT;
    std::vector<SliceData> data;
    for (uint32_t i = 0; i < count; ++i) {
        const ql_slice& s = slices[i];
        if (!s.video || !s.video_len || (s.csd_len && !s.csd)) return QL_ERR_ARGUMENT;
        data.push_back({{s.csd, s.csd ? s.csd_len : 0}, {s.video, s.video_len}});
    }
    Session::FrameOptions opt;
    if (info) {
        opt.index = info->frame_index;
        opt.target_vsync_quest_ns = info->target_vsync_quest_ns;
        if (info->has_render_pose) {
            const ql_pose& r = info->render_pose;
            opt.render_pose = Pose{{r.qx, r.qy, r.qz, r.qw}, {r.px, r.py, r.pz}};
        }
        opt.intra = info->is_intra != 0;
        opt.picture_height = info->picture_height;
        if (info->rectify_mesh_id) opt.rectify_mesh_id = info->rectify_mesh_id;
        if (info->has_fov) {
            auto port = [](const ql_fov& f) { return FovPort{f.left, f.right, f.up, f.down}; };
            opt.fov = std::array<FovPort, 2>{port(info->fov_left), port(info->fov_right)};
        }
    }
    std::unique_lock<std::mutex> lock(host->mu);
    if (host->broken) return QL_ERR_TRANSPORT;
    Session::Submit r;
    try {
        r = host->session->submit_frame(data, opt);
    } catch (const std::exception& e) {
        host->error = e.what();
        return QL_ERR_INTERNAL;
    }
    switch (r) {
        case Session::Submit::Ok: return host->flush(lock);
        case Session::Submit::NotStreaming:
            return host->session->state() == State::Failed ? QL_ERR_FAILED : QL_ERR_NOT_STREAMING;
        case Session::Submit::NoClock: return QL_ERR_NO_CLOCK;
        case Session::Submit::BadSlices: return QL_ERR_ARGUMENT;
        case Session::Submit::TooLarge:
            host->error = "slice larger than one XRSP message (" + std::to_string(kMaxStreamMessage) + " B); frame dropped";
            return QL_ERR_ARGUMENT;
    }
    return QL_ERR_INTERNAL;
}

ql_status ql_host_send_audio(ql_host* host, const float* interleaved_stereo, int64_t ts_ns) {
    if (!host || !interleaved_stereo) return QL_ERR_ARGUMENT;
    std::unique_lock<std::mutex> lock(host->mu);
    if (host->broken) return QL_ERR_TRANSPORT;
    Session::Submit r;
    try {
        r = host->session->submit_audio({interleaved_stereo, audio::kBlockFrames * audio::kChannels}, ts_ns);
    } catch (const std::exception& e) {
        host->error = e.what();
        return QL_ERR_INTERNAL;
    }
    if (r == Session::Submit::Ok) return host->flush(lock);
    return host->session->state() == State::Failed ? QL_ERR_FAILED : QL_ERR_NOT_STREAMING;
}

int ql_host_read_mic(ql_host* host, int16_t* out, uint32_t max_frames) {
    if (!host || (!out && max_frames)) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    return int(host->session->read_mic(out, max_frames));
}

}  // extern "C"
