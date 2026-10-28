// Audio (topic 6): block encoding byte-exact vs Meta, mic parsing, playback queue, pacing.
#include <cmath>
#include <cstring>

#include "audio.hpp"
#include "harness.hpp"
#include "session.hpp"

using namespace ql;

namespace {

// The recorded (segment table, segment) pair per direction
struct Recorded {
    Bytes table, seg;
};
Recorded recorded(const char* dir) {
    for (const auto& g : qltest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == 6 && g["dir"].as_string() == dir)
            return {g["samples"].items()[0]["raw"].as_hex(), g["samples"].items()[1]["raw"].as_hex()};
    return {};
}

std::span<const uint8_t> segment_of(const Bytes& raw) {
    return unpad_body(raw[0], std::span<const uint8_t>(raw).subspan(kHeaderSize));
}

Bytes raw_of(const StreamMessage& m, uint16_t seq) { return encode_message(m.flags, m.topic, seq, m.body); }

}  // namespace

TEST(audio_blocks_encode_like_meta) {
    for (const char* dir : {"pc_to_quest", "quest_to_pc"}) {
        Recorded r = recorded(dir);
        REQUIRE(!r.table.empty());
        uint64_t ts;
        std::span<const uint8_t> pcm;
        REQUIRE(audio::parse_block(segment_of(r.seg), ts, pcm));
        auto msgs = audio::encode_block(ts, pcm);
        REQUIRE(msgs.size() == 2);
        uint16_t seq = rd16(&r.table[4]);
        CHECK_EQ(raw_of(msgs[0], seq), r.table);
        CHECK_EQ(raw_of(msgs[1], uint16_t(seq + 1)), r.seg);
    }
    // 4096 B / 1024 B messages, as PROTOCOL_NOTES says
    CHECK_EQ(recorded("pc_to_quest").seg.size(), size_t(4096));
    CHECK_EQ(recorded("quest_to_pc").seg.size(), size_t(1024));
}

TEST(audio_playback_block_from_floats) {
    Recorded r = recorded("pc_to_quest");
    uint64_t ts;
    std::span<const uint8_t> pcm;
    REQUIRE(audio::parse_block(segment_of(r.seg), ts, pcm));
    REQUIRE(pcm.size() == audio::kBlockFrames * audio::kChannels * 4);
    std::vector<float> f(audio::kBlockFrames * audio::kChannels);
    std::memcpy(f.data(), pcm.data(), pcm.size());
    auto msgs = audio::encode_playback(ts, f);
    CHECK_EQ(raw_of(msgs[1], rd16(&r.seg[4])), r.seg);
}

TEST(audio_parse_rejects_table_and_other) {
    uint64_t ts;
    std::span<const uint8_t> pcm;
    Bytes table = recorded("quest_to_pc").table;
    CHECK(!audio::parse_block(segment_of(table), ts, pcm));
    Bytes junk(64, 0);
    CHECK(!audio::parse_block(junk, ts, pcm));
}

TEST(session_collects_mic_and_sends_audio) {
    int64_t now = 1'000'000'000;
    SessionConfig cfg;
    cfg.attach = true;  // straight to Streaming
    Session s(cfg, [&] { return now; });
    s.start();
    Recorded mic = recorded("quest_to_pc");
    s.on_message(mic.table);
    s.on_message(mic.seg);
    CHECK_EQ(s.mic_blocks(), uint64_t(1));
    uint64_t ts;
    std::span<const uint8_t> pcm;
    REQUIRE(audio::parse_block(segment_of(mic.seg), ts, pcm));
    int16_t out[1000];
    REQUIRE(s.read_mic(out, 1000) == audio::kBlockFrames);
    CHECK_EQ(out[0], int16_t(rd16(&pcm[0])));
    CHECK_EQ(out[479], int16_t(rd16(&pcm[958])));
    CHECK_EQ(s.read_mic(out, 1000), size_t(0));

    std::vector<float> block(audio::kBlockFrames * audio::kChannels, 0.5f);
    CHECK(s.submit_audio(std::span<const float>(block).first(10)) == Session::Submit::BadSlices);
    CHECK(s.submit_audio(block) == Session::Submit::Ok);
    CHECK(s.submit_audio(block, 42) == Session::Submit::Ok);
    auto out_xfers = s.take_outgoing();
    REQUIRE(out_xfers.size() == 2 && out_xfers[1].size() == 2);
    // Topic-6 seq += 2 per block; ts = our clock (or the caller's)
    CHECK_EQ(rd16(&out_xfers[0][0][4]), uint16_t(0));
    CHECK_EQ(rd16(&out_xfers[1][1][4]), uint16_t(3));
    CHECK_EQ(rd64(&out_xfers[0][1][8 + 8]), uint64_t(now));
    CHECK_EQ(rd64(&out_xfers[1][1][8 + 8]), uint64_t(42));
    CHECK_EQ(out_xfers[0][1].size(), size_t(4096));

    Session idle(SessionConfig{}, [&] { return now; });
    idle.start();
    CHECK(idle.submit_audio(block) == Session::Submit::NotStreaming);
}

