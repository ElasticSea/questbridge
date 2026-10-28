// VideoToolbox strips (macOS): HEVC and H.264 through enc::create_encoder (Layout::Strips),
// every strip decoded again on its own decoder like the headset does, P frames included.
// Checks the strip format the Link stream needs: one slice per strip, per-strip parameter
// sets as csd (HEVC: VPS + SPS + PPS), IDRs on the same frame in every strip, the slice cap,
// and the EncodedSlice header build_frame writes for them.
#include <chrono>
#include <cstdlib>
#include <random>

#include "encoder.hpp"
#include "frame.hpp"
#include "harness.hpp"
#include "vt_decode.hpp"

using namespace qb;
namespace es = qb::encoded_slice;

namespace {

constexpr int kW = 4096, kStrips = 5;

// NV12 picture: strip i at luma 40 + 30 i, a white bar 256 px wide at x = bar_x
struct Canvas {
    int h;
    std::vector<uint8_t> y, uv;
    Canvas(int height) : h(height), y(size_t(kW) * size_t(height)), uv(size_t(kW) * size_t(height) / 2, 128) {}

    void draw(int bar_x) {
        const int strip_h = h / kStrips;
        for (int r = 0; r < h; ++r) {
            uint8_t* row = &y[size_t(r) * kW];
            std::memset(row, 40 + 30 * (r / strip_h), kW);
            std::memset(row + bar_x, 235, 256);
        }
    }
    void noise(std::mt19937& rng) {
        for (auto& v : y) v = uint8_t(rng());
        for (auto& v : uv) v = uint8_t(rng());
    }
    enc::CpuFrame frame() const {
        enc::CpuFrame f;
        f.plane[0] = y.data();
        f.plane[1] = uv.data();
        f.stride[0] = f.stride[1] = kW;
        return f;
    }
};

int luma_of(CVPixelBufferRef img, int x, int y) {
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    auto* p = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(img)) + CVPixelBufferGetBytesPerRow(img) * size_t(y) +
              4 * size_t(x);
    int v = (int(p[0]) + int(p[1]) + int(p[2])) / 3;  // gray: B = G = R
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    return v;
}

bool is_idr_nal(const Bytes& nal, bool hevc) {
    auto n = vtdec::split_annexb(nal);
    if (n.size() != 1) return false;
    return hevc ? (vtdec::hevc_type(n[0]) == 19 || vtdec::hevc_type(n[0]) == 20) : vtdec::h264_type(n[0]) == 5;
}

std::unique_ptr<enc::Encoder> make(enc::Codec codec, int height, int gop, size_t cap = enc::EncoderConfig{}.max_slice_bytes, uint32_t bitrate = 100'000'000) {
    enc::EncoderConfig c;
    c.width = kW;
    c.height = height;
    c.codec = codec;
    c.layout = enc::Layout::Strips;
    c.slices = kStrips;
    c.gop = gop;
    c.max_slice_bytes = cap;
    c.bitrate = bitrate;
    std::string err;
    auto e = enc::create_encoder(c, &err);
    if (!e) std::cerr << "create_encoder: " << err << "\n";
    return e;
}

