// Host API (ql_host_*): one connection, Session + Transport.
#include "questlink.h"

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
    s.hello_timeout_ns = int64_t(c.hello_timeout_ms) * 1'000'000;
    s.link_request_timeout_ns = int64_t(c.link_request_timeout_ms) * 1'000'000;
    s.step_timeout_ns = int64_t(c.step_timeout_ms) * 1'000'000;
    return s;
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
    cfg->step_timeout_ms = 3000;
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
    for (auto [t, n] : s.rx_counts())
        if (t < 64) out->rx_counts[t] = n;
    return QL_OK;
}

ql_status ql_host_get_device_info(ql_host* host, ql_device_info* out) {
    if (!host || !out) return QL_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(host->mu);
    const auto& d = host->session->device_info();
    if (!d) return QL_ERR_NO_DATA;
    auto tan_deg = [](float deg) { return float(std::tan(double(deg) * 3.14159265358979323846 / 180.0)); };
    std::memset(out, 0, sizeof *out);
    out->encode_width = d->encode_width;
    out->encode_height = d->encode_height;
    out->refresh_hz = d->refresh_hz;
    out->ipd_m = d->ipd_m;
    out->fov_left = {tan_deg(d->fov_deg[0]), tan_deg(d->fov_deg[1]), tan_deg(d->fov_deg[2]), tan_deg(d->fov_deg[3])};
    out->fov_right = {out->fov_left.up, out->fov_left.down, out->fov_left.right, out->fov_left.left};  // mirrored
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
    }
    return QL_ERR_INTERNAL;
}

}  // extern "C"
