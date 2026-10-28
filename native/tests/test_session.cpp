// Handshake state machine: replay the Quest's side of Meta's recorded handshake and check
// that our PC messages are Meta's bytes (mirrors tests/test_handshake.py Transcript).
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
    CHECK_EQ(sent, setup.size());
    CHECK_EQ(r.s.state(), State::Streaming);
    CHECK_EQ(r.s.stream_seq(31), uint16_t(40));  // 48 setup msgs: 2 on 26, 4 on 2, 2 on 28, 40 on 31
}

TEST(step_timeout_fails) {
    Rig r;
    r.s.start();
    auto hello = entries("quest_to_pc")[0];
    r.s.on_message((*hello)["raw"].as_hex());
    CHECK_EQ(r.s.state(), State::Phase1);
    r.now += 2'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Phase1);
    r.now += 2'000'000'000;
    r.s.tick();
    CHECK_EQ(r.s.state(), State::Failed);
    CHECK_EQ(r.s.error(), std::string("no 0x83 in time"));
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
