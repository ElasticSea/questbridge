// Touch controllers (topics 34/35) and haptics (RuntimeIPC on topic 31) from the SensorProbe
// capture (tests/fixtures/sensors/controllers.json, haptics.json); mirrors tests/test_controllers.py.
#include <cmath>

#include "controllers.hpp"
#include "harness.hpp"
#include "pose.hpp"
#include "session.hpp"

using namespace ql;

namespace {

// Labelled Cap'n Proto message (segment table + segment) -> the segment
Bytes segment(const char* label) {
    for (const auto& m : qltest::fixture("sensors", "controllers").items())
        if (m["label"].as_string() == label) {
            Bytes b = m["capnp"].as_hex();
            return Bytes(b.begin() + 8, b.end());
        }
    return {};
}

const ControllerInput& input(const char* label, int index) {
    static std::map<std::string, DeviceInput> cache;
    auto it = cache.find(label);
    if (it == cache.end()) it = cache.emplace(label, *parse_device_input(segment(label))).first;
    return it->second.controllers[size_t(index)];
}

const json::Value& haptic_frame(const char* label) {
    for (const auto& f : qltest::fixture("sensors", "haptics").items())
        if (f["label"].as_string() == label) return f;
    throw std::runtime_error(label);
}

// Our frame encoded with the captured XRSP seqs must equal Meta's raw messages
void check_frame(const json::Value& f, const std::vector<StreamMessage>& msgs) {
    const auto& raw = f["raw"].items();
    REQUIRE(raw.size() == msgs.size());
    for (size_t i = 0; i < msgs.size(); ++i) {
        Bytes want = raw[i].as_hex();
        CHECK_EQ(encode_message(msgs[i].flags, msgs[i].topic, rd16(&want[4]), msgs[i].body), want);
    }
}

bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }

}  // namespace

TEST(controller_poses_from_topic34) {
    Bytes seg = segment("pose_both_tracked");
    CHECK_EQ(seg.size(), size_t(520));
    auto p = parse_pose2(seg);
    REQUIRE(p);
    // Head pose still found when the controller list comes first (decode_controllers.py dump)
    CHECK(near(p->head.orientation[0], -0.106193f) && near(p->head.orientation[3], -0.5227f));
    CHECK(near(p->head.position[2], 0.527593f));
    CHECK_EQ(p->head.sample_time_ns, uint64_t(0x0001441716d97020));
    CHECK_EQ(p->head.vsync_ns, uint64_t(0x0001441716b3010a));
    CHECK_EQ(p->head.send_time_ns, uint64_t(0x0001441717177559));
    const auto& l = p->controllers[0];
    const auto& r = p->controllers[1];
    REQUIRE(l.present && l.has_pose && r.present && r.has_pose);
    CHECK_EQ(l.status, uint64_t(0xF));
    CHECK_EQ(r.status, uint64_t(0xF));
    CHECK(near(l.orientation[0], 0.358928f) && near(l.orientation[3], 0.594633f));
    CHECK(near(l.position[0], 0.020983f) && near(l.position[2], 0.3126f));
    CHECK_EQ(l.sample_time_ns, uint64_t(0x0001441716a880c8));
    CHECK(near(r.orientation[1], -0.596684f) && near(r.position[2], 0.645326f));
    CHECK_EQ(r.sample_time_ns, uint64_t(0x0001441716a04368));
    for (const auto* c : {&l, &r}) {
        double n = 0;
        for (float q : c->orientation) n += double(q) * q;
        CHECK(std::abs(n - 1.0) < 1e-4);
    }
}

TEST(controller_poses_lost_when_put_down) {
    auto p = parse_pose2(segment("pose_controllers_lost"));
    REQUIRE(p);
    CHECK(p->controllers[0].present && p->controllers[1].present);
    CHECK_EQ(p->controllers[0].status, uint64_t(0));
    CHECK_EQ(p->controllers[1].status, uint64_t(0));
}

TEST(controller_buttons_from_topic35) {
    struct Case {
        const char* label;
        int index;
        uint32_t buttons;
    };
    const Case cases[] = {{"a", 1, 0x1},           {"b", 1, 0x2},
                          {"r_stick_click", 1, 0x80000004}, {"x", 0, 0x100},
                          {"y", 0, 0x200},         {"l_stick_click", 0, 0x80000400},
                          {"menu", 0, 0x40000000}};
    for (const auto& c : cases) {
        const auto& e = input(c.label, c.index);
        CHECK(e.present);
        CHECK_EQ(e.buttons, c.buttons);
        CHECK_EQ(e.battery, uint8_t(100));
    }
    auto d = parse_device_input(segment("a"));
    REQUIRE(d);
    CHECK_EQ(d->flags, uint8_t(0x3));
    CHECK_EQ(d->controller_mask, uint16_t(3));
    CHECK(std::abs(d->ipd - 0.062f) < 0.004f);
}

