#include "pose.hpp"

#include <cstring>

#include "capnp.hpp"
#include "xrsp.hpp"

namespace qb {

namespace {

// Root struct: 18 data words (vsync @0, send time @0x88), pointers: 0 controller list,
// 1 head pose, 2 head status. Pose struct (11 words), byte offsets:
constexpr size_t kRootWords = 18, kVsync = 0x00, kSendTime = 0x88;
constexpr size_t kPoseWords = 11, kAngVel = 0x00, kLinVel = 0x0C, kAngAcc = 0x18, kLinAcc = 0x24,
                 kOrientation = 0x30, kPosition = 0x40, kSampleTime = 0x50;

void floats(std::span<const uint8_t> s, size_t at, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t u = rd32(&s[at + 4 * i]);
        std::memcpy(&out[i], &u, 4);
    }
}

}  // namespace

std::optional<Pose2> parse_pose2(std::span<const uint8_t> segment) {
    auto root = capnp::View::root(segment);
    if (!root || root.data_words() < kRootWords || root.ptr_words() < 2) return std::nullopt;
    auto pose = root.ptr(1);
    if (pose.data_words() < kPoseWords) return std::nullopt;
    Pose2 out;
    HeadPose& p = out.head;
    auto d = pose.data(), r = root.data();
    floats(d, kOrientation, p.orientation, 4);
    floats(d, kPosition, p.position, 3);
    floats(d, kAngVel, p.angular_velocity, 3);
    floats(d, kLinVel, p.linear_velocity, 3);
    floats(d, kAngAcc, p.angular_acceleration, 3);
    floats(d, kLinAcc, p.linear_acceleration, 3);
    p.sample_time_ns = rd64(&d[kSampleTime]);
    p.vsync_ns = rd64(&r[kVsync]);
    p.send_time_ns = rd64(&r[kSendTime]);

    // Controller list entries: {u32 index, ptr 0 pose, ptr 1 status}
    capnp::View entries[4];
    size_t n = root.list(0, entries, 4);
    for (size_t i = 0; i < n && i < 4; ++i) {
        const auto& e = entries[i];
        if (e.data_words() < 1 || e.ptr_words() < 2) continue;
        uint32_t index = rd32(e.data().data());
        if (index > 1) continue;
        ControllerPose& c = out.controllers[index];
        c.present = true;
        if (auto st = e.ptr(1); st.data_words() >= 1) c.status = rd64(st.data().data());
        auto cp = e.ptr(0);
        if (cp.data_words() < kPoseWords) continue;
        auto cd = cp.data();
        c.has_pose = true;
        floats(cd, kOrientation, c.orientation, 4);
        floats(cd, kPosition, c.position, 3);
        floats(cd, kAngVel, c.angular_velocity, 3);
        floats(cd, kLinVel, c.linear_velocity, 3);
        c.sample_time_ns = rd64(&cd[kSampleTime]);
    }
    return out;
}

std::optional<HeadPose> parse_head_pose(std::span<const uint8_t> segment) {
    auto p = parse_pose2(segment);
    if (!p) return std::nullopt;
    return p->head;
}

}  // namespace qb
