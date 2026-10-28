// Session API (what the Unity package calls) over the real runtime, with a fake Quest behind
// open_usb() and a fake texture encoder. The runtime is a process singleton, so this is one
// test walking through two Unity Play sessions.
#include <chrono>
#include <mutex>
#include <thread>

#include "harness.hpp"
#include "questlink.h"
#include "texture_encoder.hpp"
#include "transport.hpp"

using namespace ql;

namespace {

// Plays the Quest's side of the recorded session, records what we send
struct FakeQuest {
    std::mutex mu;
    Bytes rx;
    size_t rx_pos = 0;
    std::vector<Bytes> written;
};
FakeQuest g_quest;
bool g_quest_plugged = false;

class FakeTransport final : public Transport {
public:
    bool write(const uint8_t* data, size_t len, int) override {
        std::lock_guard<std::mutex> l(g_quest.mu);
        g_quest.written.emplace_back(data, data + len);
        return true;
    }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        {
            std::lock_guard<std::mutex> l(g_quest.mu);
            size_t n = std::min(cap, g_quest.rx.size() - g_quest.rx_pos);
            if (n) {
                std::copy(g_quest.rx.begin() + g_quest.rx_pos, g_quest.rx.begin() + g_quest.rx_pos + n, buf);
                g_quest.rx_pos += n;
                return long(n);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    }
};

// "Texture" = pointer to an int; the encoded slice carries its value
class FakeTextureEncoder final : public TextureEncoder {
public:
    int capture(void* tex) override {
        std::lock_guard<std::mutex> l(mu_);
        slots_.push_back(*static_cast<int*>(tex));
        return int(slots_.size() - 1);
    }
    bool encode(int slot, bool force_idr, EncodedSlices& out) override {
        std::lock_guard<std::mutex> l(mu_);
        out.csd = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68};
        out.idr = force_idr;
        out.slices.clear();
        for (int i = 0; i < 5; ++i) out.slices.push_back({0, 0, 0, 1, 0x65, uint8_t(slots_[size_t(slot)]), uint8_t(i)});
        return true;
    }
    void release(int) override {}
    std::string error() const override { return ""; }

private:
    std::mutex mu_;
    std::vector<int> slots_;
};

struct SentFrame {
    uint64_t index;
    int texture_value;
    float qw;
    bool intra;
};

// Frames in what we wrote so far: per transfer, topic 10's header and video messages
std::vector<SentFrame> sent_frames() {
    std::lock_guard<std::mutex> l(g_quest.mu);
    std::vector<SentFrame> out;
    for (const auto& x : g_quest.written) {
        if (x.size() < 16 || x[1] != 10) continue;
        std::vector<Bytes> msgs;
        for (size_t i = 0; i + 8 <= x.size();) {
            size_t size = 4 + size_t(rd16(&x[i + 2])) * 4;
            msgs.emplace_back(x.begin() + i, x.begin() + i + size);
            i += size;
        }
        const uint8_t* d = &msgs[1][8 + 8];  // EncodedSlice data section
        float qw;
        std::memcpy(&qw, d + 12 + 12, 4);
        out.push_back({rd64(d), msgs[3][8 + 5], qw, (d[49] & 1) != 0});
    }
    return out;
}

// Topic-6 blocks we sent; loud = PCM starts with 0.25 (else silence)
int audio_blocks(bool loud) {
    std::lock_guard<std::mutex> l(g_quest.mu);
    int n = 0;
    for (const auto& x : g_quest.written)
        for (size_t i = 0; i + 8 <= x.size();) {
            size_t size = 4 + size_t(rd16(&x[i + 2])) * 4;
            if (x[i + 1] == 6 && size == 4096) {
                float f;
                std::memcpy(&f, &x[i + 8 + 24], 4);
                n += loud ? f == 0.25f : f == 0.0f;
            }
            i += size;
        }
    return n;
}

template <class F>
bool wait_for(F cond, int ms = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

void submit(ql_session* s, int* tex, uint64_t index, float qw) {
    ql_frame_submit f{};
    f.session = s;
    f.native_texture = tex;
    f.render_pose = {0, 0, 0, qw, 0, 1.6f, 0, 0};
    f.frame_index = index;
    ql_get_render_event_func()(QL_EVENT_SUBMIT_FRAME, &f);
}

}  // namespace

// Platform backends for this test binary
namespace ql {
std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    std::lock_guard<std::mutex> l(g_quest.mu);
    if (!g_quest_plugged) {
        err = OpenError::NotFound;
        detail = "fake: not plugged";
        return nullptr;
    }
    g_quest_plugged = false;  // one connection
    err = OpenError::None;
    return std::make_unique<FakeTransport>();
}
std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig&, std::string*) {
    return std::make_unique<FakeTextureEncoder>();
}
}  // namespace ql

