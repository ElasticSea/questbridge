// Head pose from topic 34 (~543 Hz). Each pose is two XRSP messages: the 8-byte capnp
// segment table, then the 272-byte segment parsed here. Layout in PROTOCOL_NOTES.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ql {

constexpr size_t kHeadPoseSegment = 272;

struct HeadPose {
    float orientation[4];           // quaternion x, y, z, w (world frame)
    float position[3];              // metres
    float angular_velocity[3];
    float linear_velocity[3];
    float angular_acceleration[3];
    float linear_acceleration[3];
    uint64_t sample_time_ns;  // Quest clock
    uint64_t vsync_ns;        // Quest clock
    uint64_t send_time_ns;    // Quest clock
};

// Returns nullopt unless the segment is a 272-byte head pose.
std::optional<HeadPose> parse_head_pose(std::span<const uint8_t> segment);

}  // namespace ql
