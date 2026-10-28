// Frame building against Meta's recorded IDR frame (tests/fixtures/replay/idr-frame.json).
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
        auto clip = json::parse_file(qltest::repo("tests/fixtures/replay/idr-frame.json"));
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

// FovPort struct behind the pointer at data word kFovWord + eye (header = one capnp segment)
std::array<float, 4> fov_of(const Bytes& h, size_t eye) {
    size_t ptr = 8 + 8 * (es::kFovWord + eye);
    size_t t = ptr + 8 + 8 * size_t(int32_t(rd32(&h[ptr])) >> 2);
    std::array<float, 4> f;
    std::memcpy(f.data(), &h[t], sizeof f);
    return f;
}

TEST(frame_timewarp_fields) {
    const auto& r = recorded();
    // Meta's loading-screen template: mesh 0 and zero FovPorts (no reprojection) unless set
    auto plain = build_frame(slices_of(r), FrameParams{});
    CHECK_EQ(rd32(&plain[1].body[8 + es::kRectifyMeshId]), uint32_t(0));
    CHECK(fov_of(plain[1].body, 0) == (std::array<float, 4>{0, 0, 0, 0}));

    FrameParams p;
    p.rectify_mesh_id = 1001;
    p.fov = std::array<FovPort, 2>{FovPort{1.3764f, 0.8391f, 0.9657f, 1.4281f}, FovPort{0.8391f, 1.3764f, 0.9657f, 1.4281f}};
    auto out = build_frame(slices_of(r), p);
    for (size_t i = 0; i < 5; ++i) {
        const Bytes& h = out[i * 4 + 1].body;
        CHECK_EQ(rd32(&h[8 + es::kRectifyMeshId]), uint32_t(1001));
        CHECK(fov_of(h, 0) == (std::array<float, 4>{1.3764f, 0.8391f, 0.9657f, 1.4281f}));
        CHECK(fov_of(h, 1) == (std::array<float, 4>{0.8391f, 1.3764f, 0.9657f, 1.4281f}));
        // Everything else as without them
        Bytes a = h, b = plain[i * 4 + 1].body;
        for (Bytes* x : {&a, &b}) {
            wr32(&(*x)[8 + es::kRectifyMeshId], 0);
            for (size_t eye = 0; eye < 2; ++eye) {
                size_t ptr = 8 + 8 * (es::kFovWord + eye);
                std::memset(&(*x)[ptr + 8 + 8 * size_t(int32_t(rd32(&(*x)[ptr])) >> 2)], 0, 16);
            }
        }
        CHECK_EQ(a, b);
    }
}

TEST(message_cap_per_headset) {
    // Per headset, at or under the largest message seen working on it: Quest 3 128,480 B
    // (148,544 B failed), Quest Pro 106,340 B (115,332 B failed); unknown ones get the Pro's
    CHECK_EQ(message_cap("Meta Quest 3").slice, size_t(120'000));
    CHECK(message_cap("Meta Quest 3").message <= 128'480);
    CHECK_EQ(message_cap("Meta Quest Pro").slice, size_t(100'000));
    CHECK(message_cap("Meta Quest Pro").message <= 106'340);
    CHECK_EQ(message_cap("").message, message_cap("Meta Quest Pro").message);
    CHECK_EQ(message_cap("Meta Quest 3S").message, message_cap("Meta Quest Pro").message);
    for (const char* product : {"Meta Quest 3", "Meta Quest Pro"}) {
        const MessageCap cap = message_cap(product);
        // A slice at the slice cap fits, one byte over the message cap doesn't
        Bytes big(cap.slice), bigger(cap.message - kHeaderSize - 3), csd(16);
        std::vector<SliceData> ok = {{csd, big}}, too_big = {{csd, bigger}}, csd_too_big = {{bigger, csd}};
        CHECK(fits_message_cap(ok, cap.message));
        CHECK(!fits_message_cap(too_big, cap.message));
        CHECK(!fits_message_cap(csd_too_big, cap.message));
        // The whole message including padding stays within the cap
        auto out = build_frame(ok, FrameParams{});
        CHECK(out.back().body.size() + kHeaderSize <= cap.message);
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
    // Oversize slice: rejected, nothing sent, numbering unchanged
    Bytes huge(s.cap().message);
    sl[2].video = huge;
    CHECK(s.submit_frame(sl, {}) == Session::Submit::TooLarge);
    CHECK(s.take_outgoing().empty());
    CHECK_EQ(s.next_frame_index(), uint64_t(4));
    // A slice at the cap goes out whole: send_stream never drops video
    sl[2].video = Bytes(s.cap().slice);
    CHECK(s.submit_frame(sl, {}) == Session::Submit::Ok);
    CHECK_EQ(s.take_outgoing().size(), size_t(1));
    CHECK_EQ(s.dropped_messages(), uint64_t(0));
}