TEST(runtime_unity_flow) {
    // Before the Quest is plugged in: a session is idle
    ql_session* s1 = ql_open();
    REQUIRE(s1);
    CHECK_EQ(ql_poll(s1), 0);
    ql_pose pose;
    CHECK(ql_get_head_pose(s1, &pose) != 0);
    ql_device_info dev;
    CHECK(ql_get_device_info(s1, &dev) != 0);

    {
        std::lock_guard<std::mutex> l(g_quest.mu);
        auto add = [](const Bytes& raw) {
            Bytes x = pack_transfer(std::vector<Bytes>{raw});
            g_quest.rx.insert(g_quest.rx.end(), x.begin(), x.end());
        };
        for (const auto& e : qltest::fixture("handshake", "control").items())
            if (e["dir"].as_string() == "quest_to_pc") add(e["raw"].as_hex());
        for (const auto& g : qltest::fixture("handshake", "streams").items())
            if (g["topic"].as_u64() == 34 || (g["topic"].as_u64() == 6 && g["dir"].as_string() == "quest_to_pc"))
                for (const auto& s : g["samples"].items()) add(s["raw"].as_hex());
        g_quest_plugged = true;
    }
    // Handshake, then ~0.7 s of paced stream setup
    REQUIRE(wait_for([&] { return ql_poll(s1) == 2; }));
    REQUIRE(ql_get_device_info(s1, &dev) == 0);
    CHECK_EQ(dev.encode_width, uint32_t(4096));
    REQUIRE(wait_for([&] { return ql_get_head_pose(s1, &pose) == 0; }));
    CHECK_EQ(pose.time_ns, uint64_t(344814707924000));

    // Play session 1: frames 1, 1 (duplicate, dropped), 2
    int tex[] = {11, 99, 12, 13, 14};
    submit(s1, &tex[0], 1, 0.5f);
    submit(s1, &tex[1], 1, 0.5f);
    submit(s1, &tex[2], 2, 0.25f);
    REQUIRE(wait_for([] { return sent_frames().size() == 2; }));

    // Exit Play mode with a submit event still queued for the closed session: ignored
    ql_close(s1);
    submit(s1, &tex[3], 3, 1.0f);
    CHECK_EQ(ql_poll(s1), 0);

    // Play session 2: Unity's frame_index restarts at 1, the runtime and headset session stay up
    ql_session* s2 = ql_open();
    REQUIRE(s2 && s2 != s1);
    CHECK_EQ(ql_poll(s2), 2);
    submit(s2, &tex[4], 1, 0.75f);
    REQUIRE(wait_for([] { return sent_frames().size() == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto frames = sent_frames();
    REQUIRE(frames.size() == 3);
    CHECK_EQ(frames[0].texture_value, 11);
    CHECK_EQ(frames[1].texture_value, 12);
    CHECK_EQ(frames[2].texture_value, 14);
    // Session-wide frame.index across both Play sessions
    CHECK_EQ(frames[0].index, uint64_t(1));
    CHECK_EQ(frames[1].index, uint64_t(2));
    CHECK_EQ(frames[2].index, uint64_t(3));
    CHECK_EQ(frames[0].qw, 0.5f);  // render pose written into the frame
    CHECK_EQ(frames[2].qw, 0.75f);
    CHECK(frames[0].intra);         // first frame of the connection forced to IDR
    CHECK(!frames[1].intra);

    // Audio: a 10 ms block every 10 ms while streaming (silence so far), game audio when queued
    REQUIRE(wait_for([] { return audio_blocks(false) >= 5; }));
    std::vector<float> tone(2 * 4410, 0.25f);  // 100 ms at 44.1 kHz
    CHECK_EQ(ql_submit_audio(s2, tone.data(), 4410, 44100), int(QL_OK));
    CHECK_EQ(ql_submit_audio(s1, tone.data(), 4410, 44100), int(QL_ERR_ARGUMENT));  // closed
    REQUIRE(wait_for([] { return audio_blocks(true) >= 3; }));
    // Mic: the recorded Quest block
    int16_t mic[2000];
    CHECK_EQ(ql_read_mic(s2, mic, 2000), int(480));
    CHECK_EQ(ql_read_mic(s2, mic, 2000), 0);

    ql_controller c;
    CHECK_EQ(ql_get_controller(s2, 0, &c), int(QL_ERR_UNSUPPORTED));
    CHECK_EQ(c.flags, uint32_t(0));
    CHECK_EQ(ql_haptic(s2, 1, 0.5f, 0.1f), int(QL_ERR_UNSUPPORTED));
    ql_close(s2);
}
