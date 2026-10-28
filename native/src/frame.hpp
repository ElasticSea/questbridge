// Video frame -> XRSP stream messages (port of the frame path in host/handshake.py and
// tools/encode_pattern.py).
//
// One picture = N slices; slice i goes on topic 10 + i as four messages: the 8-byte capnp
// segment table, the EncodedSlice header segment (Meta's recorded one, patched), the csd
// (SPS+PPS, optional) and the Annex-B slice NALs, the last two padded to 4 bytes (flag 0x08).
#pragma once

#include <array>
#include <cstddef>
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

// Per-eye FovPort: positive half-angle tangents in the order the EncodedSlice stores them
struct FovPort {
    float left, right, up, down;
};

struct SliceData {
    std::span<const uint8_t> csd;    // Annex-B SPS + PPS; empty = none
    std::span<const uint8_t> video;  // Annex-B slice NAL(s)
};

struct FrameParams {
    uint64_t index = 0;               // frame.index, must increase across the whole session
    int64_t target_vsync_quest_ns = 0;  // in the Quest clock
    std::optional<Pose> render_pose;  // unset = keep the template's identity pose
    // Unset = keep the template's (Meta's loading screen: mesh 0, zero FOV, no reprojection).
    // Timewarp needs Meta's game-frame values: mesh 1001 and the real per-eye FOV.
    std::optional<uint32_t> rectify_mesh_id;
    std::optional<std::array<FovPort, 2>> fov;  // left eye, right eye
    bool intra = true;
    // Picture height (HELLO encode height): the strips split it into equal macroblock rows.
    // 0 = keep the template's (2240 / 5 slices: 448 px, 28 MB each)
    uint32_t encode_height = 0;
};

// Largest XRSP message (header + padded body) we send, on any topic. Over the headset's limit it
// ends the session with "buffer too small". Quest 3 (H.264): 128,480 B works, 148,544 B fails;
// Quest Pro (HEVC): 106,340 B works, 115,332 B fails. Each slice's csd and NAL data go in one
// message each and may not be split.
constexpr size_t kMaxStreamMessage = 100 * 1024;

// EncodedSlice data-section layout (byte offsets from the start of the data section)
namespace encoded_slice {
constexpr size_t kIndex = 0;            // u64 frame.index
constexpr size_t kRectifyMeshId = 8;    // u32
constexpr size_t kPose = 12;            // f32 quat xyzw, then f32 translation xyz
constexpr size_t kSliceIndex = 48;      // u8
constexpr size_t kSliceFlags = 49;      // bit0 isIntraFrame, bit1 isLastSliceOfAFrame
constexpr size_t kYOffset = 52;         // u32 yOffsetInPixels
constexpr size_t kHeightMb = 56;        // u32 heightInMacroBlocks
constexpr size_t kCsdLength = 60;       // u32
constexpr size_t kVideoLength = 64;     // u32
// u64 words in the Quest clock: renderPose time, pcPresentBegin, targetVsync, targetDeadline,
// estimatedGpuEnd, latency marker. All shift together so targetVsync hits the target.
constexpr size_t kTimingWords[] = {5, 9, 11, 12, 13, 14};
constexpr size_t kTargetVsyncWord = 11;
constexpr size_t kFovWord = 18;         // pointers to the left / right eye FovPort (4x f32)
}  // namespace encoded_slice

// True if every slice's csd and video fit in one message of at most kMaxStreamMessage.
bool fits_message_cap(std::span<const SliceData> slices);

// Throws std::invalid_argument for 0 slices or more slices than recorded templates (5).
std::vector<StreamMessage> build_frame(std::span<const SliceData> slices, const FrameParams& p);

// Patch one EncodedSlice header segment in place (exposed for tests).
void patch_slice_header(Bytes& header, size_t slice, size_t count, const SliceData& s, const FrameParams& p);

}  // namespace ql
