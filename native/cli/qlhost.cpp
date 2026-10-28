// qlhost: Link host CLI on the questlink C API (same behaviour as host/handshake.py).
//
// Waits for the Quest's XRSP interface, does the handshake (press "Enable Link" in the
// headset between the phases), then either listens for 5 s or, with XRSP_REPLAY set, loops a
// recorded clip (host/replay/*.json) at 72 Hz with the render pose patched in.
//
// Usage: qlhost [seconds]
// Env (as handshake.py): XRSP_WAIT (600 s to wait for the device), XRSP_HELLO_WAIT (300 s),
//   XRSP_REPLAY=<clip.json>, XRSP_POSE=fixed|live|template, XRSP_ATTACH=1 with
//   XRSP_CONTROL_SEQ (16), XRSP_VIDEO_SEQ (0), XRSP_FRAME_START (clip's frame_index).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "clip.hpp"
#include "questlink.h"

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point t0 = Clock::now();

double since_start() { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void log_line(const char* dir, const std::string& what, int seq = -1) {
    std::printf("%8.3f %-6s %s", since_start(), dir, what.c_str());
    if (seq >= 0) std::printf(" seq=%d", seq);
    std::printf("\n");
    std::fflush(stdout);
}

void on_log(void*, const char* dir, const char* what, int seq) { log_line(dir, what, seq); }

std::string env(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

double env_num(const char* name, double fallback) {
    std::string v = env(name);
    return v.empty() ? fallback : std::atof(v.c_str());
}

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

enum class Outcome { Done, TransportLost, Failed };

class Run {
public:
    Run(ql_host* h) : h_(h) {}
    ~Run() { stop(); }

    void start_io() {
        io_ = std::thread([this] {
            auto last = Clock::now();
            ql_info prev{};
            while (!stop_) {
                ql_status st = ql_host_poll(h_, 100);
                if (st == QL_ERR_TRANSPORT) {
                    lost_ = true;
                    return;
                }
                if (Clock::now() - last >= std::chrono::seconds(2) && ql_host_get_state(h_) == QL_STATE_STREAMING) {
                    ql_info info;
                    ql_host_get_info(h_, &info);
                    std::string line = "rx";
                    for (int t = 2; t < 64; ++t)
                        if (info.rx_counts[t] != prev.rx_counts[t])
                            line += " t" + std::to_string(t) + ":" + std::to_string(info.rx_counts[t] - prev.rx_counts[t]);
                    log_line("info", line);
                    prev = info;
                    last = Clock::now();
                }
            }
        });
    }

    void stop() {
        stop_ = true;
        if (io_.joinable()) io_.join();
    }

    bool lost() const { return lost_; }

    // Wait until streaming; false on failure or transport loss
    bool wait_streaming() {
        for (;;) {
            if (lost_) return false;
            ql_state s = ql_host_get_state(h_);
            if (s == QL_STATE_STREAMING) return true;
            if (s == QL_STATE_FAILED) {
                log_line("info", std::string("session failed: ") + ql_host_get_error(h_));
                return false;
            }
            sleep_s(0.01);
        }
    }

    Outcome replay(const qlcli::Clip& clip, double seconds, const std::string& pose_mode) {
        log_line("info", "looping " + std::to_string(clip.frames.size()) + " frame(s) at 72 Hz");
        std::vector<std::vector<ql_slice>> prepared;
        for (const auto& f : clip.frames) {
            std::vector<ql_slice> s;
            for (const auto& [csd, video] : f)
                s.push_back({csd.empty() ? nullptr : csd.data(), csd.size(), video.data(), video.size()});
            prepared.push_back(std::move(s));
        }
        ql_info info;
        // Need one clock ping to map our clock onto the Quest's
        while (ql_host_get_info(h_, &info) == QL_OK && !info.has_clock_offset && !lost_) sleep_s(0.05);
        ql_head_pose fixed{};
        if (pose_mode != "template")
            while (ql_host_get_head_pose(h_, &fixed) != QL_OK && !lost_) sleep_s(0.01);
        if (lost_) return Outcome::TransportLost;

        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 72));
        auto next = Clock::now();
        const auto end = next + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        size_t sent = 0;
        while (Clock::now() < end) {
            const auto& slices = prepared[sent % prepared.size()];
            ql_frame_info fi{};
            fi.is_intra = 1;
            if (pose_mode != "template") {
                ql_head_pose live{};
                fi.has_render_pose = 1;
                fi.render_pose = pose_mode == "fixed" || ql_host_get_head_pose(h_, &live) != QL_OK ? fixed.pose : live.pose;
            }
            ql_status st = ql_host_submit_frame_slices(h_, slices.data(), uint32_t(slices.size()), &fi);
            if (st == QL_ERR_TRANSPORT) return Outcome::TransportLost;
            if (st == QL_ERR_NOT_STREAMING) {
                // Link quit and relaunched, or the headset sent BYE: the session re-handshakes
                log_line("info", "session restarted after " + std::to_string(sent) + " frames; waiting");
                if (!wait_streaming()) return lost_ ? Outcome::TransportLost : Outcome::Failed;
                while (ql_host_get_info(h_, &info) == QL_OK && !info.has_clock_offset && !lost_) sleep_s(0.05);
                next = Clock::now();
                continue;
            }
            if (st != QL_OK) {
                log_line("info", "submit failed: " + std::to_string(st) + " " + ql_host_get_error(h_));
                return Outcome::Failed;
            }
            ++sent;
            next += period;
            std::this_thread::sleep_until(next);
        }
        log_line("info", "streamed " + std::to_string(sent) + " frames");
        return Outcome::Done;
    }

    void log_resume() {
        // What a later XRSP_ATTACH run needs to continue this headset session
        ql_info info;
        if (ql_host_get_info(h_, &info) != QL_OK) return;
        log_line("info", "attach later with XRSP_CONTROL_SEQ=" + std::to_string(info.control_seq) +
                             " XRSP_VIDEO_SEQ=" + std::to_string(info.video_seq) +
                             " XRSP_FRAME_START=" + std::to_string(info.next_frame_index));
    }

private:
    ql_host* h_;
    std::thread io_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> lost_{false};
};

}  // namespace