TEST(audio_playback_buffer_primes_and_underflows) {
    audio::PlaybackBuffer b(4800, 1440);
    float in[2 * 1000], out[2 * 480];
    for (int i = 0; i < 1000; ++i) in[2 * i] = in[2 * i + 1] = float(i);
    b.write(in, 1000, 48000);
    // Below the 30 ms cushion: silence, nothing consumed
    CHECK_EQ(b.read(out, 480), size_t(0));
    CHECK_EQ(out[0], 0.0f);
    CHECK_EQ(b.buffered(), size_t(1000));
    b.write(in, 1000, 48000);
    CHECK_EQ(b.read(out, 480), size_t(480));
    CHECK_EQ(out[2 * 479 + 1], 479.0f);
    CHECK_EQ(b.read(out, 480), size_t(480));
    CHECK_EQ(out[0], 480.0f);
    CHECK_EQ(out[2 * 40], 520.0f);
    // 1040 left: two full blocks, then a partial one padded with silence
    CHECK_EQ(b.read(out, 480), size_t(480));
    CHECK_EQ(b.read(out, 480), size_t(480));
    CHECK_EQ(b.read(out, 480), size_t(80));
    CHECK_EQ(out[2 * 79], 999.0f);
    CHECK_EQ(out[2 * 80], 0.0f);
    // Ran dry: waits for the cushion again
    b.write(in, 480, 48000);
    CHECK_EQ(b.read(out, 480), size_t(0));
}

TEST(audio_playback_buffer_drops_oldest) {
    audio::PlaybackBuffer b(1000, 0);
    float in[2 * 1500], out[2 * 480];
    for (int i = 0; i < 1500; ++i) in[2 * i] = in[2 * i + 1] = float(i);
    b.write(in, 1500, 48000);
    CHECK_EQ(b.buffered(), size_t(1000));
    CHECK_EQ(b.dropped_frames(), uint64_t(500));
    CHECK_EQ(b.read(out, 480), size_t(480));
    CHECK_EQ(out[0], 500.0f);
    b.clear();
    CHECK_EQ(b.buffered(), size_t(0));
}

TEST(audio_playback_buffer_resamples) {
    // 24 kHz ramp -> 48 kHz: every other output frame is a midpoint
    audio::PlaybackBuffer b(48000, 0);
    std::vector<float> in(2 * 2400);
    for (int i = 0; i < 2400; ++i) {
        in[2 * i] = float(i);
        in[2 * i + 1] = -float(i);
    }
    b.write(in.data(), 1200, 24000);
    b.write(in.data() + 2 * 1200, 1200, 24000);  // phase carries across writes
    CHECK_EQ(b.buffered(), size_t(4798));  // the last input frame waits for its successor
    std::vector<float> out(2 * 4798);
    b.read(out.data(), 4798);
    bool ok = true;
    for (int i = 0; i < 4798; ++i) ok &= out[2 * size_t(i)] == float(i) / 2 && out[2 * size_t(i) + 1] == -float(i) / 2;
    CHECK(ok);

    // 44.1 kHz: 441 frames -> ~480, constant stays constant
    audio::PlaybackBuffer c(48000, 0);
    std::vector<float> ones(2 * 4410, 0.25f);
    c.write(ones.data(), 4410, 44100);
    CHECK(c.buffered() >= 4799 && c.buffered() <= 4801);
    std::vector<float> o(2 * 4800);
    c.read(o.data(), 4799);
    bool flat = true;
    for (size_t i = 0; i < 2 * 4799; ++i) flat &= std::abs(o[i] - 0.25f) < 1e-6f;
    CHECK(flat);
}

TEST(audio_mic_buffer_keeps_newest) {
    audio::MicBuffer m(4);
    int16_t in[6] = {1, 2, 3, 4, 5, 6}, out[8];
    m.write(in);
    CHECK_EQ(m.buffered(), size_t(4));
    CHECK_EQ(m.read(out, 3), size_t(3));
    CHECK_EQ(out[0], int16_t(3));
    CHECK_EQ(out[2], int16_t(5));
    CHECK_EQ(m.read(out, 8), size_t(1));
    CHECK_EQ(out[0], int16_t(6));
}

TEST(audio_pacer_fake_clock) {
    audio::Pacer p;  // 10 ms period, 50 ms max lag
    int64_t t = 5'000'000'000;
    CHECK_EQ(p.due(t), 1);  // first block right away
    CHECK_EQ(p.next_ns(), t + 10'000'000);
    CHECK_EQ(p.due(t + 5'000'000), 0);
    CHECK_EQ(p.due(t + 10'000'000), 1);
    // Woke late (coarse sleep): catch up, keep the grid
    CHECK_EQ(p.due(t + 35'000'000), 2);
    CHECK_EQ(p.next_ns(), t + 40'000'000);
    // 10 s of sending at exact wake-ups: 1000 blocks
    int sent = 0;
    for (int64_t now = t + 40'000'000; now < t + 40'000'000 + 10'000'000'000; now += 10'000'000) sent += p.due(now);
    CHECK_EQ(sent, 1000);
    CHECK_EQ(p.skipped(), uint64_t(0));
    // Stalled 200 ms (USB): skip the missed blocks instead of a burst
    int64_t stall = p.next_ns() + 200'000'000;
    CHECK_EQ(p.due(stall), 1);
    CHECK_EQ(p.skipped(), uint64_t(20));
    CHECK_EQ(p.next_ns(), stall + 10'000'000);
    p.reset();
    CHECK_EQ(p.due(1), 1);
    CHECK_EQ(p.next_ns(), int64_t(10'000'001));
}
