// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
#include "hands.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace qb {

namespace {

constexpr size_t kRecord = 0x290, kFirstRecord = 0x004;
constexpr uint32_t kHandVersion = 0xDF000001;
constexpr size_t kBindPoses = 0x18, kParents = 0x2b8, kCapsules = 0x2e8;
constexpr size_t kChunkHeader = 20;

float f32(std::span<const uint8_t> s, size_t at) {
    uint32_t u = rd32(&s[at]);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

double f64(std::span<const uint8_t> s, size_t at) {
    uint64_t u = rd64(&s[at]);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

void floats(std::span<const uint8_t> s, size_t at, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = f32(s, at + 4 * i);
}

HandRecord parse_record(std::span<const uint8_t> r) {
    HandRecord h;
    h.status = rd32(&r[0x08]);
    floats(r, 0x0c, h.root_rot, 4);
    floats(r, 0x1c, h.root_pos, 3);
    for (size_t b = 0; b < kHandBones; ++b) floats(r, 0x34 + 16 * b, h.bones[b], 4);
    h.time_s = f64(r, 0x1b4);
    h.confidence = f32(r, 0x1c4);
    h.scale = f32(r, 0x1c8);
    floats(r, 0x1cc, h.finger_confidence, 5);
    floats(r, 0x254, h.pinch, 4);
    floats(r, 0x264, h.pointer_rot, 4);
    floats(r, 0x274, h.pointer_pos, 3);
    h.input_status = rd32(&r[0x28c]);
    return h;
}

// --- quaternion math (x y z w) ---

struct Q {
    float x, y, z, w;
};
struct V {
    float x, y, z;
};

Q q(const float* p) { return {p[0], p[1], p[2], p[3]}; }
V v(const float* p) { return {p[0], p[1], p[2]}; }

Q mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

V rotate(Q r, V p) {
    Q t = mul(mul(r, {p.x, p.y, p.z, 0}), {-r.x, -r.y, -r.z, r.w});
    return {t.x, t.y, t.z};
}

V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V normalize(V a) {
    float n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return n > 0 ? scale(a, 1 / n) : a;
}

// Rotation whose columns are the given orthonormal axes
Q from_axes(V x, V y, V z) {
    float tr = x.x + y.y + z.z;
    Q r;
    if (tr > 0) {
        float s = std::sqrt(tr + 1) * 2;
        r = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    } else if (x.x > y.y && x.x > z.z) {
        float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        r = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        r = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        r = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return r;
}

// OpenXR joint frame looking along forward (-Z), +Y as close to up as possible
Q look(V forward, V up) {
    V z = normalize(scale(forward, -1));
    V x = normalize(cross(up, z));
    return from_axes(x, cross(z, x), z);
}

JointPose pose(Q r, V p, float radius) { return {{r.x, r.y, r.z, r.w}, {p.x, p.y, p.z}, radius}; }

// OVR bone frame -> OpenXR joint frame. OVR bones point along +X (left hand) / -X (right) with
// the back of the hand at -Y (left) / +Y (right); OpenXR wants -Z along the bone, +Y dorsal.
constexpr float kS = 0.70710678f;
constexpr Q kToOpenXr[2] = {{kS, 0, -kS, 0}, {0, kS, 0, kS}};

// OpenXR joint -> OVR bone; -1 = synthesized (palm, index/middle/ring metacarpals)
constexpr int kJointBone[kHandJoints] = {-1, 0,  3,  4,  5,  19, -1, 6,  7,  8,  20, -1, 9,
                                         10, 11, 21, -1, 12, 13, 14, 22, 15, 16, 17, 18, 23};
constexpr int kPalm = 0, kWrist = 1, kMiddleMetacarpal = 11;

// The bench user's left hand (topic 9 of the hands fixture)
const float kDefaultBind[kHandBones][7] = {
    {0, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 1, 0, 0, 0},
    {0.375386924f, 0.424584061f, -0.00777885597f, 0.8238644f, 0.020069303f, 0.0115540996f, -0.0104965204f},
    {0.260230273f, 0.0243308842f, 0.125678018f, 0.957023084f, 0.0248525608f, 0, 0},
    {-0.0827037692f, -0.0769616961f, -0.0840622336f, 0.990035653f, 0.0325129107f, 0, 0},
    {0.0835059285f, 0.0650157332f, -0.0582740605f, 0.992675185f, 0.0337930955f, 0, 0},
    {0.0306830909f, -0.0188555885f, 0.0432814434f, 0.998413622f, 0.0959962383f, 0.00731645525f, -0.0235506799f},
    {-0.0258524101f, -0.0071160607f, 0.00329294428f, 0.999635041f, 0.0379273035f, 0, 0},
    {-0.0160560012f, -0.027148718f, -0.0720340014f, 0.99690336f, 0.0243036486f, 0, 0},
    {-0.00906632561f, -0.0514655858f, 0.0518357493f, 0.997287393f, 0.0956466123f, 0.00254315487f, -0.00172590557f},
    {-0.0112282336f, -0.00437887385f, -0.00197826698f, 0.999925435f, 0.0429270007f, 0, 0},
    {-0.0343195461f, -0.00461183861f, -0.0930070132f, 0.995063126f, 0.027549576f, 0, 0},
    {-0.0531593636f, -0.123103403f, 0.0498134904f, 0.989716232f, 0.0886937976f, 0.00652930792f, 0.0174652431f},
    {-0.0336325206f, -0.0027898401f, 0.00567601994f, 0.999414265f, 0.0389961042f, 0, 0},
    {-0.00347746164f, 0.0291794538f, -0.0250285417f, 0.999254763f, 0.0265733916f, 0, 0},
    {-0.207035989f, -0.140342847f, 0.0183118004f, 0.968041658f, 0.0340735577f, 0.0094198361f, 0.0229985751f},
    {0.0911130384f, 0.00407136977f, 0.0281292256f, 0.99543488f, 0.0456505492f, 9.97678967e-07f, -2.19396293e-06f},
    {-0.0376166478f, -0.0429377183f, -0.0132860504f, 0.998280942f, 0.0307204165f, 0, 0},
    {0.000644743443f, 0.0491706654f, -0.0240188278f, 0.99850136f, 0.0203113798f, 0, 0},
    {0, 0, 0, 1, 0.0245907661f, -0.00102697383f, 0.000670370122f},
    {0, 0, 0, 1, 0.0223633796f, -0.00102507032f, 0.000295607577f},
    {0, 0, 0, 1, 0.0249649212f, -0.00113729888f, 0.000308652816f},
    {0, 0, 0, 1, 0.0243261252f, -0.0016081715f, 0.000257904961f},
    {0, 0, 0, 1, 0.0219223797f, -0.00121608598f, -0.000246479642f},
};
constexpr int16_t kDefaultParents[kHandBones] = {-1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18};
struct Capsule {
    uint32_t bone;
    float radius;
};
const Capsule kDefaultCapsules[] = {
    {0, 0.0182282813f},  {0, 0.0232319646f},  {0, 0.0160882752f},  {0, 0.0234608483f},  {3, 0.0183825176f},
    {4, 0.0102829514f},  {5, 0.00976880454f}, {6, 0.0102952598f},  {7, 0.00803810172f}, {8, 0.00763619645f},
    {9, 0.0111739393f},  {10, 0.00803095847f}, {11, 0.00762941083f}, {12, 0.00992213748f}, {13, 0.00761167239f},
    {14, 0.00723108882f}, {16, 0.00848335307f}, {17, 0.00676419446f}, {18, 0.00642598467f},
};

// Per-bone radius: the bone's capsule (the wrist: mean of its palm capsules); bones without
// one take their first child's (Thumb0, Pinky0) or parent's (ForearmStub, the tips).
void bone_radii(HandSkeleton& sk, std::span<const Capsule> caps) {
    float sum[kHandBones] = {};
    int n[kHandBones] = {};
    for (const auto& c : caps)
        if (c.bone < kHandBones) sum[c.bone] += c.radius, ++n[c.bone];
    for (size_t b = 0; b < kHandBones; ++b) sk.bone_radius[b] = n[b] ? sum[b] / float(n[b]) : 0;
    for (size_t b = 0; b < kHandBones; ++b) {
        if (sk.bone_radius[b] > 0) continue;
        for (size_t c = b + 1; c < kHandBones && !sk.bone_radius[b]; ++c)
            if (sk.parents[c] == int(b) && n[c]) sk.bone_radius[b] = sk.bone_radius[c];
    }
    for (size_t b = 0; b < kHandBones; ++b)
        if (!sk.bone_radius[b] && sk.parents[b] >= 0) sk.bone_radius[b] = sk.bone_radius[sk.parents[b]];
}

HandSkeleton make_default(int hand) {
    HandSkeleton sk;
    float sign = hand == 0 ? 1.0f : -1.0f;  // the right hand is the left mirrored
    for (size_t b = 0; b < kHandBones; ++b) {
        std::memcpy(sk.bind_rot[b], kDefaultBind[b], sizeof sk.bind_rot[b]);
        for (int i = 0; i < 3; ++i) sk.bind_pos[b][i] = kDefaultBind[b][4 + i] * sign;
        sk.parents[b] = kDefaultParents[b];
    }
    bone_radii(sk, kDefaultCapsules);
    return sk;
}

}  // namespace

Bytes input_control_hands(bool on) {
    // One-segment message: struct pointer (offset 0, 1 data word, 0 pointers), then the word
    Bytes b = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, uint8_t(on ? 1 : 0), 0, 0, 0, 0, 0};
    return b;
}

std::optional<HandsFrame> parse_hands(std::span<const uint8_t> body) {
    if (body.size() != kHandsBody) return std::nullopt;
    HandsFrame f;
    f.header = rd32(&body[0]);
    for (int i = 0; i < 2; ++i) f.hands[i] = parse_record(body.subspan(kFirstRecord + kRecord * size_t(i), kRecord));
    return f;
}

std::optional<SkeletonChunk> parse_skeleton_chunk(std::span<const uint8_t> body) {
    if (body.size() < kChunkHeader + kSkeletonChunkData) return std::nullopt;
    SkeletonChunk c;
    c.hand = rd32(&body[0]);
    c.time_s = f64(body, 8);
    c.offset = rd32(&body[16]);
    c.data = body.subspan(kChunkHeader, kSkeletonChunkData);
    if (c.hand > 1) return std::nullopt;
    return c;
}

std::optional<HandSkeleton> parse_skeleton(std::span<const uint8_t> blob) {
    if (blob.size() < kCapsules) return std::nullopt;
    uint32_t version = rd32(&blob[0]), bones = rd32(&blob[4]), capsules = rd32(&blob[8]);
    if (version != kHandVersion || bones != kHandBones || blob.size() < kCapsules + 32 * size_t(capsules))
        return std::nullopt;
    HandSkeleton sk;
    for (size_t b = 0; b < kHandBones; ++b) {
        floats(blob, kBindPoses + 28 * b, sk.bind_rot[b], 4);
        floats(blob, kBindPoses + 28 * b + 16, sk.bind_pos[b], 3);
        sk.parents[b] = int16_t(rd16(&blob[kParents + 2 * b]));
        if (sk.parents[b] >= int(b)) return std::nullopt;  // FK needs parents first
    }
    std::vector<Capsule> caps;
    for (size_t c = 0; c < capsules; ++c)
        caps.push_back({rd32(&blob[kCapsules + 32 * c]), f32(blob, kCapsules + 32 * c + 28)});
    bone_radii(sk, caps);
    return sk;
}

const HandSkeleton& default_skeleton(int hand) {
    static const HandSkeleton sk[2] = {make_default(0), make_default(1)};
    return sk[hand ? 1 : 0];
}

std::array<JointPose, kHandBones> hand_bones(const HandRecord& r, const HandSkeleton& sk) {
    std::array<JointPose, kHandBones> out{};
    for (size_t b = 0; b < kHandBones; ++b) {
        int p = sk.parents[b];
        Q prot = p < 0 ? q(r.root_rot) : q(out[size_t(p)].rot);
        V ppos = p < 0 ? v(r.root_pos) : v(out[size_t(p)].pos);
        V t = rotate(prot, scale(v(sk.bind_pos[b]), r.scale));
        out[b] = pose(mul(prot, q(r.bones[b])), add(ppos, t), sk.bone_radius[b] * r.scale);
    }
    return out;
}

std::array<JointPose, kHandJoints> hand_joints(int hand, const HandRecord& r, const HandSkeleton& sk) {
    auto bones = hand_bones(r, sk);
    Q fix = kToOpenXr[hand ? 1 : 0];
    std::array<JointPose, kHandJoints> out{};
    for (size_t j = 0; j < kHandJoints; ++j) {
        int b = kJointBone[j];
        if (b >= 0) out[j] = pose(mul(q(bones[size_t(b)].rot), fix), v(bones[size_t(b)].pos), bones[size_t(b)].radius);
    }
    // Index/middle/ring metacarpals: a third of the way from the wrist to the knuckle (where the
    // carpometacarpal joint sits; Pinky0 shows the same ratio), pointing at the knuckle
    V wrist = v(out[kWrist].pos);
    V up = rotate(q(out[kWrist].rot), {0, 1, 0});
    for (size_t j : {6, 11, 16}) {
        V knuckle = v(out[j + 1].pos);
        V p = add(wrist, scale(sub(knuckle, wrist), 1.0f / 3));
        out[j] = pose(look(sub(knuckle, p), up), p, out[j + 1].radius);
    }
    // Palm: centre of the middle metacarpal (OpenXR's definition)
    const JointPose& mm = out[kMiddleMetacarpal];
    V palm = scale(add(v(mm.pos), v(out[kMiddleMetacarpal + 1].pos)), 0.5f);
    out[kPalm] = pose(q(mm.rot), palm, bones[0].radius);
    return out;
}

}  // namespace qb
