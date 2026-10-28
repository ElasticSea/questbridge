// Handshake state machine: replay the Quest's side of Meta's recorded handshake and check
// that our PC messages are Meta's bytes (mirrors tests/test_handshake.py Transcript).
#include <cmath>
#include <stdexcept>

#include "harness.hpp"
#include "session.hpp"
#include "templates.hpp"

using namespace ql;

namespace {

struct Rig {
    int64_t now = 1'000'000'000;
    std::vector<std::string> events;
    Session s;

    explicit Rig(SessionConfig cfg = {})
        : s(cfg, [this] { return now; }, [this](const char* dir, const std::string& what, int) {
              events.push_back(std::string(dir) + " " + what);
          }) {}

    // Outgoing control messages (topic 1), raw
    std::vector<Bytes> take_control() {
        std::vector<Bytes> out;
        for (auto& xfer : s.take_outgoing())
            for (auto& m : xfer)
                if (m[1] == topic::kControl) out.push_back(m);
        return out;
    }
};

std::vector<const json::Value*> entries(const char* dir) {
    std::vector<const json::Value*> out;
    for (const auto& e : qltest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == dir) out.push_back(&e);
    return out;
}

bool is_clock(const Bytes& raw) { return raw.size() >= 16 && raw[9] == control::kClockFlags; }

}  // namespace

TEST(templates_match_fixtures) {
    // Control templates = first PC message per (type, version), Cap'n Proto bytes as sent
    size_t found = 0;
    for (const auto& t : templates::controls()) {
        for (const auto* e : entries("pc_to_quest")) {
            if (!e->has("capnp") || (*e)["inner"]["type"].as_u64() != t.type) continue;
            if (((*e)["capnp"]["tree"]["s"][0].as_u64() & 0xFFFF) != t.version) continue;
            Bytes raw = (*e)["raw"].as_hex();
            CHECK_EQ(Bytes(t.capnp.begin(), t.capnp.end()), Bytes(raw.begin() + 16, raw.end()));
            CHECK_EQ(t.inner_flags, raw[9]);
            ++found;
            break;
        }
    }
    CHECK_EQ(found, size_t(6));

    auto clip = json::parse_file(qltest::repo("host/replay/meta-idr.json"));
    const auto& setup = templates::stream_setup();
    REQUIRE(setup.size() == clip["setup"].size());
    for (size_t i = 0; i < setup.size(); ++i) {
        const auto& m = clip["setup"][i];
        CHECK_EQ(setup[i].topic, uint8_t(m["topic"].as_u64()));
        CHECK_EQ(setup[i].flags, uint8_t(m["flags"].as_u64()));
        CHECK_EQ(Bytes(setup[i].body.begin(), setup[i].body.end()), m["body"].as_hex());
    }
    const auto& frame = clip["frame"];
    for (const auto& st : templates::slice_headers()) {
        bool seen = false;
        for (size_t i = 1; i < frame.size(); ++i) {
            if (frame[i]["topic"].as_u64() != st.topic || frame[i - 1]["topic"].as_u64() != st.topic) continue;
            Bytes table = frame[i - 1]["body"].as_hex();
            if (table.size() != 8) continue;
            CHECK_EQ(Bytes(st.table.begin(), st.table.end()), table);
            CHECK_EQ(Bytes(st.header.begin(), st.header.end()), frame[i]["body"].as_hex());
            seen = true;
            break;
        }
        CHECK(seen);
    }
    CHECK_EQ(templates::slice_headers().size(), size_t(5));
}

TEST(handshake_reproduces_meta_bytes) {
    Rig r;
    r.s.start();
    std::vector<Bytes> ours;
    std::vector<Bytes> pings;  // Quest clock pings we were fed, in order
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (is_clock(raw) && raw[8] == control::kClockPing) pings.push_back(raw);
        r.now += 1'000'000;
        r.s.on_message(raw);
        r.s.tick();
        for (auto& m : r.take_control()) ours.push_back(m);
    }
    CHECK_EQ(r.s.state(), State::Setup);
    CHECK_EQ(r.s.session_id(), uint64_t(6936518381556450372ull));

    // Meta's six handshake messages, byte for byte (session id patched from the HELLO)
    std::vector<Bytes> meta;
    for (const auto* e : entries("pc_to_quest")) meta.push_back((*e)["raw"].as_hex());
    REQUIRE(ours.size() >= 7 && meta.size() >= 7);
    for (size_t i = 0; i < 6; ++i) CHECK_EQ(ours[i], meta[i]);

    // First clock ping: same header, inner header and seq; only the timestamps differ
    CHECK(is_clock(meta[6]) && meta[6][8] == control::kClockPing);
    CHECK_EQ(Bytes(ours[6].begin(), ours[6].begin() + 16), Bytes(meta[6].begin(), meta[6].begin() + 16));
    CHECK_EQ(ours[6].size(), meta[6].size());

    // Every Quest ping gets a pong echoing its transmit time; control seq and clock counter run on
    size_t pong = 0;
    for (size_t i = 7; i < ours.size(); ++i) {
        const Bytes& m = ours[i];
        CHECK_EQ(rd16(&m[4]), uint16_t(3 + (i - 6)));
        CHECK_EQ(m[8], control::kClockPong);
        CHECK_EQ(rd32(&m[12]), uint32_t(1 + (i - 6)));
        REQUIRE(pong < pings.size());
        CHECK_EQ(rd64(&m[16]), rd64(&pings[pong][16 + 16]));
        ++pong;
    }
    CHECK_EQ(pong, pings.size());
    CHECK(r.s.quest_clock_offset_ns().has_value());
}