int main(int argc, char** argv) {
    double seconds = argc > 1 ? std::atof(argv[1]) : 30;
    std::string replay = env("XRSP_REPLAY");
    std::string pose_mode = env("XRSP_POSE", "fixed");
    bool attach = !env("XRSP_ATTACH").empty();
    qlcli::Clip clip;
    if (!replay.empty()) {
        try {
            clip = qlcli::load_clip(replay);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "cannot load %s: %s\n", replay.c_str(), e.what());
            return 2;
        }
    }

    ql_config cfg;
    ql_config_init(&cfg);
    cfg.log = on_log;
    cfg.send_setup = replay.empty() ? 0 : 1;  // handshake.py sends the setup only when replaying
    cfg.hello_timeout_ms = int(env_num("XRSP_HELLO_WAIT", 300) * 1000);
    cfg.link_request_timeout_ms = int(seconds * 1000);
    cfg.bye_retry_ms = int(env_num("XRSP_BYE_RETRY_MS", cfg.bye_retry_ms));  // < 0: never send BYE
    cfg.first_frame_index = uint64_t(env_num("XRSP_FRAME_START", 0));
    if (!cfg.first_frame_index) cfg.first_frame_index = clip.frame_index;
    if (attach) {
        cfg.attach = 1;
        cfg.attach_control_seq = uint16_t(env_num("XRSP_CONTROL_SEQ", 16));
        cfg.attach_video_seq = uint16_t(env_num("XRSP_VIDEO_SEQ", 0));
    }

    // The Quest sends HELLO once per pairing attempt and drops it if no host is reading; the
    // session's preluding BYE asks for a new one. Open the interface as soon as it appears.
    double wait = env_num("XRSP_WAIT", 600);
    log_line("info", "waiting up to " + std::to_string(int(wait)) + "s for the Quest to be plugged in");
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(wait));
    for (;;) {
        ql_host* h = nullptr;
        ql_status st;
        while ((st = ql_host_open(&cfg, &h)) != QL_OK) {
            if (Clock::now() > deadline) {
                std::fprintf(stderr, "Quest XRSP interface not available (status %d)\n", st);
                return 1;
            }
            sleep_s(0.02);
        }
        log_line("info", "XRSP interface opened");
        Outcome outcome;
        {
            Run run(h);
            run.start_io();
            if (!run.wait_streaming()) {
                outcome = run.lost() ? Outcome::TransportLost : Outcome::Failed;
            } else if (replay.empty()) {
                sleep_s(5);  // listen like handshake.py does without a clip
                outcome = run.lost() ? Outcome::TransportLost : Outcome::Done;
            } else {
                outcome = run.replay(clip, seconds, pose_mode);
            }
            run.stop();
            if (outcome != Outcome::TransportLost) run.log_resume();
        }
        ql_host_close(h);
        if (outcome == Outcome::Done) return 0;
        if (outcome == Outcome::Failed) return 1;
        // Unplugged/replugged mid-session: wait for it to come back and start over
        log_line("info", "transport lost; waiting for reconnect");
    }
}
