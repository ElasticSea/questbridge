// Recovery inside a running session: the Session API runtime (what Unity calls) against a
// reactive fake headset (fake_headset.hpp) while a "game" thread keeps submitting frames and
// audio, as Play mode does. Every disconnect must come back to streaming on its own: resumed
// with the same numbering after a USB glitch, or a new handshake (fresh numbering, IDR) after
// a USB drop, a headset BYE, a new HELLO or silence. The runtime is a process singleton, so
// the tests run in order on one runtime.
#include <atomic>
#include <cstdlib>
#include <random>

#include "fake_headset.hpp"
#include "harness.hpp"
#include "questlink.h"
#include "texture_encoder.hpp"

using namespace ql;

namespace {

// "Texture" = its frame number; every frame is encoded as 5 tiny slices
class FakeTextureEncoder final : public TextureEncoder {
public:
    int capture(void*) override { return 0; }
    bool encode(int, bool force_idr, EncodedSlices& out) override {
        out.csd = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68};
        out.idr = force_idr;
        out.slices.assign(5, Bytes{0, 0, 0, 1, 0x65, 1, 2});
        return true;
    }
    void release(int) override {}
    std::string error() const override { return ""; }
};

struct Frame {
    uint64_t index;
    uint16_t seq;  // topic 10 seq of its first message
    bool intra;
    uint32_t stream;
    uint64_t conn;
};

// Frames the headset received after written[from]
std::vector<Frame> frames_since(size_t from) {
    std::lock_guard<std::mutex> l(g_hs.mu);
    std::vector<Frame> out;
    const auto& w = g_hs.written;
    for (size_t i = from; i + 1 < w.size(); ++i) {
        // Topic 10: segment table, then the EncodedSlice header (data section after its table)
        if (w[i].raw[1] != 10 || w[i + 1].raw[1] != 10 || w[i].raw.size() != 16) continue;
        const uint8_t* d = &w[i + 1].raw[8 + 8];
        out.push_back({rd64(d), rd16(&w[i].raw[4]), (d[49] & 1) != 0, w[i].stream, w[i].conn});
        ++i;
    }
    return out;
}

size_t mark() {
    std::lock_guard<std::mutex> l(g_hs.mu);
    return g_hs.written.size();
}

int count_since(size_t from, uint8_t topic) {
    std::lock_guard<std::mutex> l(g_hs.mu);
    int n = 0;
    for (size_t i = from; i < g_hs.written.size(); ++i) n += g_hs.written[i].raw[1] == topic;
    return n;
}

uint32_t headset_streams() {
    std::lock_guard<std::mutex> l(g_hs.mu);
    return g_hs.streams;
}

ql_session* g_s = nullptr;

ql_link_status link() {
    ql_link_status st{};
    ql_get_link_status(g_s, &st);
    return st;
}

