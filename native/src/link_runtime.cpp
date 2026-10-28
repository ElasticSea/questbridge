// Session API (ql_open, ql_poll, ql_submit_frame_texture, ...) over one process-wide Link
// runtime. The runtime is created on first ql_open and never destroyed: the Unity Editor keeps
// native plugins loaded across Play mode, and the headset session should survive with it.
//
// Threads: I/O (opens USB when the Quest appears, drives ql_host_poll, reopens after a drop),
// encode (encodes captured textures and sends them) and audio (one 10 ms block every 10 ms
// from the game-audio queue while streaming, silence when it runs dry). Textures are captured
// on the caller's render thread inside ql_submit_frame_texture / the render event.
//
// ql_session* values are registry tokens, never dereferenced, so render events still queued
// for a closed session find nothing and are ignored.
#include <algorithm>
#include <atomic>
#include <cmath>
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

#include "audio.hpp"
#include "questlink.h"
#include "texture_encoder.hpp"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

// Layouts the C# side marshals (Runtime/Native/QuestLinkNative.cs, LayoutKind.Sequential)
static_assert(sizeof(ql_pose) == 40 && offsetof(ql_pose, time_ns) == 32);
static_assert(sizeof(ql_fov) == 16 && sizeof(ql_device_info) == 48);
static_assert(sizeof(ql_controller) == 68 + 4 && offsetof(ql_controller, flags) == 64);
static_assert(sizeof(ql_hand_joint) == 32 && sizeof(ql_hand) == 944 && offsetof(ql_hand, joints) == 112);
static_assert(sizeof(ql_hands) == 1896 && offsetof(ql_hands, count) == 1888);
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

