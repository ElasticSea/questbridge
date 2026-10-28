// Session API (qb_open, qb_poll, qb_submit_frame_texture, ...) over one process-wide Link
// runtime. The runtime is created on first qb_open and never destroyed: the Unity Editor keeps
// native plugins loaded across Play mode, and the headset session should survive with it.
//
// Threads: I/O (opens USB when the Quest appears, drives qb_host_poll, reopens after a drop),
// encode (encodes captured textures and sends them) and audio (one 10 ms block every 10 ms
// from the game-audio queue while streaming, silence when it runs dry). Textures are captured
// on the caller's render thread inside qb_submit_frame_texture / the render event.
//
// qb_session* values are registry tokens, never dereferenced, so render events still queued
// for a closed session find nothing and are ignored.
//
// Recovery: a USB error while streaming reopens the same host within QB_RESUME_MS and resumes
// the stream with its numbering if the headset still has it; otherwise (device gone longer,
// headset BYE / new HELLO / silence inside the session) a new handshake runs and the user
// presses Launch. Every new stream or resume starts with an IDR. qb_get_link_status reports it.
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
#include "fake_quest.hpp"
#include "frame.hpp"
#include "questbridge.h"
#include "texture_encoder.hpp"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

// Layouts the C# side marshals (Runtime/Native/QuestBridgeNative.cs, LayoutKind.Sequential)
static_assert(sizeof(qb_pose) == 40 && offsetof(qb_pose, time_ns) == 32);
static_assert(sizeof(qb_fov) == 16 && sizeof(qb_device_info) == 256 && offsetof(qb_device_info, features) == 104 &&
              offsetof(qb_device_info, firmware) == 176 && offsetof(qb_device_info, stream_codec) == 240 &&
              offsetof(qb_device_info, max_slice_bytes) == 244 && offsetof(qb_device_info, max_message_bytes) == 248 &&
              offsetof(qb_device_info, default_bitrate) == 252);
static_assert(sizeof(qb_controller) == 68 + 4 && offsetof(qb_controller, flags) == 64);
static_assert(sizeof(qb_hand_joint) == 32 && sizeof(qb_hand) == 944 && offsetof(qb_hand, joints) == 112);
static_assert(sizeof(qb_hands) == 1896 && offsetof(qb_hands, count) == 1888);
static_assert(sizeof(void*) != 8 || (sizeof(qb_frame_submit) == 64 && offsetof(qb_frame_submit, frame_index) == 56));
static_assert(sizeof(qb_link_status) == 192 && offsetof(qb_link_status, state_since_ns) == 24 &&
              offsetof(qb_link_status, encoder) == 32);

namespace {

using namespace qb;

// rectifyMeshId of Meta's game frames; 0 (its loading screen) disables reprojection
constexpr uint32_t kRectifyMeshId = 1001;

struct Job {
    int slot;
    qb_pose render_pose;
};

struct Client {
    bool any = false;
    uint64_t last_index = 0;
};

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : fallback;
}

// QB_CODEC: h264 | hevc | auto, or a raw CONFIG value
int env_codec() {
    const char* v = std::getenv("QB_CODEC");
    std::string s = v ? v : "";
    if (s == "h264" || s == "avc") return QB_CODEC_H264;
    if (s == "hevc" || s == "h265") return QB_CODEC_HEVC;
    return !s.empty() && s[0] >= '0' && s[0] <= '9' ? std::atoi(s.c_str()) : QB_CODEC_AUTO;
}