TEST(handshake_transcript_order) {
    Rig r;
    r.s.start();
    std::vector<State> states = {r.s.state()};
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (is_clock(raw)) break;
        r.s.on_message(raw);
        if (states.back() != r.s.state()) states.push_back(r.s.state());
    }
    std::vector<State> expected = {State::WaitHello, State::Phase1, State::WaitLinkRequest, State::WaitHelloV3,
                                   State::Phase2, State::Setup};
    CHECK(states == expected);
}

TEST(setup_paced_with_capped_gaps) {
    Rig r;
    r.s.start();
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (is_clock(raw)) break;
        r.s.on_message(raw);
    }
    REQUIRE(r.s.state() == State::Setup);
    size_t sent = 0;
    auto count = [&] {
        for (auto& x : r.s.take_outgoing())
            for (auto& m : x)
                if (m[1] != topic::kControl) ++sent;
    };
    count();
    const auto& setup = templates::stream_setup();
    CHECK(sent >= 1 && sent < setup.size());
    // Recorded gaps sum to ~2 s, but each is capped at 0.5 s
    for (int i = 0; i < 100 && r.s.state() == State::Setup; ++i) {
        r.now += 10'000'000;
        r.s.tick();
        count();
    }
    CHECK_EQ(sent, setup.size() + 2 + 8);  // + InputControl hands on, HapticsServer lookup + connect
    CHECK_EQ(r.s.state(), State::Streaming);
    CHECK_EQ(r.s.stream_seq(31), uint16_t(48));  // 48 setup msgs: 2 on 26, 4 on 2, 2 on 28, 40 on 31; + 8
    CHECK_EQ(r.s.stream_seq(topic::kInputControl), uint16_t(4));  // Meta's hands off, then our hands on
}

TEST(step_timeout_restarts_with_bye) {
    Rig r;
    r.s.start();
    auto hello = entries("quest_to_pc")[0];
    r.s.on_message((*hello)["raw"].as_hex());
    CHECK_EQ(r.s.state(), State::Phase1);
    r.take_control();
    r.now += 2'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Phase1);
    r.now += 2'000'000'000;
    r.s.tick();
    // A stalled headset step is not fatal: back to WaitHello with a BYE right away
    CHECK_EQ(r.s.state(), State::WaitHello);
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kBye);
}

namespace {

// Feed the recorded Quest side up to the first clock ping, then pace the setup to Streaming
void to_streaming(Rig& r) {
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (is_clock(raw)) break;
        r.now += 1'000'000;
        r.s.on_message(raw);
        r.s.tick();
    }
    for (int i = 0; i < 200 && r.s.state() == State::Setup; ++i) {
        r.now += 10'000'000;
        r.s.tick();
    }
    r.s.take_outgoing();
}

Bytes quest_msg(uint8_t type, int version) {
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (raw.size() > 16 && raw[8] == type && (version < 0 || rd16(&raw[0x20]) == version)) return raw;
    }
    throw std::runtime_error("no recorded Quest message");
}

}  // namespace

TEST(preluding_bye_when_no_hello) {
    Rig r;
    r.s.start();
    r.now += 10'000'000;
    r.s.tick();
    CHECK(r.take_control().empty());  // Meta waits 50 ms for an INVITE first
    r.now += 50'000'000;
    r.s.tick();
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    // All-zero Bye root (version 0, session id 0 = any session), like Meta's preluding BYE
    Bytes bye = {0x10, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x8c, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    bye.resize(bye.size() + 16, 0);
    CHECK_EQ(out[0], bye);
    CHECK_EQ(r.s.byes_sent(), uint32_t(1));
    r.now += 2'900'000'000;
    r.s.tick();
    CHECK(r.take_control().empty());
    r.now += 200'000'000;
    r.s.tick();
    CHECK_EQ(r.take_control().size(), size_t(1));  // retried after 3 s

    // The HELLO that follows starts phase 1 with control seq 0, as Meta's CONFIG
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.state(), State::Phase1);
    out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0], (*entries("pc_to_quest")[0])["raw"].as_hex());
}