// Encode `frames` frames with a moving bar, check the strip format and decode every strip
void round_trip(enc::Codec codec, int height) {
    const bool hevc = codec == enc::Codec::HEVC;
    const int strip_h = height / kStrips, gop = 4, frames = 9;
    auto e = make(codec, height, gop);
    REQUIRE(e);
    std::cerr << "  " << e->name() << "\n";
    Canvas pic(height);
    vtdec::StripDecoder dec[kStrips];
    double ms = 0;
    for (int f = 0; f < frames; ++f) {
        const int bar_x = 256 + 384 * f;
        pic.draw(bar_x);
        enc::EncodedFrame out;
        REQUIRE(e->encode(pic.frame(), int64_t(f) * 13'888'889, false, out));
        if (f) ms += out.encode_ms;  // the first frame pays session start-up
        const bool key = f % gop == 0;
        CHECK_EQ(out.idr, key);
        REQUIRE(out.slices.size() == size_t(kStrips));
        std::vector<SliceData> wire;
        for (int i = 0; i < kStrips; ++i) {
            const auto& s = out.slices[size_t(i)];
            CHECK_EQ(s.index, i);
            CHECK_EQ(s.first_mb, 0);  // every strip is its own picture
            CHECK(s.nal.size() <= enc::EncoderConfig{}.max_slice_bytes);
            CHECK_EQ(is_idr_nal(s.nal, hevc), key);  // IDRs aligned across strips
            auto ps = vtdec::split_annexb(s.csd);
            CHECK_EQ(vtdec::is_hevc(s.csd), hevc);
            if (hevc) {
                REQUIRE(ps.size() == 3);
                CHECK_EQ(vtdec::hevc_type(ps[1]), 33);
                CHECK_EQ(vtdec::hevc_type(ps[2]), 34);
            } else {
                REQUIRE(ps.size() == 2);
            }
            CVPixelBufferRef img = dec[i].decode(s.csd, s.nal);
            REQUIRE(img);
            CHECK_EQ(int(CVPixelBufferGetWidth(img)), kW);
            CHECK_EQ(int(CVPixelBufferGetHeight(img)), strip_h);
            int bg = luma_of(img, 3800, strip_h / 2), bar = luma_of(img, bar_x + 128, strip_h / 2);
            int want = int((40 + 30 * i - 16) * 255 / 219.0);  // video range -> RGB
            if (std::abs(bg - want) > 20 || bar < 200)
                std::cerr << "  frame " << f << " strip " << i << ": background " << bg << " (want ~" << want << "), bar " << bar
                          << "\n";
            CHECK(std::abs(bg - want) <= 20);
            CHECK(bar >= 200);
            CVPixelBufferRelease(img);
            wire.push_back({s.csd, s.nal});
        }
        // EncodedSlice headers: geometry from the picture height, csd / video lengths, intra flag
        FrameParams p;
        p.index = uint64_t(f + 1);
        p.encode_height = uint32_t(height);
        p.intra = out.idr;
        auto msgs = build_frame(wire, p);
        REQUIRE(msgs.size() == size_t(4 * kStrips));
        for (int i = 0; i < kStrips; ++i) {
            const Bytes& h = msgs[size_t(4 * i + 1)].body;
            size_t d = 8 + 8 * (rd32(h.data()) >> 2);
            CHECK_EQ(int(msgs[size_t(4 * i)].topic), 10 + i);
            CHECK_EQ(rd32(&h[d + es::kYOffset]), uint32_t(strip_h * i));
            CHECK_EQ(rd32(&h[d + es::kHeightMb]), uint32_t(strip_h / 16));
            CHECK_EQ(rd32(&h[d + es::kCsdLength]), uint32_t(wire[size_t(i)].csd.size()));
            CHECK_EQ(rd32(&h[d + es::kVideoLength]), uint32_t(wire[size_t(i)].video.size()));
            CHECK_EQ(int(h[d + es::kSliceFlags] & 1), int(out.idr));
        }
    }
    std::cerr << "  " << (hevc ? "HEVC" : "H.264") << " " << kW << "x" << height << ": " << ms / (frames - 1)
              << " ms/frame (sequential)\n";
}

}  // namespace

TEST(vt_hevc_strips_round_trip) { round_trip(enc::Codec::HEVC, 2240); }
TEST(vt_hevc_strips_quest_pro_height) { round_trip(enc::Codec::HEVC, 2160); }  // 432 px strips, not CTU aligned
TEST(vt_h264_strips_round_trip) { round_trip(enc::Codec::H264, 2240); }

// Noise at a high rate: VideoToolbox overshoots; encode() must refit every strip under the cap
// and the result (refit IDRs with their own parameter sets, then the main session) must decode.
TEST(vt_hevc_strips_slice_cap) {
    const size_t cap = 110'000;  // pure noise is ~100 KB per strip even at QP 51, over the real cap
    auto e = make(enc::Codec::HEVC, 2240, 72, cap, 400'000'000);
    REQUIRE(e);
    Canvas pic(2240);
    std::mt19937 rng(7);
    vtdec::StripDecoder dec[kStrips];
    int refits = 0;
    for (int f = 0; f < 6; ++f) {
        pic.noise(rng);
        enc::EncodedFrame out;
        bool ok = e->encode(pic.frame(), int64_t(f) * 13'888'889, false, out);
        if (!ok) std::cerr << "  frame " << f << ": " << e->error() << "\n";
        REQUIRE(ok);
        refits += out.refits;
        REQUIRE(out.slices.size() == size_t(kStrips));
        for (int i = 0; i < kStrips; ++i) {
            const auto& s = out.slices[size_t(i)];
            CHECK(s.nal.size() <= cap);
            CVPixelBufferRef img = dec[i].decode(s.csd, s.nal);
            CHECK(img != nullptr);
            if (img) CVPixelBufferRelease(img);
        }
    }
    std::cerr << "  refits " << refits << "\n";
    CHECK(refits > 0);  // the case is meant to hit the cap
}