// Sleep until a qb_now_ns() deadline with ~1 ms precision (Windows' default timer tick is
// 15.6 ms, more than the 10 ms audio period)
void sleep_until_ns(int64_t deadline) {
    int64_t wait = deadline - qb_now_ns();
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

// stderr, or the file QB_LOG_FILE names (the Unity Editor on Windows drops plugin stderr)
void stderr_log(void*, const char* dir, const char* what, int seq) {
    static std::FILE* out = [] {
        const char* path = std::getenv("QB_LOG_FILE");
        std::FILE* f = path && *path ? std::fopen(path, "a") : nullptr;
        return f ? f : stderr;
    }();
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    if (seq >= 0)
        std::fprintf(out, "[questbridge] %-6s %s seq=%d\n", dir, what, seq);
    else
        std::fprintf(out, "[questbridge] %-6s %s\n", dir, what);
    std::fflush(out);
}

class Runtime {
public:
    static Runtime& get() {
        static Runtime* rt = new Runtime;  // intentionally leaked: threads run until process exit
        return *rt;
    }

    qb_session* open() {
        std::lock_guard<std::mutex> l(mu_);
        uint64_t id = next_id_++;
        clients_[id];
        return reinterpret_cast<qb_session*>(uintptr_t(id));
    }

    void close(qb_session* s) {
        std::lock_guard<std::mutex> l(mu_);
        clients_.erase(id(s));
    }

    bool known(qb_session* s) {
        std::lock_guard<std::mutex> l(mu_);
        return clients_.count(id(s)) != 0;
    }

    std::shared_ptr<qb_host> host() {
        std::lock_guard<std::mutex> l(mu_);
        return host_;
    }

    // Stop sending, log the numbers another host needs to continue the headset's session
    // (handshake.py XRSP_ATTACH), then close USB. The io thread ends; nothing reopens.
    void shutdown() {
        std::shared_ptr<qb_host> h;
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
        qb_info info;
        if (!h || qb_host_get_info(h.get(), &info) != QB_OK || info.state != QB_STATE_STREAMING) {
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

    qb_link_status link() {
        std::lock_guard<std::mutex> l(mu_);
        return link_;
    }

    int submit(qb_session* s, void* texture, const qb_pose* pose) {
        if (!texture || !pose) return QB_ERR_ARGUMENT;
        auto h = host();
        if (!h || qb_host_get_state(h.get()) != QB_STATE_STREAMING) return QB_ERR_NOT_STREAMING;
        TextureEncoder* enc = encoder(h.get());
        if (!enc) return QB_ERR_UNSUPPORTED;
        int slot = enc->capture(texture);  // copies now, on the caller's render thread
        if (slot < 0) return QB_ERR_NOT_STREAMING;
        (void)s;
        std::lock_guard<std::mutex> l(mu_);
        if (jobs_.size() >= 2) {  // encoder is behind: drop the oldest frame
            enc->release(jobs_.front().slot);
            jobs_.pop_front();
        }
        jobs_.push_back({slot, *pose});
        cv_.notify_one();
        return QB_OK;
    }

    int submit_audio(const float* stereo, uint32_t frames, uint32_t rate) {
        if (!audio_enabled_) return QB_ERR_UNSUPPORTED;
        auto h = host();
        if (!h || qb_host_get_state(h.get()) != QB_STATE_STREAMING) return QB_ERR_NOT_STREAMING;
        playback_.write(stereo, frames, rate);
        return QB_OK;
    }

    // Unity restarts frame_index every Play session (every qb_session); accept only increasing
    // indices per session. The headset's frame.index comes from the host's session-wide counter.
    bool accept_index(qb_session* s, uint64_t index) {
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
    std::shared_ptr<qb_host> host_;
    uint64_t host_generation_ = 0;
    std::deque<Job> jobs_;
    std::unique_ptr<TextureEncoder> encoder_;
    bool encoder_tried_ = false;
    std::thread io_, encode_, audio_;
    std::atomic<bool> stopping_{false};
    audio::PlaybackBuffer playback_;
    const bool audio_enabled_ = env_int("QB_AUDIO", 1) != 0;
    uint64_t dropped_ = 0;  // encode thread only
    uint64_t refits_ = 0;   // encode thread only
    uint64_t failed_ = 0;   // encode thread only
    std::shared_ptr<FakeQuest> fake_quest_;  // io thread only
    bool fake_failed_ = false;
    std::string encoder_name_;  // encode thread only
    const int64_t vsync_lead_ns_ = int64_t(env_int("QB_VSYNC_LEAD_MS", 16)) * 1'000'000;
    // USB back within this long after an error while streaming: try to resume (0 = never)
    const int64_t resume_window_ns_ = int64_t(env_int("QB_RESUME_MS", 500)) * 1'000'000;
    qb_link_status link_{};    // mu_
    uint32_t seen_losses_ = 0;  // io thread: host's qb_info.losses already counted

    Runtime() {
        io_ = std::thread([this] { io_loop(); });
        encode_ = std::thread([this] { encode_loop(); });
        if (audio_enabled_) audio_ = std::thread([this] { audio_loop(); });
    }

    static uint64_t id(qb_session* s) { return uint64_t(reinterpret_cast<uintptr_t>(s)); }

    // Created on the first frame of the first streaming session, at that session's rate and codec
    TextureEncoder* encoder(qb_host* h) {
        qb_device_info dev{};
        bool known = qb_host_get_device_info(h, &dev) == QB_OK;
        uint32_t hz = known ? dev.stream_refresh_hz : 0;
        std::lock_guard<std::mutex> l(mu_);
        if (!encoder_tried_) {
            encoder_tried_ = true;
            TextureEncoderConfig cfg;
            if (hz) cfg.fps = int(hz);
            cfg.hevc = known && dev.stream_codec == QB_CODEC_HEVC;
            if (known && dev.max_slice_bytes) cfg.max_slice_bytes = dev.max_slice_bytes;
#ifdef __APPLE__
            // All-intra VideoToolbox runs ~3x over the bitrate (~90 KB per strip per frame) and
            // the Quest Pro ends the session within seconds ("buffer too small"); P frames are
            // what played on the Pro from the Mac (gop 72 clip)
            cfg.gop = env_int("QB_GOP", 72);
#else
            cfg.gop = env_int("QB_GOP", 1);
#endif
            // QB_MBPS overrides the headset's default (qb_device_info.default_bitrate)
            cfg.bitrate = known && dev.default_bitrate ? dev.default_bitrate : 100'000'000u;
            if (int mbps = env_int("QB_MBPS", 0); mbps > 0) cfg.bitrate = uint32_t(mbps) * 1'000'000u;
            std::string err;
            encoder_ = create_texture_encoder(cfg, &err);
            if (!encoder_) stderr_log(nullptr, "info", ("no texture encoder: " + err).c_str(), -1);
        }
        return encoder_.get();
    }

    // Link state transitions; loss = why a stream ended (QB_LOSS_USB unless the session says)
    void set_link(int32_t state, int32_t loss) {
        std::lock_guard<std::mutex> l(mu_);
        if (state == link_.state) return;
        int32_t prev = link_.state;
        if (prev == QB_LINK_STREAMING) {
            ++link_.disconnects;
            link_.last_loss = loss ? loss : QB_LOSS_USB;
        }
        if (state == QB_LINK_STREAMING) {
            ++link_.connects;
            if (prev == QB_LINK_RESUMING) ++link_.resumes;
        }
        link_.state = state;
        link_.state_since_ns = qb_now_ns();
        static const char* names[] = {"no device", "connecting", "waiting for Launch", "streaming", "resuming"};
        static const char* losses[] = {"", "USB", "headset BYE", "new HELLO", "silence"};
        std::string what = std::string("link: ") + names[prev] + " -> " + names[state];
        if (prev == QB_LINK_STREAMING) what += std::string(" (lost: ") + losses[link_.last_loss] + ")";
        stderr_log(nullptr, "info", what.c_str(), -1);
    }

    void track(qb_host* h) {
        qb_info info;
        if (qb_host_get_info(h, &info) != QB_OK) return;
        int32_t loss = info.losses != seen_losses_ ? info.last_loss : QB_LOSS_NONE;
        seen_losses_ = info.losses;
        int32_t state = info.state == QB_STATE_STREAMING ? (info.resuming ? QB_LINK_RESUMING : QB_LINK_STREAMING)
                        : info.state == QB_STATE_WAIT_LINK_REQUEST ? QB_LINK_WAIT_LAUNCH
                                                                   : QB_LINK_CONNECTING;
        set_link(state, loss);
    }

    qb_config host_config() const {
        qb_config cfg;
        qb_config_init(&cfg);
        if (env_int("QB_LOG", 0)) cfg.log = stderr_log;
        cfg.hands = env_int("QB_HANDS", 1);
        cfg.haptics = env_int("QB_HAPTICS", 1);
        cfg.codec = env_codec();
        cfg.silence_timeout_ms = env_int("QB_SILENCE_MS", cfg.silence_timeout_ms);
        cfg.resume_timeout_ms = env_int("QB_RESUME_TIMEOUT_MS", cfg.resume_timeout_ms);
        return cfg;
    }

    // QB_FAKE_QUEST=<file>: the offline bench headset (fake_quest.hpp) instead of USB
    qb_status open_host(const qb_config& cfg, qb_host** out) {
        const char* fake = std::getenv("QB_FAKE_QUEST");
        if (!fake || !*fake) return qb_host_open(&cfg, out);
        if (fake_quest_ || fake_failed_) return QB_ERR_NO_DEVICE;  // one session, one try
        std::string err;
        fake_quest_ = load_fake_quest(fake, kConservativeCap.message,
                                      [](const std::string& s) { stderr_log(nullptr, "info", s.c_str(), -1); }, &err);
        if (!fake_quest_) {
            stderr_log(nullptr, "info", ("QB_FAKE_QUEST: " + err).c_str(), -1);
            fake_failed_ = true;
            return QB_ERR_NO_DEVICE;
        }
        stderr_log(nullptr, "info", (std::string("QB_FAKE_QUEST: offline bench headset from ") + fake).c_str(), -1);
        qb_transport t = fake_quest_transport(fake_quest_);
        return qb_host_open_transport(&cfg, &t, out);
    }

    void io_loop() {
        // QB_USB=0: never touch USB (Unity tests load the real library with a headset plugged in)
        if (env_int("QB_USB", 1) == 0) {
            stderr_log(nullptr, "info", "QB_USB=0: not opening USB", -1);
            while (!stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return;
        }
        std::shared_ptr<qb_host> lost;  // lost USB while streaming: reopen it to resume the stream
        int64_t lost_ns = 0, resumed_ns = 0;
        while (!stopping_) {
            std::shared_ptr<qb_host> h;
            if (lost && qb_now_ns() - lost_ns > resume_window_ns_) {
                // Gone this long, the headset restarted its Link stack (Link quit, replug): start over
                stderr_log(nullptr, "info", "USB not back in time; starting a new session", -1);
                lost.reset();
                set_link(QB_LINK_NO_DEVICE, QB_LOSS_USB);
            }
            if (lost) {
                if (qb_host_reopen(lost.get(), nullptr) == QB_OK) {
                    h = std::move(lost);
                    resumed_ns = qb_now_ns();
                }
            } else {
                qb_config cfg = host_config();
                qb_host* raw = nullptr;
                if (open_host(cfg, &raw) == QB_OK) {
                    h.reset(raw, qb_host_close);
                    seen_losses_ = 0;
                } else {
                    set_link(QB_LINK_NO_DEVICE, QB_LOSS_USB);
                }
            }
            if (!h) {
                // Poll fast: the sooner we hold the interface after enumeration, the sooner the
                // preluding BYE / HELLO exchange runs
                std::this_thread::sleep_for(std::chrono::milliseconds(lost ? 10 : 50));
                continue;
            }
            {
                std::lock_guard<std::mutex> l(mu_);
                if (stopping_) return;
                host_ = h;
                ++host_generation_;  // the next frame is an IDR
            }
            qb_status st = QB_OK;
            while (!stopping_) {
                st = qb_host_poll(h.get(), 50);
                track(h.get());
                if (st == QB_ERR_TRANSPORT || st == QB_ERR_FAILED) break;
            }
            if (stopping_) return;  // shutdown() owns the host now
            {
                std::lock_guard<std::mutex> l(mu_);
                host_.reset();
            }
            // USB error mid-stream: try the same session once more (not again right after a
            // resume, so a stream the headset keeps rejecting falls back to a new handshake).
            // Otherwise (unplugged, handshake failed) start over with a fresh one.
            qb_info info;
            bool streaming = qb_host_get_info(h.get(), &info) == QB_OK && info.state == QB_STATE_STREAMING &&
                             !info.resuming;
            if (st == QB_ERR_TRANSPORT && streaming && resume_window_ns_ > 0 &&
                (!resumed_ns || qb_now_ns() - resumed_ns > 5'000'000'000)) {
                stderr_log(nullptr, "info", "USB lost while streaming; reopening to resume", -1);
                lost = std::move(h);
                lost_ns = qb_now_ns();
                set_link(QB_LINK_RESUMING, QB_LOSS_USB);
                continue;
            }
            set_link(QB_LINK_NO_DEVICE, QB_LOSS_USB);
            h.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void encode_loop() {
        uint64_t idr_generation = 0;  // force an IDR on the first frame of each connection
        int stream_frames = 0;        // frames encoded in this stream; the first few are logged
        // Video stats, logged every 10 s: average and peak 1 s rate, largest slice, refits and
        // frames not sent (failed encodes, slices over the cap)
        uint64_t rate_bytes = 0, second_bytes = 0, peak_second_bytes = 0, rate_frames = 0;
        size_t rate_largest = 0;
        int64_t rate_since = qb_now_ns(), second_since = rate_since;
        for (;;) {
            Job job;
            std::shared_ptr<qb_host> h;
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
            // A new streaming session (Link relaunched, same USB link) also needs an IDR
            qb_info info;
            bool have_info = qb_host_get_info(h.get(), &info) == QB_OK;
            if (have_info) gen = (gen << 32) | info.streams;
            EncodedSlices out;
            if (!enc->encode(job.slot, gen != idr_generation, out)) {
                stderr_log(nullptr, "info", ("encode failed: " + enc->error() + " (" + std::to_string(++failed_) + " so far)").c_str(), -1);
                continue;
            }
            stream_frames = gen != idr_generation ? 1 : stream_frames + 1;
            idr_generation = gen;
            if (std::string name = enc->name(); name != encoder_name_) {  // created by the first encode
                encoder_name_ = name;
                set_encoder_info(name);
                stderr_log(nullptr, "info", ("encoder: " + name).c_str(), -1);
                if (std::string skipped = enc->selection(); !skipped.empty())
                    stderr_log(nullptr, "info", ("encoder skipped: " + skipped).c_str(), -1);
            }
            if (out.refits)
                stderr_log(nullptr, "info",
                           ("slice cap: frame re-encoded as IDR (" + std::to_string(++refits_) + " so far), largest slice " +
                            std::to_string(out.first_try_bytes) + " B -> " + std::to_string(out.largest) + " B at QP " +
                            std::to_string(out.qp)).c_str(),
                           -1);
            if (stream_frames <= 3) {
                size_t csd = 0;
                for (auto& c : out.slice_csd) csd = std::max(csd, c.size());
                stderr_log(nullptr, "info",
                           ("frame " + std::to_string(stream_frames) + (out.idr ? " IDR" : "") + ": " +
                            std::to_string(out.slices.size()) + " slices, largest " + std::to_string(out.largest) +
                            " B, csd " + std::to_string(std::max(csd, out.csd.size())) + " B, QP " + std::to_string(out.qp)).c_str(),
                           -1);
            }
            std::vector<qb_slice> slices;
            for (size_t i = 0; i < out.slices.size(); ++i) {
                const Bytes& csd = i < out.slice_csd.size() ? out.slice_csd[i] : out.csd;
                slices.push_back({csd.data(), csd.size(), out.slices[i].data(), out.slices[i].size()});
            }
            qb_frame_info fi{};
            fi.has_render_pose = 1;
            fi.render_pose = job.render_pose;
            fi.is_intra = out.idr ? 1 : 0;
            // Mesh 1001 + the FOV the eyes were rendered with (the device's) turn on timewarp.
            // The HELLO doesn't name a mesh; 1001 is what Meta's game frames use on Quest 3
            fi.rectify_mesh_id = kRectifyMeshId;
            // Target vsync = now + lead in the Quest clock. 20 ms (qbhost's default) made the
            // headset log "Frame arrived ~7.6 ms before deadline, frame drop", so aim ~4 ms closer
            if (have_info && info.has_clock_offset)
                fi.target_vsync_quest_ns = qb_now_ns() + info.quest_clock_offset_ns + vsync_lead_ns_;
            qb_device_info dev;
            if (qb_host_get_device_info(h.get(), &dev) == QB_OK) {
                fi.has_fov = 1;
                fi.fov_left = dev.fov_left;
                fi.fov_right = dev.fov_right;
            }
            for (auto& sl : slices) {
                rate_bytes += sl.csd_len + sl.video_len;
                second_bytes += sl.csd_len + sl.video_len;
                rate_largest = std::max({rate_largest, sl.csd_len, sl.video_len});
            }
            ++rate_frames;
            if (int64_t now = qb_now_ns(); now - second_since >= 1'000'000'000) {
                peak_second_bytes = std::max(peak_second_bytes, second_bytes * 1'000'000'000 / uint64_t(now - second_since));
                second_bytes = 0;
                second_since = now;
                if (now - rate_since >= 10'000'000'000) {
                    const uint64_t ns = uint64_t(now - rate_since);
                    stderr_log(nullptr, "info",
                               ("video: " + std::to_string(rate_frames * 1'000'000'000 / ns) + " fps, " +
                                std::to_string(rate_bytes * 8'000 / ns) + " Mbit/s avg, " +
                                std::to_string(peak_second_bytes * 8 / 1'000'000) + " peak (1 s), largest slice " +
                                std::to_string(rate_largest) + " B; so far " + std::to_string(refits_) + " refits, " +
                                std::to_string(failed_) + " failed encodes, " + std::to_string(dropped_) + " frames over the cap")
                                   .c_str(),
                               -1);
                    rate_bytes = peak_second_bytes = rate_frames = 0;
                    rate_largest = 0;
                    rate_since = now;
                }
            }
            if (qb_host_submit_frame_slices(h.get(), slices.data(), uint32_t(slices.size()), &fi) == QB_ERR_ARGUMENT)
                stderr_log(nullptr, "info",
                           (std::string(qb_host_get_error(h.get())) + " (" + std::to_string(++dropped_) + " so far)").c_str(), -1);
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
            if (!h || qb_host_get_state(h.get()) != QB_STATE_STREAMING) {
                h.reset();
                pacer.reset();
                playback_.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            for (int n = pacer.due(qb_now_ns()); n > 0; --n) {
                if (playback_.read(block, audio::kBlockFrames) > 0) ++real;
                float p = 0.f;
                for (float s : block) p = std::max(p, std::abs(s));
                if (p > 1e-4f) ++audible;
                peak = std::max(peak, p);
                if (qb_host_send_audio(h.get(), block, 0) != QB_OK) break;
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

void QB_UNITY_CALL on_render_event(int event_id, void* data) {
    if (event_id != QB_EVENT_SUBMIT_FRAME || !data) return;
    const auto* f = static_cast<const qb_frame_submit*>(data);
    qb_submit_frame_texture(f->session, f->native_texture, &f->render_pose, f->frame_index);
}

}  // namespace

extern "C" {

qb_session* qb_open(void) {
    try {
        return Runtime::get().open();
    } catch (...) {
        return nullptr;
    }
}

void qb_close(qb_session* session) {
    if (session) Runtime::get().close(session);
}

void qb_shutdown(void) { Runtime::get().shutdown(); }

int qb_poll(qb_session* session) {
    auto& rt = Runtime::get();
    if (!rt.known(session)) return 0;
    auto h = rt.host();
    if (!h) return 0;
    return qb_host_get_state(h.get()) == QB_STATE_STREAMING ? 2 : 1;
}

int qb_get_link_status(qb_session* session, qb_link_status* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QB_ERR_ARGUMENT;
    *out = rt.link();
    std::string enc = encoder_info();
    std::memset(out->encoder, 0, sizeof out->encoder);
    std::strncpy(out->encoder, enc.c_str(), sizeof out->encoder - 1);
    return QB_OK;
}

int qb_get_device_info(qb_session* session, qb_device_info* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QB_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? qb_host_get_device_info(h.get(), out) : QB_ERR_NO_DATA;
}

int qb_get_head_pose(qb_session* session, qb_pose* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QB_ERR_ARGUMENT;
    auto h = rt.host();
    qb_head_pose p;
    if (!h || qb_host_get_head_pose(h.get(), &p) != QB_OK) return QB_ERR_NO_DATA;
    *out = p.pose;
    return QB_OK;
}

int qb_get_hands(qb_session* session, qb_hands* out) {
    auto& rt = Runtime::get();
    if (!out || !rt.known(session)) return QB_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? qb_host_get_hands(h.get(), out) : QB_ERR_NO_DATA;
}

int qb_get_controller(qb_session* session, int hand, qb_controller* out) {
    auto& rt = Runtime::get();
    if (!out || (hand != 0 && hand != 1) || !rt.known(session)) return QB_ERR_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    auto h = rt.host();
    return h ? qb_host_get_controller(h.get(), hand, out) : QB_ERR_NO_DATA;
}

int qb_haptic(qb_session* session, int hand, float amplitude, float duration_s) {
    auto& rt = Runtime::get();
    if ((hand != 0 && hand != 1) || !rt.known(session)) return QB_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? qb_host_haptic(h.get(), hand, amplitude, duration_s) : QB_ERR_NOT_STREAMING;
}

int qb_submit_frame_texture(qb_session* session, void* native_texture, const qb_pose* render_pose,
                            uint64_t frame_index) {
    auto& rt = Runtime::get();
    if (!rt.accept_index(session, frame_index)) return QB_ERR_ARGUMENT;  // closed session or stale index
    return rt.submit(session, native_texture, render_pose);
}

qb_render_event_fn qb_get_render_event_func(void) { return on_render_event; }

int qb_submit_audio(qb_session* session, const float* interleaved_stereo, uint32_t frames, uint32_t sample_rate) {
    auto& rt = Runtime::get();
    if (!interleaved_stereo || !sample_rate || !rt.known(session)) return QB_ERR_ARGUMENT;
    return rt.submit_audio(interleaved_stereo, frames, sample_rate);
}

int qb_read_mic(qb_session* session, int16_t* out, uint32_t max_frames) {
    auto& rt = Runtime::get();
    if ((!out && max_frames) || !rt.known(session)) return QB_ERR_ARGUMENT;
    auto h = rt.host();
    return h ? qb_host_read_mic(h.get(), out, max_frames) : 0;
}

}  // extern "C"
