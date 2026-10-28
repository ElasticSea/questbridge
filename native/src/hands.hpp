// Hand tracking: topic 8 Hands (per-hand root pose + 24 local bone rotations, 60/s) and
// topic 9 Skeleton (bind pose, chunked, once after hands are enabled). Packed little-endian C
// structs, not Cap'n Proto; layout in docs/protocol/spec.md
// and tools/decode_hands.py. Both flow only after InputControl hands-on (topic 28).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xrsp.hpp"

namespace qb {

constexpr size_t kHandsBody = 1320;
constexpr size_t kHandBones = 24;           // OVR hand bone order (WristRoot .. PinkyTip)
constexpr size_t kHandJoints = 26;          // OpenXR XR_EXT_hand_tracking order (PALM .. LITTLE_TIP)
constexpr size_t kSkeletonChunkData = 4096;

// Topic 28 InputControl segment word: u16 type (1 = hands), u16 value
Bytes input_control_hands(bool on);

namespace hand_input {
constexpr uint32_t kPointerValid = 0x02, kDominant = 0x80;
}

struct HandRecord {
    uint32_t status = 0;      // 1 tracked, 0 lost (the rest is then zero)
    float root_rot[4] = {};   // wrist (OVR WristRoot frame), x y z w
    float root_pos[3] = {};
    float bones[kHandBones][4] = {};  // local to the parent bone
    double time_s = 0;        // Quest clock
    float confidence = 0;
    float scale = 0;          // multiplies the bind translations
    float finger_confidence[5] = {};
    float pinch[4] = {};      // index, middle, ring, pinky
    float pointer_rot[4] = {};
    float pointer_pos[3] = {};
    uint32_t input_status = 0;  // hand_input bits
};

struct HandsFrame {
    uint32_t header = 0;  // 3
    HandRecord hands[2];  // left, right
};

// nullopt unless body is a 1320-byte topic 8 body
std::optional<HandsFrame> parse_hands(std::span<const uint8_t> body);

struct HandSkeleton {
    float bind_rot[kHandBones][4];
    float bind_pos[kHandBones][3];  // local to the parent, metres before hand scale
    int16_t parents[kHandBones];
    float bone_radius[kHandBones];  // from the collision capsules, before hand scale
};

struct SkeletonChunk {
    uint32_t hand = 0;  // 0 left, 1 right
    double time_s = 0;
    uint32_t offset = 0;  // into the per-hand blob
    std::span<const uint8_t> data;
};
std::optional<SkeletonChunk> parse_skeleton_chunk(std::span<const uint8_t> body);
// Bind skeleton from the first 0x548 bytes of a hand's blob (chunk at offset 0)
std::optional<HandSkeleton> parse_skeleton(std::span<const uint8_t> blob);
// The bench user's skeleton (left; right = positions negated), for when topic 9 was missed
const HandSkeleton& default_skeleton(int hand);

struct JointPose {
    float rot[4];  // x y z w
    float pos[3];
    float radius;
};

// World (tracking space) pose of every OVR bone: per bone parent * (bind translation * scale,
// streamed local rotation), from the root pose. Rotations in the OVR bone frame.
std::array<JointPose, kHandBones> hand_bones(const HandRecord& r, const HandSkeleton& sk);
// The 26 OpenXR joints with OpenXR axes (-Z toward the fingertip, +Y back of the hand),
// still in Link tracking space. Palm and index/middle/ring metacarpals are synthesized.
std::array<JointPose, kHandJoints> hand_joints(int hand, const HandRecord& r, const HandSkeleton& sk);

}  // namespace qb
