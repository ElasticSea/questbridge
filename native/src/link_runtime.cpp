// Session API (ql_open, ql_poll, ql_submit_frame_texture, ...) over one process-wide Link
// runtime. The runtime is created on first ql_open and never destroyed: the Unity Editor keeps
// native plugins loaded across Play mode, and the headset session should survive with it.
//
// Threads: I/O (opens USB when the Quest appears, drives ql_host_poll, reopens after a drop)
// and encode (encodes captured textures and sends them). Textures are captured on the caller's
// render thread inside ql_submit_frame_texture / the render event.
//
// ql_session* values are registry tokens, never dereferenced, so render events still queued
// for a closed session find nothing and are ignored.
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "questlink.h"
#include "texture_encoder.hpp"

// Layouts the C# side marshals (Runtime/Native/QuestLinkNative.cs, LayoutKind.Sequential)
static_assert(sizeof(ql_pose) == 40 && offsetof(ql_pose, time_ns) == 32);
static_assert(sizeof(ql_fov) == 16 && sizeof(ql_device_info) == 48);
static_assert(sizeof(ql_controller) == 68 + 4 && offsetof(ql_controller, flags) == 64);
static_assert(sizeof(void*) != 8 || (sizeof(ql_frame_submit) == 64 && offsetof(ql_frame_submit, frame_index) == 56));

namespace {

using namespace ql;

// rectifyMeshId of Meta's game frames; 0 (its loading screen) disables reprojection
constexpr uint32_t kRectifyMeshId = 1001;

struct Job {
    int slot;
    ql_pose render_pose;
};

struct Client {
    bool any = false;
    uint64_t last_index = 0;
};

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : fallback;
}

// stderr, or the file QL_LOG_FILE names (the Unity Editor on Windows drops plugin stderr)
void stderr_log(void*, const char* dir, const char* what, int seq) {
    static std::FILE* out = [] {
        const char* path = std::getenv("QL_LOG_FILE");
        std::FILE* f = path && *path ? std::fopen(path, "a") : nullptr;
        return f ? f : stderr;
    }();
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    if (seq >= 0)
        std::fprintf(out, "[questlink] %-6s %s seq=%d\n", dir, what, seq);
    else
        std::fprintf(out, "[questlink] %-6s %s\n", dir, what);
    std::fflush(out);
}

class Runtime {
public:
    static Runtime& get() {
        static Runtime* rt = new Runtime;  // intentionally leaked: threads run until process exit
        return *rt;
    }

    ql_session* open() {
        std::lock_guard<std::mutex> l(mu_);
        uint64_t id = next_id_++;
        clients_[id];
        return reinterpret_cast<ql_session*>(uintptr_t(id));
    }

    void close(ql_session* s) {
        std::lock_guard<std::mutex> l(mu_);
        clients_.erase(id(s));
    }

    bool known(ql_session* s) {
        std::lock_guard<std::mutex> l(mu_);
        return clients_.count(id(s)) != 0;
    }

    std::shared_ptr<ql_host> host() {
        std::lock_guard<std::mutex> l(mu_);
        return host_;
    }

    int submit(ql_session* s, void* texture, const ql_pose* pose) {
        if (!texture || !pose) return QL_ERR_ARGUMENT;
        auto h = host();
        if (!h || ql_host_get_state(h.get()) != QL_STATE_STREAMING) return QL_ERR_NOT_STREAMING;
        TextureEncoder* enc = encoder();
        if (!enc) return QL_ERR_UNSUPPORTED;
        int slot = enc->capture(texture);  // copies now, on the caller's render thread
        if (slot < 0) return QL_ERR_NOT_STREAMING;
        (void)s;
        std::lock_guard<std::mutex> l(mu_);
        if (jobs_.size() >= 2) {  // encoder is behind: drop the oldest frame
            enc->release(jobs_.front().slot);
            jobs_.pop_front();
        }
        jobs_.push_back({slot, *pose});
        cv_.notify_one();
        return QL_OK;
    }

