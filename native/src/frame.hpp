// Video frame -> XRSP stream messages (port of the frame path in host/handshake.py and
// tools/encode_pattern.py).
//
// One picture = N slices; slice i goes on topic 10 + i as four messages: the 8-byte capnp
// segment table, the EncodedSlice header segment (Meta's recorded one, patched), the csd
// (SPS+PPS, optional) and the Annex-B slice NALs, the last two padded to 4 bytes (flag 0x08).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xrsp.hpp"

namespace ql {

struct Pose {
    float orientation[4];  // quaternion x, y, z, w
    float position[3];     // metres
};

struct SliceData {
    std::span<const uint8_t> csd;    // Annex-B SPS + PPS; empty = none
    std::span<const uint8_t> video;  // Annex-B slice NAL(s)
};

struct FrameParams {
    uint64_t index = 0;               // frame.index, must increase across the whole session
    int64_t target_vsync_quest_ns = 0;  // in the Quest clock
    std::optional<Pose> render_pose;  // unset = keep the template's identity pose
    bool intra = true;
};

// EncodedSlice data-section layout (byte offsets from the start of the data section)
namespace encoded_slice {
constexpr size_t kIndex = 0;            // u64 frame.index
constexpr size_t kPose = 12;            // f32 quat xyzw, then f32 translation xyz
constexpr size_t kSliceIndex = 48;      // u8
constexpr size_t kSliceFlags = 49;      // bit0 isIntraFrame, bit1 isLastSliceOfAFrame
constexpr size_t kCsdLength = 60;       // u32
constexpr size_t kVideoLength = 64;     // u32
// u64 words in the Quest clock: renderPose time, pcPresentBegin, targetVsync, targetDeadline,
// estimatedGpuEnd, latency marker. All shift together so targetVsync hits the target.
constexpr size_t kTimingWords[] = {5, 9, 11, 12, 13, 14};
constexpr size_t kTargetVsyncWord = 11;
}  // namespace encoded_slice

// Throws std::invalid_argument for 0 slices or more slices than recorded templates (5).
std::vector<StreamMessage> build_frame(std::span<const SliceData> slices, const FrameParams& p);

// Patch one EncodedSlice header segment in place (exposed for tests).
void patch_slice_header(Bytes& header, size_t slice, size_t count, const SliceData& s, const FrameParams& p);

}  // namespace ql
