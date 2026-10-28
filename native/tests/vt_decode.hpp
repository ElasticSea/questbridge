// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// VideoToolbox strip decoder for the macOS encoder tests: one strip's stream (Annex-B csd +
// slice NALs, H.264 or HEVC) back to BGRA, the way the headset runs one decoder per strip.
#pragma once

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vtdec {

using Bytes = std::vector<uint8_t>;

inline std::vector<std::span<const uint8_t>> split_annexb(const Bytes& b) {
    std::vector<std::span<const uint8_t>> nals;
    size_t start = std::string::npos;
    for (size_t i = 0; i + 3 <= b.size(); ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && b[end - 1] == 0) --end;
                nals.emplace_back(b.data() + start, end - start);
            }
            start = i + 3;
            i += 2;
        }
    }
    if (start != std::string::npos && start < b.size()) nals.emplace_back(b.data() + start, b.size() - start);
    return nals;
}

inline int hevc_type(std::span<const uint8_t> nal) { return nal.empty() ? -1 : (nal[0] >> 1) & 0x3F; }
inline int h264_type(std::span<const uint8_t> nal) { return nal.empty() ? -1 : nal[0] & 0x1F; }
// First csd NAL is an HEVC VPS (32), else H.264
inline bool is_hevc(const Bytes& csd) {
    auto n = split_annexb(csd);
    return !n.empty() && (n[0][0] & 0x80) == 0 && hevc_type(n[0]) == 32;
}

// One decoder session per strip; feed it the strip's frames in order (P frames need the last).
class StripDecoder {
public:
    StripDecoder() = default;
    StripDecoder(const StripDecoder&) = delete;
    StripDecoder& operator=(const StripDecoder&) = delete;
    ~StripDecoder() { reset(); }

    // Decode one frame; a csd (new parameter sets) restarts the session. Null on failure.
    // The caller releases the returned buffer.
    CVPixelBufferRef decode(const Bytes& csd, const Bytes& video) {
        if (!csd.empty() && csd != csd_) {
            reset();
            if (!create(csd)) return nullptr;
            csd_ = csd;
        }
        if (!session_) return nullptr;
        Bytes avcc;
        for (auto n : split_annexb(video)) {
            for (int k = 3; k >= 0; --k) avcc.push_back(uint8_t(n.size() >> (8 * k)));
            avcc.insert(avcc.end(), n.begin(), n.end());
        }
        const size_t len = avcc.size();
        CMBlockBufferRef bb = nullptr;
        CMSampleBufferRef sample = nullptr;
        out_ = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(nullptr, avcc.data(), len, kCFAllocatorNull, nullptr, 0, len, 0, &bb) == noErr &&
            CMSampleBufferCreateReady(nullptr, bb, fd_, 1, 0, nullptr, 1, &len, &sample) == noErr) {
            VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, nullptr);
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
        }
        if (sample) CFRelease(sample);
        if (bb) CFRelease(bb);
        return out_;
    }

private:
    static void on_decoded(void* self, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef img, CMTime, CMTime) {
        if (status == noErr && img) static_cast<StripDecoder*>(self)->out_ = CVPixelBufferRetain(img);
    }

    bool create(const Bytes& csd) {
        std::vector<const uint8_t*> ps;
        std::vector<size_t> ps_len;
        for (auto n : split_annexb(csd)) {
            ps.push_back(n.data());
            ps_len.push_back(n.size());
        }
        if (ps.size() < 2) return false;
        OSStatus st = is_hevc(csd)
                          ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, ps.size(), ps.data(), ps_len.data(), 4,
                                                                               nullptr, &fd_)
                          : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, ps.size(), ps.data(), ps_len.data(), 4,
                                                                               &fd_);
        if (st != noErr) return false;
        int32_t bgra = kCVPixelFormatType_32BGRA;
        CFNumberRef fmt = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bgra);
        const void* keys[1] = {kCVPixelBufferPixelFormatTypeKey};
        const void* vals[1] = {fmt};
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        VTDecompressionOutputCallbackRecord cb{on_decoded, this};
        st = VTDecompressionSessionCreate(nullptr, fd_, nullptr, attrs, &cb, &session_);
        CFRelease(attrs);
        CFRelease(fmt);
        return st == noErr;
    }

    void reset() {
        if (session_) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (fd_) {
            CFRelease(fd_);
            fd_ = nullptr;
        }
        csd_.clear();
    }

    CMFormatDescriptionRef fd_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    CVPixelBufferRef out_ = nullptr;
    Bytes csd_;
};

}  // namespace vtdec
