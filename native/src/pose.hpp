// Topic 34 Pose2 (~543 Hz): head pose plus, while Touch controllers are in use, a list of
// controller poses. Each message is two XRSP messages: the 8-byte capnp segment table, then the
// segment parsed here (272 bytes head only, 520 with two controllers). Layout in docs/protocol/spec.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace qb {

constexpr size_t kHeadPoseSegment = 272;  // without controllers

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

// One entry of the controller list (index 0 left, 1 right); same pose layout as the head.
struct ControllerPose {
    bool present = false;  // listed in the message
    bool has_pose = false;
    uint64_t status = 0;   // 0xf tracked, 0 lost (controllers put down)
    float orientation[4] = {0, 0, 0, 1};
    float position[3] = {};
    float angular_velocity[3] = {};
    float linear_velocity[3] = {};
    uint64_t sample_time_ns = 0;  // Quest clock
};

namespace pose_status {
// Assumed OVR ovrStatus bits; the capture only shows 0xf and 0
constexpr uint64_t kOrientationTracked = 0x1, kPositionTracked = 0x2;
}

struct Pose2 {
    HeadPose head;
    std::array<ControllerPose, 2> controllers;  // by index; not present when the list is null/empty
};

// nullopt unless the segment is a Pose2 message
std::optional<Pose2> parse_pose2(std::span<const uint8_t> segment);
std::optional<HeadPose> parse_head_pose(std::span<const uint8_t> segment);

}  // namespace qb