// Sleep until a ql_now_ns() deadline with ~1 ms precision (Windows' default timer tick is
// 15.6 ms, more than the 10 ms audio period)
void sleep_until_ns(int64_t deadline) {
    int64_t wait = deadline - ql_now_ns();
    if (wait <= 0) return;
#if defined(_WIN32)
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(wait / 100);  // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
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

    // Stop sending, log the numbers another host needs to continue the headset's session
    // (handshake.py XRSP_ATTACH), then close USB. The io thread ends; nothing reopens.
    void shutdown() {
        std::shared_ptr<ql_host> h;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (stopping_) return;
            stopping_ = true;
            h = std::move(host_);
        }
        if (io_.joinable()) io_.join();
        // Frames or audio blocks already in flight still hold the host
        for (int i = 0; i < 100 && h && h.use_count() > 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ql_info info;
        if (!h || ql_host_get_info(h.get(), &info) != QL_OK || info.state != QL_STATE_STREAMING) {
            stderr_log(nullptr, "info", "shutdown: no streaming session", -1);
            return;
        }
        std::string video, other;
        for (int t = 0; t < 64; ++t) {
            bool slice = t >= 10 && t < 15;
            if (slice)
                video += (video.empty() ? "" : ",") + std::to_string(info.tx_seq[t]);
            else if (info.tx_seq[t])
                other += (other.empty() ? "" : ",") + std::to_string(t) + ":" + std::to_string(info.tx_seq[t]);
        }
        std::string what = "resume with XRSP_FRAME_START=" + std::to_string(info.next_frame_index) +
                           " XRSP_CONTROL_SEQ=" + std::to_string(info.control_seq) + " XRSP_VIDEO_SEQ=" + video +
                           " XRSP_STREAM_SEQ=" + other;
        stderr_log(nullptr, "info", what.c_str(), -1);
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

    int submit_audio(const float* stereo, uint32_t frames, uint32_t rate) {
        if (!audio_enabled_) return QL_ERR_UNSUPPORTED;
        auto h = host();
        if (!h || ql_host_get_state(h.get()) != QL_STATE_STREAMING) return QL_ERR_NOT_STREAMING;
        playback_.write(stereo, frames, rate);
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
    std::thread io_, encode_, audio_;
    std::atomic<bool> stopping_{false};
    audio::PlaybackBuffer playback_;
    const bool audio_enabled_ = env_int("QL_AUDIO", 1) != 0;
    uint64_t dropped_ = 0;  // encode thread only
    const int64_t vsync_lead_ns_ = int64_t(env_int("QL_VSYNC_LEAD_MS", 16)) * 1'000'000;

    Runtime() {
        io_ = std::thread([this] { io_loop(); });
        encode_ = std::thread([this] { encode_loop(); });
        if (audio_enabled_) audio_ = std::thread([this] { audio_loop(); });
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
        // QL_USB=0: never touch USB (Unity tests load the real library with a headset plugged in)
        if (env_int("QL_USB", 1) == 0) {
            stderr_log(nullptr, "info", "QL_USB=0: not opening USB", -1);
            while (!stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return;
        }
        while (!stopping_) {
            ql_config cfg;
            ql_config_init(&cfg);
            if (env_int("QL_LOG", 0)) cfg.log = stderr_log;
            cfg.hands = env_int("QL_HANDS", 1);
            ql_host* raw = nullptr;
            if (ql_host_open(&cfg, &raw) != QL_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            std::shared_ptr<ql_host> h(raw, ql_host_close);
            {
                std::lock_guard<std::mutex> l(mu_);
                if (stopping_) return;
                host_ = h;
                ++host_generation_;
            }
            while (!stopping_) {
                ql_status st = ql_host_poll(h.get(), 50);
                if (st == QL_ERR_TRANSPORT || st == QL_ERR_FAILED) break;
            }
            if (stopping_) return;  // shutdown() owns the host now
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
            // Target vsync = now + lead in the Quest clock. 20 ms (qlhost's default) made the
            // headset log "Frame arrived ~7.6 ms before deadline, frame drop", so aim ~4 ms closer
            ql_info info;
            if (ql_host_get_info(h.get(), &info) == QL_OK && info.has_clock_offset)
                fi.target_vsync_quest_ns = ql_now_ns() + info.quest_clock_offset_ns + vsync_lead_ns_;
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

    // Meta's host sends a block every 10 ms for the whole session, silent or not; the headset
    // resamples adaptively around a 20 ms buffer.
    void audio_loop() {
        audio::Pacer pacer;
        float block[audio::kBlockFrames * audio::kChannels];
        uint64_t blocks = 0, real = 0, audible = 0;
        float peak = 0.f;
        for (;;) {
            auto h = host();
            if (!h || ql_host_get_state(h.get()) != QL_STATE_STREAMING) {
                h.reset();
                pacer.reset();
                playback_.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            for (int n = pacer.due(ql_now_ns()); n > 0; --n) {
                if (playback_.read(block, audio::kBlockFrames) > 0) ++real;
                float p = 0.f;
                for (float s : block) p = std::max(p, std::abs(s));
                if (p > 1e-4f) ++audible;
                peak = std::max(peak, p);
                if (ql_host_send_audio(h.get(), block, 0) != QL_OK) break;
                if (++blocks % 1000 == 0) {  // every 10 s
                    std::string what = "audio: " + std::to_string(blocks) + " blocks, " + std::to_string(real) +
                                       " from the game, " + std::to_string(audible) + " audible, peak " +
                                       std::to_string(peak) + ", " + std::to_string(pacer.skipped()) + " skipped, " +
                                       std::to_string(playback_.dropped_frames()) + " frames dropped";
                    peak = 0.f;
                    stderr_log(nullptr, "info", what.c_str(), -1);
                }
            }
            h.reset();
            sleep_until_ns(pacer.next_ns());
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

void ql_shutdown(void) { Runtime::get().shutdown(); }

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

int ql_get_hands(ql_session* session, ql_hands* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QL_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? ql_host_get_hands(h.get(), out) : QL_ERR_NO_DATA;
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

int ql_submit_audio(ql_session* session, const float* interleaved_stereo, uint32_t frames, uint32_t sample_rate) {
    auto& rt = Runtime::get();
    if (!interleaved_stereo || !sample_rate || !rt.known(session)) return QL_ERR_ARGUMENT;
    return rt.submit_audio(interleaved_stereo, frames, sample_rate);
}

int ql_read_mic(ql_session* session, int16_t* out, uint32_t max_frames) {
    auto& rt = Runtime::get();
    if ((!out && max_frames) || !rt.known(session)) return QL_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? ql_host_read_mic(h.get(), out, max_frames) : 0;
}

}  // extern "C"