    // Unity restarts frame_index every Play session (every ql_session); accept only increasing
    // indices per session. The headset's frame.index comes from the host's session-wide counter.
    bool accept_index(ql_session* s, uint64_t index) {
        std::lock_guard<std::mutex> l(mu_);
        auto it = clients_.find(id(s));
        if (it == clients_.end()) return false;
        Client& c = it->second;
        if (c.any && index <= c.last_index) return false;
        c.any = true;
        c.last_index = index;
        return true;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<uint64_t, Client> clients_;
    uint64_t next_id_ = 1;
    std::shared_ptr<ql_host> host_;
    uint64_t host_generation_ = 0;
    std::deque<Job> jobs_;
    std::unique_ptr<TextureEncoder> encoder_;
    bool encoder_tried_ = false;
    std::thread io_, encode_;
    uint64_t dropped_ = 0;  // encode thread only

    Runtime() {
        io_ = std::thread([this] { io_loop(); });
        encode_ = std::thread([this] { encode_loop(); });
    }

    static uint64_t id(ql_session* s) { return uint64_t(reinterpret_cast<uintptr_t>(s)); }

    TextureEncoder* encoder() {
        std::lock_guard<std::mutex> l(mu_);
        if (!encoder_tried_) {
            encoder_tried_ = true;
            TextureEncoderConfig cfg;
            cfg.gop = env_int("QL_GOP", 1);
            cfg.bitrate = uint32_t(env_int("QL_MBPS", 100)) * 1'000'000u;
            std::string err;
            encoder_ = create_texture_encoder(cfg, &err);
            if (!encoder_) stderr_log(nullptr, "info", ("no texture encoder: " + err).c_str(), -1);
        }
        return encoder_.get();
    }

    void io_loop() {
        for (;;) {
            ql_config cfg;
            ql_config_init(&cfg);
            if (env_int("QL_LOG", 0)) cfg.log = stderr_log;
            ql_host* raw = nullptr;
            if (ql_host_open(&cfg, &raw) != QL_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            std::shared_ptr<ql_host> h(raw, ql_host_close);
            {
                std::lock_guard<std::mutex> l(mu_);
                host_ = h;
                ++host_generation_;
            }
            for (;;) {
                ql_status st = ql_host_poll(h.get(), 50);
                if (st == QL_ERR_TRANSPORT || st == QL_ERR_FAILED) break;
            }
            {
                // Unplugged, reset or handshake failed: start over with a fresh handshake
                std::lock_guard<std::mutex> l(mu_);
                host_.reset();
            }
            h.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    void encode_loop() {
        uint64_t idr_generation = 0;  // force an IDR on the first frame of each connection
        for (;;) {
            Job job;
            std::shared_ptr<ql_host> h;
            uint64_t gen;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return !jobs_.empty(); });
                job = jobs_.front();
                jobs_.pop_front();
                h = host_;
                gen = host_generation_;
            }
            TextureEncoder* enc = encoder_.get();  // set before any job was queued
            if (!h) {
                enc->release(job.slot);
                continue;
            }
            EncodedSlices out;
            if (!enc->encode(job.slot, gen != idr_generation, out)) {
                stderr_log(nullptr, "info", ("encode failed: " + enc->error()).c_str(), -1);
                continue;
            }
            idr_generation = gen;
            std::vector<ql_slice> slices;
            for (const auto& s : out.slices)
                slices.push_back({out.csd.data(), out.csd.size(), s.data(), s.size()});
            ql_frame_info fi{};
            fi.has_render_pose = 1;
            fi.render_pose = job.render_pose;
            fi.is_intra = out.idr ? 1 : 0;
            // Mesh 1001 + the FOV the eyes were rendered with (the device's) turn on timewarp
            fi.rectify_mesh_id = kRectifyMeshId;
            ql_device_info dev;
            if (ql_host_get_device_info(h.get(), &dev) == QL_OK) {
                fi.has_fov = 1;
                fi.fov_left = dev.fov_left;
                fi.fov_right = dev.fov_right;
            }
            if (ql_host_submit_frame_slices(h.get(), slices.data(), uint32_t(slices.size()), &fi) == QL_ERR_ARGUMENT &&
                dropped_++ % 72 == 0)
                stderr_log(nullptr, "info", ql_host_get_error(h.get()), -1);
        }
    }
};

void QL_UNITY_CALL on_render_event(int event_id, void* data) {
    if (event_id != QL_EVENT_SUBMIT_FRAME || !data) return;
    const auto* f = static_cast<const ql_frame_submit*>(data);
    ql_submit_frame_texture(f->session, f->native_texture, &f->render_pose, f->frame_index);
}

}  // namespace

extern "C" {

ql_session* ql_open(void) {
    try {
        return Runtime::get().open();
    } catch (...) {
        return nullptr;
    }
}

void ql_close(ql_session* session) {
    if (session) Runtime::get().close(session);
}

int ql_poll(ql_session* session) {
    auto& rt = Runtime::get();
    if (!rt.known(session)) return 0;
    auto h = rt.host();
    if (!h) return 0;
    return ql_host_get_state(h.get()) == QL_STATE_STREAMING ? 2 : 1;
}

int ql_get_device_info(ql_session* session, ql_device_info* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QL_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? ql_host_get_device_info(h.get(), out) : QL_ERR_NO_DATA;
}

int ql_get_head_pose(ql_session* session, ql_pose* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QL_ERR_ARGUMENT;
    auto h = rt.host();
    ql_head_pose p;
    if (!h || ql_host_get_head_pose(h.get(), &p) != QL_OK) return QL_ERR_NO_DATA;
    *out = p.pose;
    return QL_OK;
}

int ql_get_controller(ql_session* session, int hand, ql_controller* out) {
    if (!out || (hand != 0 && hand != 1) || !Runtime::get().known(session)) return QL_ERR_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    return QL_ERR_UNSUPPORTED;  // controller topics not decoded yet (roadmap M7)
}

int ql_haptic(ql_session* session, int hand, float, float) {
    if ((hand != 0 && hand != 1) || !Runtime::get().known(session)) return QL_ERR_ARGUMENT;
    return QL_ERR_UNSUPPORTED;
}

int ql_submit_frame_texture(ql_session* session, void* native_texture, const ql_pose* render_pose,
                            uint64_t frame_index) {
    auto& rt = Runtime::get();
    if (!rt.accept_index(session, frame_index)) return QL_ERR_ARGUMENT;  // closed session or stale index
    return rt.submit(session, native_texture, render_pose);
}

ql_render_event_fn ql_get_render_event_func(void) { return on_render_event; }

}  // extern "C"