TEST(bye_disabled) {
    SessionConfig cfg;
    cfg.bye_delay_ns = -1;
    Rig r(cfg);
    r.s.start();
    r.now += 10'000'000'000;
    r.s.tick();
    CHECK(r.take_control().empty());
}

TEST(hello_v1_restarts_a_streaming_session) {
    // Link quit in the headset: xrspd sends a new HELLO v1 right away
    Rig r;
    r.s.start();
    to_streaming(r);
    REQUIRE(r.s.state() == State::Streaming);
    CHECK_EQ(r.s.streams(), uint32_t(1));
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.state(), State::Phase1);
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0], (*entries("pc_to_quest")[0])["raw"].as_hex());

    // ... and the user's Launch brings a second stream with fresh numbering
    to_streaming(r);
    CHECK_EQ(r.s.state(), State::Streaming);
    CHECK_EQ(r.s.streams(), uint32_t(2));
    CHECK_EQ(r.s.stream_seq(31), uint16_t(48));  // setup + HapticsServer lookup and connect again
    CHECK_EQ(r.s.next_frame_index(), uint64_t(1));
}

TEST(hello_v3_while_waiting_for_v1) {
    // A new host while the headset sits in its phase-1 session: Launch sends BYE + HELLO v3
    Rig r;
    r.s.start();
    r.s.on_message(quest_msg(control::kBye, 1));
    CHECK_EQ(r.s.state(), State::WaitHello);
    r.s.on_message(quest_msg(control::kHello, 3));
    CHECK_EQ(r.s.state(), State::Phase2);
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kConfig);
    CHECK_EQ(rd16(&out[0][4]), uint16_t(0));
}

TEST(headset_bye_ends_stream) {
    Rig r;
    r.s.start();
    to_streaming(r);
    REQUIRE(r.s.state() == State::Streaming);
    r.s.on_message(quest_msg(control::kBye, 1));
    CHECK_EQ(r.s.state(), State::WaitHello);
    r.now += 1'000'000'000;
    r.s.tick();
    CHECK(r.take_control().empty());  // give its own HELLO a chance first
    r.now += 2'100'000'000;
    r.s.tick();
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kBye);
}

TEST(attach_continues_numbering) {
    SessionConfig cfg;
    cfg.attach = true;
    cfg.attach_control_seq = 20;
    cfg.attach_video_seq = 500;
    Rig r(cfg);
    r.s.start();
    CHECK_EQ(r.s.state(), State::Streaming);
    CHECK_EQ(r.s.control_seq(), uint16_t(20));
    CHECK_EQ(r.s.stream_seq(12), uint16_t(500));
    CHECK(r.s.take_outgoing().empty());
}

// --- recovery inside a running session ---------------------------------------

namespace {

// The recorded head pose (segment table, then the segment)
void feed_pose(Session& s) {
    for (const auto& g : qltest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == topic::kHeadPose)
            for (const auto& m : g["samples"].items()) s.on_message(m["raw"].as_hex());
}

// Streaming with a clock offset (one recorded ping) and a head pose
void to_live_stream(Rig& r) {
    to_streaming(r);
    for (const auto* e : entries("quest_to_pc")) {
        Bytes raw = (*e)["raw"].as_hex();
        if (is_clock(raw) && raw[8] == control::kClockPing) {
            r.s.on_message(raw);
            break;
        }
    }
    feed_pose(r.s);
    r.s.take_outgoing();
}

// One 5-slice frame; returns (frame index, topic 10 seq) of what went out
std::pair<uint64_t, uint16_t> send_frame(Rig& r) {
    static const Bytes csd = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68}, nal = {0, 0, 0, 1, 0x65, 1, 2, 3};
    std::vector<SliceData> slices(5, SliceData{csd, nal});
    uint16_t seq = r.s.stream_seq(topic::kVideoFirst);
    if (r.s.submit_frame(slices, {}) != Session::Submit::Ok) throw std::runtime_error("frame not sent");
    r.s.take_outgoing();
    return {r.s.next_frame_index() - 1, seq};
}

}  // namespace