TEST(controller_analog_from_topic35) {
    for (int i = 0; i < 2; ++i) {
        std::string s = i ? "r_" : "l_";
        CHECK(input((s + "trigger").c_str(), i).trigger > 0.99f);
        CHECK(input((s + "grip").c_str(), i).grip > 0.99f);
        CHECK(input((s + "stick_x_max").c_str(), i).stick[0] > 0.9f);
        CHECK(input((s + "stick_x_min").c_str(), i).stick[0] < -0.9f);
        CHECK(input((s + "stick_y_max").c_str(), i).stick[1] > 0.9f);
        CHECK(input((s + "stick_y_min").c_str(), i).stick[1] < -0.9f);
    }
}

TEST(controllers_absent_in_hands_mode) {
    auto d = parse_device_input(segment("hands_mode"));
    REQUIRE(d);
    CHECK_EQ(d->flags, uint8_t(0x7));
    CHECK_EQ(d->controller_mask, uint16_t(0));
    CHECK(!d->controllers[0].present && !d->controllers[1].present);
}

TEST(haptics_frames_match_meta) {
    const auto& lookup = haptic_frame("lookup");
    check_frame(lookup, ripc::lookup(lookup["header"]["client"].as_u64()));
    const auto& connect = haptic_frame("connect_haptics");
    CHECK_EQ(connect["header"]["client"].as_u64(), ripc::kHapticsClient);
    check_frame(connect, ripc::connect_haptics(ripc::kHapticsClient, uint32_t(connect["header"]["seq"].as_u64())));
    // xrApplyHapticFeedback(amplitude 1, 1 s) on the left (1), then the right (2) controller
    for (int n = 0; n < 2; ++n) {
        const auto& f = haptic_frame(n ? "haptic_1" : "haptic_0");
        check_frame(f, ripc::haptic(ripc::kHapticsClient, uint32_t(f["header"]["seq"].as_u64()), uint8_t(n + 1),
                                    1'000'000'000, 1.0f));
    }
}

TEST(session_controllers_and_haptics) {
    int64_t now = 0;
    SessionConfig cfg;
    cfg.attach = true;
    Session s(cfg, [&] { return now; });
    CHECK_EQ(s.haptic(0, 1.0f, 1000), Session::Submit::NotStreaming);
    s.start();
    CHECK_EQ(s.controller_count(), uint64_t(0));
    for (const char* label : {"pose_both_tracked", "a"}) {
        Bytes seg = segment(label);
        uint8_t t = std::string(label) == "a" ? topic::kDeviceInput : topic::kHeadPose;
        Bytes table = {0, 0, 0, 0, uint8_t(seg.size() / 8), 0, 0, 0};
        s.on_message(encode_message(kFlagsNormal, t, 0, table));
        s.on_message(encode_message(kFlagsNormal, t, 1, seg));
    }
    CHECK_EQ(s.controller_count(), uint64_t(2));
    CHECK_EQ(s.head_pose_count(), uint64_t(1));
    CHECK(s.controller_poses()[1].present);
    REQUIRE(s.device_input());
    CHECK_EQ(s.device_input()->controllers[1].buttons, uint32_t(0x1));

    // Attach skips the setup, so the first haptic connects first: lookup, connect, call
    s.take_outgoing();
    CHECK_EQ(s.haptic(1, 2.0f, 250'000'000), Session::Submit::Ok);  // amplitude clamped to 1
    auto out = s.take_outgoing();
    std::vector<Bytes> msgs;
    for (auto& x : out)
        for (auto& m : x) msgs.push_back(m);
    REQUIRE(msgs.size() == 12);
    auto setup = ripc::from_setup();
    CHECK_EQ(setup.lookup_id, uint64_t(42770887468));  // the replayed setup's lookup
    CHECK_EQ(setup.next_seq, uint32_t(26));
    CHECK_EQ(rd64(&msgs[2][8 + 9]), setup.lookup_id);
    CHECK_EQ(rd32(&msgs[6][8 + 18]), uint32_t(26));     // connect
    CHECK_EQ(rd32(&msgs[10][8 + 18]), uint32_t(27));    // call
    auto call = ripc::haptic(ripc::kHapticsClient, 27, 2, 250'000'000, 1.0f);
    CHECK_EQ(Bytes(msgs[11].begin() + 8, msgs[11].end()), call[3].body);
    // Connected once
    CHECK_EQ(s.haptic(0, 0.5f, 1000), Session::Submit::Ok);
    size_t n = 0;
    for (auto& x : s.take_outgoing()) n += x.size();
    CHECK_EQ(n, size_t(4));
}
