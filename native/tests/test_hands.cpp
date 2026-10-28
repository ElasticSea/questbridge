// Hand tracking (topics 8/9) from the hands-on bench dump (tests/fixtures/hands/messages.json);
// mirrors tests/test_hands.py, golden joint positions from tools/decode_hands.py joints().
#include <cmath>

#include "hands.hpp"
#include "harness.hpp"
#include "session.hpp"

using namespace ql;

namespace {

std::vector<Bytes> raws(const char* key) {
    std::vector<Bytes> out;
    for (const auto& h : qltest::fixture("hands", "messages")[key].items()) out.push_back(h.as_hex());
    return out;
}

std::span<const uint8_t> body(const Bytes& raw) { return std::span(raw).subspan(8); }

std::vector<HandsFrame> frames() {
    std::vector<HandsFrame> out;
    for (const auto& r : raws("topic8")) {
        auto f = parse_hands(body(r));
        if (f) out.push_back(*f);
    }
    return out;
}

HandSkeleton skeleton(int hand) {
    for (const auto& r : raws("topic9_first_chunks")) {
        auto c = parse_skeleton_chunk(body(r));
        if (c && int(c->hand) == hand) return *parse_skeleton(c->data);
    }
    return {};
}

struct V3 {
    double x, y, z;
};
V3 vec(const float* p) { return {p[0], p[1], p[2]}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 unit(V3 a) {
    double n = len(a);
    return {a.x / n, a.y / n, a.z / n};
}
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// Column `axis` of the rotation matrix of q (x y z w)
V3 axis(const float* q, int axis) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    if (axis == 0) return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
    if (axis == 1) return {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
    return {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
}
bool near(const float* p, V3 e, double tol = 2e-5) { return len(sub(vec(p), e)) < tol; }

// OpenXR joint ids used below
constexpr int kPalm = 0, kWrist = 1, kThumbTip = 5, kIndexMeta = 6, kIndexProx = 7, kIndexInter = 8, kIndexTip = 10,
              kMiddleMeta = 11, kMiddleProx = 12, kLittleTip = 25;

}  // namespace

TEST(hands_skeleton_from_topic9) {
    HandSkeleton l = skeleton(0), r = skeleton(1);
    const int16_t parents[] = {-1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18};
    for (size_t b = 0; b < kHandBones; ++b) CHECK_EQ(l.parents[b], parents[b]);
    CHECK(std::abs(len(vec(l.bind_pos[7])) - 0.0379) < 1e-4);  // index proximal bone
    for (size_t b = 0; b < kHandBones; ++b) {
        for (int i = 0; i < 3; ++i) CHECK(std::abs(l.bind_pos[b][i] + r.bind_pos[b][i]) < 1e-6f);  // right = left mirrored
        CHECK(l.bone_radius[b] > 0.005f && l.bone_radius[b] < 0.025f);
    }
    CHECK(std::abs(l.bone_radius[6] - 0.0102952598) < 1e-9);  // Index1 capsule
    CHECK_EQ(l.bone_radius[20], l.bone_radius[8]);            // IndexTip takes Index3's
    CHECK_EQ(l.bone_radius[2], l.bone_radius[3]);             // Thumb0 takes Thumb1's
    // The built-in fallback is this skeleton
    for (int h = 0; h < 2; ++h) {
        const HandSkeleton& d = default_skeleton(h);
        const HandSkeleton& s = h ? r : l;
        for (size_t b = 0; b < kHandBones; ++b) {
            for (int i = 0; i < 3; ++i) CHECK(std::abs(d.bind_pos[b][i] - s.bind_pos[b][i]) < 1e-6f);
            CHECK_EQ(d.parents[b], s.parents[b]);
            CHECK(std::abs(d.bone_radius[b] - s.bone_radius[b]) < 1e-5f);
        }
    }
    CHECK(!parse_skeleton(std::span(raws("topic8")[0]).subspan(8, 0x600)));  // wrong version
}

TEST(hands_records) {
    auto fs = frames();
    REQUIRE(fs.size() == 2);
    for (const auto& f : fs) {
        CHECK_EQ(f.header, uint32_t(3));
        for (const auto& h : f.hands) {
            CHECK_EQ(h.status, uint32_t(1));
            CHECK(std::abs(dot(vec(h.root_rot), vec(h.root_rot)) + double(h.root_rot[3]) * h.root_rot[3] - 1) < 1e-3);
            CHECK(h.scale > 1.0f && h.scale < 1.1f);
            CHECK(h.input_status & hand_input::kPointerValid);
        }
        CHECK(f.hands[0].root_pos[0] < f.hands[1].root_pos[0]);  // left of right
        CHECK(f.hands[1].input_status & hand_input::kDominant);
    }
    CHECK(std::abs(fs[0].hands[0].time_s - 351540.861454) < 1e-6);
    CHECK(std::abs(fs[1].hands[0].pinch[0] - 0.7f) < 1e-4f);
    CHECK(!parse_hands(body(raws("topic8")[0]).subspan(0, 1000)));
}

TEST(hands_forward_kinematics_match_python) {
    auto fs = frames();
    REQUIRE(fs.size() == 2);
    HandSkeleton l = skeleton(0), r = skeleton(1);
    auto b00 = hand_bones(fs[0].hands[0], l);
    CHECK(near(b00[0].pos, {0.057472, -0.349098, -0.244506}));
    CHECK(near(b00[19].pos, {0.182576, -0.368312, -0.299846}));
    CHECK(near(b00[23].pos, {0.144000, -0.371993, -0.369572}));
    CHECK(near(b00[8].rot, {0.776810, -0.475305, -0.316093}, 2e-5) && std::abs(b00[8].rot[3] - 0.265965) < 2e-5);
    auto b11 = hand_bones(fs[1].hands[1], r);
    CHECK(near(b11[20].pos, {0.273151, -0.294242, -0.291338}));
    CHECK(near(b11[23].pos, {0.303384, -0.328185, -0.256662}));
    // Index pinch 0.7: thumb tip meets index tip
    auto b10 = hand_bones(fs[1].hands[0], l);
    CHECK(len(sub(vec(b10[19].pos), vec(b10[20].pos))) < 0.02);
}

TEST(hands_openxr_joints) {
    auto fs = frames();
    REQUIRE(fs.size() == 2);
    for (const auto& f : fs)
        for (int h = 0; h < 2; ++h) {
            HandSkeleton sk = skeleton(h);
            auto bones = hand_bones(f.hands[h], sk);
            auto j = hand_joints(h, f.hands[h], sk);
            // Mapped joints sit on their OVR bones
            CHECK_EQ(j[kWrist].pos[0], bones[0].pos[0]);
            CHECK_EQ(j[kIndexProx].pos[2], bones[6].pos[2]);
            CHECK_EQ(j[kThumbTip].pos[1], bones[19].pos[1]);
            CHECK_EQ(j[kLittleTip].pos[0], bones[23].pos[0]);
            for (const auto& p : j) {
                CHECK(std::abs(dot(vec(p.rot), vec(p.rot)) + double(p.rot[3]) * p.rot[3] - 1) < 1e-4);
                CHECK(p.radius > 0.004f && p.radius < 0.03f);
            }
            // OpenXR axes: -Z along the bone toward the fingertip, +Y out of the back of the hand
            V3 toward = unit(sub(vec(j[kIndexInter].pos), vec(j[kIndexProx].pos)));
            CHECK(dot(axis(j[kIndexProx].rot, 2), toward) < -0.999);
            V3 d = unit(sub(vec(j[kMiddleProx].pos), vec(j[kWrist].pos)));
            V3 thumbward = unit(sub(vec(j[kIndexProx].pos), vec(bones[16].pos)));  // pinky -> index knuckle
            V3 dorsal = unit(h == 0 ? cross(thumbward, d) : cross(d, thumbward));
            CHECK(dot(axis(j[kWrist].rot, 1), dorsal) > 0.9);
            CHECK(dot(axis(j[kWrist].rot, 2), d) < -0.9);
            // Synthesized: metacarpal a third of the way to the knuckle, pointing at it; palm on
            // the middle metacarpal
            V3 meta = vec(j[kIndexMeta].pos);
            CHECK(std::abs(len(sub(meta, vec(j[kWrist].pos))) * 2 - len(sub(vec(j[kIndexProx].pos), meta))) < 1e-5);
            CHECK(dot(axis(j[kIndexMeta].rot, 2), unit(sub(vec(j[kIndexProx].pos), meta))) < -0.9999);
            V3 mid = {(j[kMiddleMeta].pos[0] + j[kMiddleProx].pos[0]) / 2.0, (j[kMiddleMeta].pos[1] + j[kMiddleProx].pos[1]) / 2.0,
                      (j[kMiddleMeta].pos[2] + j[kMiddleProx].pos[2]) / 2.0};
            CHECK(near(j[kPalm].pos, mid, 1e-6));
            CHECK(len(sub(vec(j[kIndexTip].pos), vec(j[kPalm].pos))) > 0.08);
        }
}

TEST(session_hands_and_input_control) {
    int64_t now = 0;
    SessionConfig cfg;
    cfg.send_setup = false;
    Session s(cfg, [&] { return now; });
    Bytes on = input_control_hands(true);
    // Python host (XRSP_INPUT_CONTROL=0100010000000000): struct pointer, then {type 1, value 1}
    CHECK_EQ(on, Bytes({0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0}));
    CHECK_EQ(input_control_hands(false)[10], uint8_t(0));

    CHECK(!s.hands());
    for (const auto& r : raws("topic9_first_chunks")) s.on_message(r);
    CHECK(s.skeleton(0) && s.skeleton(1));
    for (const auto& r : raws("topic8")) s.on_message(r);
    REQUIRE(s.hands());
    CHECK_EQ(s.hands_count(), uint64_t(2));
    CHECK(std::abs(s.hands()->hands[0].pinch[0] - 0.7f) < 1e-4f);

    s.set_hands(true);
    auto out = s.take_outgoing();
    REQUIRE(out.size() == 1 && out[0].size() == 2);
    CHECK_EQ(out[0][0][1], topic::kInputControl);
    CHECK_EQ(Bytes(out[0][0].begin() + 8, out[0][0].end()), Bytes({0, 0, 0, 0, 2, 0, 0, 0}));
    CHECK_EQ(Bytes(out[0][1].begin() + 8, out[0][1].end()), on);
    CHECK_EQ(rd16(&out[0][1][4]), uint16_t(1));  // seq per topic
}
