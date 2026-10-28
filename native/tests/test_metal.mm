// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Metal frame submission (macOS): the real Metal + VideoToolbox texture encoder behind the
// Session API, a fake Quest behind open_usb(). A 4096x2240 texture with one colour per strip
// goes through the render event; every strip on the wire is decoded again and its colour
// checked, which proves the GPU split (rows, order) and the per-strip SPS/PPS.
// Default (auto) is HEVC on macOS (questbridge_metal_hevc_tests); QB_CODEC=h264 (questbridge_metal_tests)
// does the same with H.264 strips.
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <cstdlib>

#include "fake_quest.hpp"
#include "harness.hpp"
#include "questbridge.h"
#include "vt_decode.hpp"

namespace {

using vtdec::split_annexb;

constexpr int kW = 4096, kH = 2240, kStrips = 5, kStripH = kH / kStrips;
// BGRA per strip; one more for the bottom 32 rows of each strip
const uint8_t kColors[kStrips][4] = {
    {0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}, {64, 64, 64, 255}};
const uint8_t kEdge[4] = {0, 255, 255, 255};

struct WireSlice {
    Bytes csd, video;
};

// Slices of every frame sent so far: slice i goes on topic 10 + i as four messages
std::vector<std::vector<WireSlice>> wire_frames() {
    std::lock_guard<std::mutex> l(g_quest.mu);
    std::vector<std::vector<WireSlice>> out;
    std::vector<Bytes> pending[kStrips];  // messages of the slice in progress, per topic
    for (const auto& x : g_quest.written) {
        for (size_t i = 0; i + 8 <= x.size();) {
            size_t size = 4 + size_t(rd16(&x[i + 2])) * 4;
            int slice = x[i + 1] - 10;
            if (slice >= 0 && slice < kStrips && i + size <= x.size()) {
                auto& msgs = pending[slice];
                msgs.emplace_back(x.begin() + long(i), x.begin() + long(i + size));
                if (msgs.size() == 4) {
                    const uint8_t* d = &msgs[1][8 + 8];
                    uint32_t csd_len = rd32(d + 60), video_len = rd32(d + 64);
                    if (slice == 0) out.emplace_back(size_t(kStrips));
                    if (!out.empty()) {
                        out.back()[size_t(slice)].csd.assign(msgs[2].begin() + 8, msgs[2].begin() + 8 + csd_len);
                        out.back()[size_t(slice)].video.assign(msgs[3].begin() + 8, msgs[3].begin() + 8 + video_len);
                    }
                    msgs.clear();
                }
            }
            i += size;
        }
    }
    return out;
}

bool near(const uint8_t* px, const uint8_t* want) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(int(px[c]) - int(want[c])) > 40) return false;
    return true;
}

std::string bgr(const uint8_t* p) {
    return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
}

}  // namespace

TEST(metal_frame_submit) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    REQUIRE(dev != nil);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                                                 width:kW
                                                                                height:kH
                                                                             mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    REQUIRE(tex != nil);
    std::vector<uint8_t> pixels(size_t(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        const uint8_t* c = y % kStripH >= kStripH - 32 ? kEdge : kColors[y / kStripH];
        for (int x = 0; x < kW; ++x) std::memcpy(&pixels[(size_t(y) * kW + size_t(x)) * 4], c, 4);
    }
    [tex replaceRegion:MTLRegionMake2D(0, 0, kW, kH) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:kW * 4];

    const char* env_codec = std::getenv("QB_CODEC");
    const bool hevc = !env_codec || std::string(env_codec) != "h264";  // auto: HEVC on macOS
    qb_session* s = qb_open();
    REQUIRE(s);
    plug_recorded_quest();
    REQUIRE(wait_for([&] { return qb_poll(s) == 2; }));
    qb_device_info info{};
    REQUIRE(qb_get_device_info(s, &info) == QB_OK);
    CHECK_EQ(info.stream_codec, uint32_t(hevc ? QB_CODEC_HEVC : QB_CODEC_H264));

    qb_frame_submit f{};
    f.session = s;
    f.native_texture = (__bridge void*)tex;
    f.render_pose = {0, 0, 0, 1, 0, 1.6f, 0, 0};
    for (uint64_t i = 1; i <= 3; ++i) {
        f.frame_index = i;
        qb_get_render_event_func()(QB_EVENT_SUBMIT_FRAME, &f);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    REQUIRE(wait_for([] {
        auto w = wire_frames();
        return w.size() >= 2 && !w[1].back().video.empty();
    }));

    auto frames = wire_frames();
    const auto& frame = frames[0];
    REQUIRE(frame.size() == size_t(kStrips));
    for (int i = 0; i < kStrips; ++i) {
        const WireSlice& sl = frame[size_t(i)];
        auto csd = split_annexb(sl.csd);
        auto nals = split_annexb(sl.video);
        REQUIRE(nals.size() == 1);
        if (hevc) {
            REQUIRE(csd.size() == 3);  // VPS, SPS, PPS: each strip is its own stream
            CHECK_EQ(vtdec::hevc_type(csd[0]), 32);
            CHECK_EQ(vtdec::hevc_type(csd[1]), 33);
            CHECK_EQ(vtdec::hevc_type(csd[2]), 34);
            CHECK(vtdec::hevc_type(nals[0]) == 19 || vtdec::hevc_type(nals[0]) == 20);  // IDR
        } else {
            REQUIRE(csd.size() >= 2);
            CHECK_EQ(vtdec::h264_type(csd[0]), 7);  // SPS, then PPS: each strip is its own stream
            CHECK_EQ(vtdec::h264_type(csd[1]), 8);
            CHECK_EQ(vtdec::h264_type(nals[0]), 5);  // IDR
        }
        vtdec::StripDecoder dec;
        CVPixelBufferRef img = dec.decode(sl.csd, sl.video);
        REQUIRE(img);
        CHECK_EQ(int(CVPixelBufferGetWidth(img)), kW);
        CHECK_EQ(int(CVPixelBufferGetHeight(img)), kStripH);
        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(img));
        size_t stride = CVPixelBufferGetBytesPerRow(img);
        const uint8_t* mid = base + stride * 100 + 4 * 2048;
        const uint8_t* edge = base + stride * size_t(kStripH - 8) + 4 * 2048;
        if (!near(mid, kColors[i]) || !near(edge, kEdge))
            std::cerr << "strip " << i << ": middle " << bgr(mid) << ", bottom " << bgr(edge) << "\n";
        CHECK(near(mid, kColors[i]));
        CHECK(near(edge, kEdge));
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(img);
    }
    qb_close(s);
}
