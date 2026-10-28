#include "pose.hpp"

#include <cstring>

#include "xrsp.hpp"

namespace ql {

namespace {

// Byte offsets in the segment (root struct: 18 data words, 3 pointers)
constexpr size_t kVsync = 0x08, kSendTime = 0x90, kAngVel = 0xB8, kLinVel = 0xC4, kAngAcc = 0xD0, kLinAcc = 0xDC,
                 kOrientation = 0xE8, kPosition = 0xF8, kSampleTime = 0x108;

void floats(std::span<const uint8_t> s, size_t at, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t u = rd32(&s[at + 4 * i]);
        std::memcpy(&out[i], &u, 4);
    }
}

}  // namespace

std::optional<HeadPose> parse_head_pose(std::span<const uint8_t> s) {
    if (s.size() != kHeadPoseSegment) return std::nullopt;
    HeadPose p;
    floats(s, kOrientation, p.orientation, 4);
    floats(s, kPosition, p.position, 3);
    floats(s, kAngVel, p.angular_velocity, 3);
    floats(s, kLinVel, p.linear_velocity, 3);
    floats(s, kAngAcc, p.angular_acceleration, 3);
    floats(s, kLinAcc, p.linear_acceleration, 3);
    p.sample_time_ns = rd64(&s[kSampleTime]);
    p.vsync_ns = rd64(&s[kVsync]);
    p.send_time_ns = rd64(&s[kSendTime]);
    return p;
}

}  // namespace ql
