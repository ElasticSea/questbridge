// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Head pose (topic 34) from the recorded Quest stream.
#include <cmath>

#include "harness.hpp"
#include "pose.hpp"
#include "session.hpp"

using namespace qb;

namespace {

std::vector<Bytes> pose_samples() {
    std::vector<Bytes> out;
    for (const auto& g : qbtest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == topic::kHeadPose && g["dir"].as_string() == "quest_to_pc")
            for (const auto& s : g["samples"].items()) out.push_back(s["raw"].as_hex());
    return out;
}

}  // namespace

TEST(head_pose_golden_sample) {
    auto samples = pose_samples();
    REQUIRE(samples.size() == 2);
    CHECK(!parse_head_pose(std::span(samples[0]).subspan(8)));  // the segment table message
    auto p = parse_head_pose(std::span(samples[1]).subspan(8));
    REQUIRE(p);
    double n = 0;
    for (float q : p->orientation) n += double(q) * q;
    CHECK(std::abs(n - 1.0) < 1e-6);
    CHECK(std::abs(p->orientation[3] - -0.9991f) < 1e-4f);
    CHECK(std::abs(p->position[2] - 0.0755765f) < 1e-6f);
    CHECK_EQ(p->sample_time_ns, uint64_t(344814707924000));
    CHECK_EQ(p->vsync_ns, uint64_t(344814700643910));
    CHECK_EQ(p->send_time_ns, uint64_t(344814710923747));
}

TEST(session_tracks_latest_head_pose) {
    int64_t now = 0;
    SessionConfig cfg;
    cfg.attach = true;
    Session s(cfg, [&] { return now; });
    s.start();
    CHECK(!s.head_pose());
    for (const auto& raw : pose_samples()) s.on_message(raw);
    REQUIRE(s.head_pose());
    CHECK_EQ(s.head_pose()->sample_time_ns, uint64_t(344814707924000));
    CHECK_EQ(s.head_pose_count(), uint64_t(1));
}
