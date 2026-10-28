// Frame building against Meta's recorded IDR frame (host/replay/meta-idr.json).
#include "frame.hpp"
#include "harness.hpp"
#include "session.hpp"

using namespace ql;
namespace es = ql::encoded_slice;

namespace {

struct Recorded {
    std::vector<StreamMessage> msgs;  // the frame as Meta sent it
    std::vector<Bytes> csd, video;    // per slice, unpadded by the header lengths
    uint64_t index = 0;
};

const Recorded& recorded() {
    static Recorded rec = [] {
        Recorded r;
        auto clip = json::parse_file(qltest::repo("host/replay/meta-idr.json"));
        for (const auto& m : clip["frame"].items())
            r.msgs.push_back({uint8_t(m["topic"].as_u64()), uint8_t(m["flags"].as_u64()), m["body"].as_hex()});
        // Per slice: table, header, csd, video
        for (size_t i = 0; i + 3 < r.msgs.size(); i += 4) {
            const Bytes& h = r.msgs[i + 1].body;
            r.csd.emplace_back(r.msgs[i + 2].body.begin(), r.msgs[i + 2].body.begin() + rd32(&h[8 + es::kCsdLength]));
            r.video.emplace_back(r.msgs[i + 3].body.begin(), r.msgs[i + 3].body.begin() + rd32(&h[8 + es::kVideoLength]));
        }
        r.index = clip["frame_index"].as_u64();
        return r;
    }();
    return rec;
}

std::vector<SliceData> slices_of(const Recorded& r) {
    std::vector<SliceData> s;
    for (size_t i = 0; i < r.csd.size(); ++i) s.push_back({r.csd[i], r.video[i]});
    return s;
}

}  // namespace

TEST(frame_rebuilds_meta_idr) {
    const auto& r = recorded();
    REQUIRE(r.csd.size() == 5);
    FrameParams p;
    p.index = r.index;
    p.target_vsync_quest_ns = 400'000'000'000'000;
    auto out = build_frame(slices_of(r), p);
    REQUIRE(out.size() == r.msgs.size());
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK_EQ(out[i].topic, r.msgs[i].topic);
        CHECK_EQ(out[i].flags, r.msgs[i].flags);
        if (i % 4 != 1) {
            CHECK_EQ(out[i].body, r.msgs[i].body);  // table, csd, video: byte-exact incl. padding
            continue;
        }
        // Header: identical except the timing words, which all moved by the same shift
        Bytes ours = out[i].body, meta = r.msgs[i].body;
        REQUIRE(ours.size() == meta.size());
        int64_t shift = p.target_vsync_quest_ns - int64_t(rd64(&meta[8 + 8 * es::kTargetVsyncWord]));
        for (size_t w : es::kTimingWords) {
            uint64_t m = rd64(&meta[8 + 8 * w]);
            CHECK_EQ(rd64(&ours[8 + 8 * w]), m ? uint64_t(int64_t(m) + shift) : uint64_t(0));
            wr64(&ours[8 + 8 * w], 0);
            wr64(&meta[8 + 8 * w], 0);
        }
        CHECK_EQ(rd64(&ours[8 + 8 * es::kTargetVsyncWord]), uint64_t(0));
        CHECK_EQ(ours, meta);
    }
}

TEST(frame_header_fields) {
    const auto& r = recorded();
    FrameParams p;
    p.index = 77;
    p.target_vsync_quest_ns = 123'456'789'000;
    p.render_pose = Pose{{0.1f, 0.2f, 0.3f, 0.9f}, {1.0f, 1.5f, -2.0f}};
    p.intra = false;
    Bytes csd;  // no csd on this frame
    std::vector<SliceData> s = {{csd, r.video[0]}, {csd, r.video[1]}};
    auto out = build_frame(s, p);
    REQUIRE(out.size() == 6);  // table, header, video per slice
    for (size_t i = 0; i < 2; ++i) {
        const Bytes& h = out[i * 3 + 1].body;
        const uint8_t* d = &h[8];
        CHECK_EQ(out[i * 3].topic, uint8_t(10 + i));
        CHECK_EQ(rd64(d + es::kIndex), uint64_t(77));
        CHECK_EQ(d[es::kSliceIndex], uint8_t(i));
        CHECK_EQ(d[es::kSliceFlags], uint8_t(i == 1 ? 2 : 0));  // not intra; second is last
        CHECK_EQ(rd32(d + es::kCsdLength), uint32_t(0));
        CHECK_EQ(rd32(d + es::kVideoLength), uint32_t(r.video[i].size()));
        CHECK_EQ(rd64(d + 8 * es::kTargetVsyncWord), uint64_t(123'456'789'000));
        float q[7];
        std::memcpy(q, d + es::kPose, sizeof q);
        CHECK_EQ(q[0], 0.1f);
        CHECK_EQ(q[3], 0.9f);
        CHECK_EQ(q[6], -2.0f);
    }
}

TEST(submit_frame_needs_clock_and_numbers_seq) {
    int64_t now = 5'000'000'000;
    SessionConfig cfg;
    cfg.attach = true;
    cfg.attach_video_seq = 100;
    cfg.first_frame_index = 2;
    Session s(cfg, [&] { return now; });
    s.start();
    auto sl = slices_of(recorded());
    CHECK(s.submit_frame(sl, {}) == Session::Submit::NoClock);

    // One Quest ping: Quest clock = ours + 71.4 s
    Bytes ping(8 + 32);
    ping[0] = control::kClockPing;
    ping[1] = control::kClockFlags;
    wr16(&ping[2], 2);
    wr64(&ping[8 + 16], uint64_t(now + 71'400'000'000));
    s.on_message(encode_message(0x10, topic::kControl, 0, ping));
    s.take_outgoing();  // the pong

    now += 1'000'000;
    REQUIRE(s.submit_frame(sl, {}) == Session::Submit::Ok);
    REQUIRE(s.submit_frame(sl, {}) == Session::Submit::Ok);
    auto xfers = s.take_outgoing();
    REQUIRE(xfers.size() == 2);
    for (size_t f = 0; f < 2; ++f) {
        REQUIRE(xfers[f].size() == 20);
        for (size_t i = 0; i < 20; ++i) {
            const Bytes& m = xfers[f][i];
            CHECK_EQ(m[1], uint8_t(10 + i / 4));
            CHECK_EQ(rd16(&m[4]), uint16_t(100 + f * 4 + i % 4));
        }
        const Bytes& h = xfers[f][1];
        CHECK_EQ(rd64(&h[16 + es::kIndex]), uint64_t(2 + f));
        CHECK_EQ(int64_t(rd64(&h[16 + 8 * es::kTargetVsyncWord])), now + 71'400'000'000 + 20'000'000);
    }
    CHECK_EQ(s.next_frame_index(), uint64_t(4));
    std::vector<SliceData> none;
    CHECK(s.submit_frame(none, {}) == Session::Submit::BadSlices);
}