TEST(silence_ends_stream) {
    Rig r;
    r.s.start();
    to_live_stream(r);
    REQUIRE(r.s.head_pose().has_value());
    r.now += 2'900'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Streaming);
    feed_pose(r.s);  // anything from the Quest keeps it alive
    r.now += 2'900'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Streaming);
    r.now += 200'000'000;
    r.s.tick();
    // Headset asleep with USB up, or its xrspd gone: BYE now, so a living one sends HELLO
    CHECK_EQ(r.s.state(), State::WaitHello);
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kBye);
    CHECK_EQ(r.s.losses(), uint32_t(1));
    CHECK(r.s.last_loss() == Loss::Silence);
    CHECK(!r.s.head_pose().has_value());  // no stale pose while reconnecting
    // Not while waiting for HELLO or Launch: only BYE retries there
    r.now += 10'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.losses(), uint32_t(1));
}

TEST(silence_disabled) {
    SessionConfig cfg;
    cfg.silence_timeout_ns = 0;
    Rig r(cfg);
    r.s.start();
    to_live_stream(r);
    r.now += 60'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Streaming);
}

TEST(stream_losses_have_reasons) {
    Rig r;
    r.s.start();
    to_live_stream(r);
    r.s.on_message(quest_msg(control::kBye, 1));
    CHECK_EQ(r.s.losses(), uint32_t(1));
    CHECK(r.s.last_loss() == Loss::HeadsetBye);
    CHECK(!r.s.head_pose().has_value());
    to_live_stream(r);
    REQUIRE(r.s.state() == State::Streaming);
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.losses(), uint32_t(2));
    CHECK(r.s.last_loss() == Loss::NewHello);
    // A restart before the stream was up is no lost stream
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.losses(), uint32_t(2));
}

TEST(resume_continues_numbering) {
    Rig r;
    r.s.start();
    to_live_stream(r);
    auto [index, seq] = send_frame(r);
    uint16_t control = r.s.control_seq(), audio = r.s.stream_seq(topic::kAudio);

    // USB reopened: no BYE, no handshake, the stream goes on where it was
    r.s.resume();
    CHECK(r.s.resuming());
    CHECK_EQ(r.s.state(), State::Streaming);
    CHECK(r.s.take_outgoing().empty());
    CHECK(!r.s.head_pose().has_value());
    r.now += 300'000'000;
    feed_pose(r.s);
    CHECK(!r.s.resuming());
    CHECK_EQ(r.s.resumes(), uint32_t(1));
    CHECK_EQ(r.s.losses(), uint32_t(0));
    auto [index2, seq2] = send_frame(r);
    CHECK_EQ(index2, index + 1);
    CHECK_EQ(seq2, uint16_t(seq + 4));  // table, header, csd, video on each slice topic
    CHECK_EQ(r.s.control_seq(), control);
    CHECK_EQ(r.s.stream_seq(topic::kAudio), audio);
    r.now += 1'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Streaming);
}

TEST(resume_times_out_to_new_handshake) {
    Rig r;
    r.s.start();
    to_live_stream(r);
    r.s.resume();
    r.now += 900'000'000;
    r.s.tick();
    CHECK(r.take_control().empty());
    r.now += 200'000'000;
    r.s.tick();
    // The headset dropped the stream with its USB function: BYE, and its HELLO starts over
    CHECK_EQ(r.s.state(), State::WaitHello);
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kBye);
    CHECK_EQ(r.s.resumes(), uint32_t(0));
    CHECK_EQ(r.s.losses(), uint32_t(0));  // the USB error was the loss (the runtime counts it)
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.state(), State::Phase1);
}

TEST(resume_hello_starts_over) {
    // Link quit: the reopened USB carries the headset's new INVITE, not the old stream
    Rig r;
    r.s.start();
    to_live_stream(r);
    r.s.resume();
    r.s.on_message(quest_msg(control::kHello, 1));
    CHECK_EQ(r.s.state(), State::Phase1);
    CHECK(!r.s.resuming());
    CHECK_EQ(r.s.resumes(), uint32_t(0));
    CHECK_EQ(r.s.losses(), uint32_t(0));
}

TEST(resume_during_handshake_starts_over) {
    Rig r;
    r.s.start();
    r.s.on_message(quest_msg(control::kHello, 1));
    REQUIRE(r.s.state() == State::Phase1);
    r.take_control();
    r.s.resume();
    CHECK_EQ(r.s.state(), State::WaitHello);
    CHECK(!r.s.resuming());
    r.now += 60'000'000;
    r.s.tick();
    auto out = r.take_control();
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0][8], control::kBye);
}