// The game: a frame every 5 ms and 10 ms of audio every 10 ms, whatever the link does
struct Game {
    std::atomic<bool> run{true};
    std::thread t;
    uint64_t index = 0;
    Game() {
        t = std::thread([this] {
            int tex = 0;
            std::vector<float> audio(2 * 480, 0.25f);
            while (run) {
                ql_frame_submit f{};
                f.session = g_s;
                f.native_texture = &tex;
                f.render_pose = {0, 0, 0, 1, 0, 1.6f, 0, 0};
                f.frame_index = ++index;
                ql_get_render_event_func()(QL_EVENT_SUBMIT_FRAME, &f);
                if (index % 2 == 0) ql_submit_audio(g_s, audio.data(), 480, 48000);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    ~Game() {
        run = false;
        t.join();
    }
};
Game* g_game = nullptr;

// Back to streaming with new frames flowing; returns the frames since `from`
std::vector<Frame> recovered(size_t from, uint32_t connects_before) {
    bool ok = wait_for([&] { return link().state == QL_LINK_STREAMING && link().connects > connects_before; }, 8000);
    if (!ok) return {};
    size_t at = mark();
    wait_for([&] { return frames_since(at).size() >= 3; }, 3000);
    return frames_since(from);
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

// Texture backend for this test binary (USB: fake_headset.hpp)
namespace ql {
std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig&, std::string*) {
    return std::make_unique<FakeTextureEncoder>();
}
}  // namespace ql

TEST(recovery_connects) {
    // Short limits so the cases run fast (defaults: silence 3 s, resume window 500 ms)
    set_env("QL_SILENCE_MS", "400");
    set_env("QL_RESUME_MS", "300");
    set_env("QL_RESUME_TIMEOUT_MS", "300");
    g_s = ql_open();
    REQUIRE(g_s);
    CHECK_EQ(link().state, int32_t(QL_LINK_NO_DEVICE));
    g_game = new Game;
    {
        std::lock_guard<std::mutex> l(g_hs.mu);
        g_hs.present = true;
    }
    auto f = recovered(0, 0);
    REQUIRE(!f.empty());
    CHECK(f.front().intra);
    CHECK_EQ(f.front().index, uint64_t(1));
    CHECK_EQ(f.front().seq, uint16_t(0));
    ql_link_status st = link();
    CHECK_EQ(st.connects, uint32_t(1));
    CHECK_EQ(st.disconnects, uint32_t(0));
}

TEST(recovery_usb_glitch_resumes_stream) {
    size_t from;
    Frame before;
    uint32_t streams = headset_streams();
    ql_link_status st0 = link();
    {
        std::lock_guard<std::mutex> l(g_hs.mu);
        from = g_hs.written.size();
        g_hs.usb_glitch();
    }
    before = frames_since(0).back();
    auto f = recovered(from, st0.connects);
    REQUIRE(!f.empty());
    ql_link_status st = link();
    CHECK_EQ(st.disconnects, st0.disconnects + 1);
    CHECK_EQ(st.last_loss, int32_t(QL_LOSS_USB));
    CHECK_EQ(st.resumes, st0.resumes + 1);
    // Same headset stream, numbering continues (a frame the broken link swallowed leaves a gap),
    // the first frame on the new USB link is an IDR
    CHECK_EQ(headset_streams(), streams);
    CHECK(f.front().conn != before.conn);
    CHECK(f.front().intra);
    CHECK(f.front().index > before.index && f.front().index <= before.index + 3);
    CHECK(f.front().seq > before.seq && (f.front().seq - before.seq) % 4 == 0);
    for (size_t i = 1; i < f.size(); ++i) CHECK_EQ(f[i].index, f[i - 1].index + 1);
    CHECK(count_since(from, topic::kAudio) > 0);
}

namespace {

// A disturbance that ends the headset's stream: a new handshake, fresh numbering, IDR,
// hands on again, audio flowing, haptics reconnected
void check_new_stream(const char* what, int32_t loss, void (*disturb)(), int max_ms = 8000) {
    size_t from = mark();
    uint32_t streams = headset_streams();
    ql_link_status st0 = link();
    {
        std::lock_guard<std::mutex> l(g_hs.mu);
        disturb();
    }
    // Lost: no stale pose while the link is down
    REQUIRE(wait_for([&] { return link().disconnects > st0.disconnects; }, max_ms));
    if (link().state != QL_LINK_STREAMING) {
        ql_pose p;
        CHECK(ql_get_head_pose(g_s, &p) != 0 || link().state == QL_LINK_STREAMING);
    }
    auto f = recovered(from, st0.connects);
    if (f.empty()) std::printf("  %s: not recovered\n", what);
    REQUIRE(!f.empty());
    ql_link_status st = link();
    CHECK_EQ(st.disconnects, st0.disconnects + 1);
    CHECK_EQ(st.last_loss, loss);
    CHECK_EQ(st.resumes, st0.resumes);
    CHECK_EQ(headset_streams(), streams + 1);
    // Frames of the new stream: index from 1, seq from 0, an IDR first
    std::vector<Frame> fresh;
    for (const auto& x : f)
        if (x.stream == streams + 1) fresh.push_back(x);
    REQUIRE(!fresh.empty());
    CHECK(fresh.front().intra);
    CHECK_EQ(fresh.front().index, uint64_t(1));
    CHECK_EQ(fresh.front().seq, uint16_t(0));
    // Hands turned on and audio flowing in the new stream; a haptic goes out
    size_t at = mark();
    CHECK(count_since(from, topic::kInputControl) >= 2);
    CHECK_EQ(ql_haptic(g_s, 1, 0.5f, 0.05f), int(QL_OK));
    CHECK(wait_for([&] { return count_since(at, topic::kAudio) > 0 && count_since(at, topic::kRuntimeIpc) > 0; }));
    ql_pose p;
    CHECK(wait_for([&] { return ql_get_head_pose(g_s, &p) == 0; }));
}

}  // namespace

TEST(recovery_usb_drop_new_handshake) {
    // Link quit, cable pull, headset reboot: the USB function is gone longer than the resume window
    check_new_stream("usb drop", QL_LOSS_USB, [] { g_hs.usb_drop(800); });
}

TEST(recovery_headset_bye) {
    check_new_stream("headset bye", QL_LOSS_HEADSET_BYE, [] { g_hs.headset_bye(); });
}

TEST(recovery_new_hello_mid_stream) {
    check_new_stream("new hello", QL_LOSS_NEW_HELLO, [] { g_hs.new_hello(); });
}

TEST(recovery_silence) {
    // Headset asleep (proximity) with USB up: silence ends the stream, waking brings it back
    check_new_stream("silence", QL_LOSS_SILENCE, [] { g_hs.sleep_for(1200); });
}

TEST(recovery_glitch_then_drop_falls_back) {
    // USB back at once but the headset's stream is gone: the resume times out, BYE, new handshake
    size_t from = mark();
    uint32_t streams = headset_streams();
    ql_link_status st0 = link();
    {
        std::lock_guard<std::mutex> l(g_hs.mu);
        g_hs.usb_glitch();
        g_hs.phase = FakeHeadset::Phase::Idle;  // ... and the stream is gone
        g_hs.pending.clear();
    }
    auto f = recovered(from, st0.connects);
    REQUIRE(!f.empty());
    CHECK_EQ(headset_streams(), streams + 1);
    CHECK_EQ(link().resumes, st0.resumes);
}

TEST(recovery_soak) {
    // Random disturbances back to back, each must recover; QL_SOAK_CYCLES overrides the count
    const char* env = std::getenv("QL_SOAK_CYCLES");
    int cycles = env && *env ? std::atoi(env) : 12;
    std::mt19937 rng(1234);
    int recovered_n = 0;
    for (int c = 0; c < cycles; ++c) {
        int kind = int(rng() % 5);
        size_t from = mark();
        ql_link_status st0 = link();
        {
            std::lock_guard<std::mutex> l(g_hs.mu);
            switch (kind) {
                case 0: g_hs.usb_glitch(); break;
                case 1: g_hs.usb_drop(200 + int(rng() % 800)); break;
                case 2: g_hs.headset_bye(); break;
                case 3: g_hs.new_hello(); break;
                case 4: g_hs.sleep_for(500 + int(rng() % 700)); break;
            }
        }
        auto f = recovered(from, st0.connects);
        if (f.empty()) {
            std::printf("  soak cycle %d (kind %d): not recovered\n", c, kind);
            break;
        }
        // Within a stream and USB connection, frame index increases by one; each new one starts
        // with an IDR
        for (size_t i = 1; i < f.size(); ++i) {
            if (f[i].stream == f[i - 1].stream && f[i].conn == f[i - 1].conn) CHECK_EQ(f[i].index, f[i - 1].index + 1);
            else CHECK(f[i].intra);
        }
        ++recovered_n;
        // Random dwell while streaming before the next hit
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + rng() % 200));
    }
    CHECK_EQ(recovered_n, cycles);
    ql_link_status st = link();
    CHECK_EQ(st.state, int32_t(QL_LINK_STREAMING));
    CHECK(st.connects >= st.disconnects);

    delete g_game;
    ql_close(g_s);
    ql_shutdown();
}
