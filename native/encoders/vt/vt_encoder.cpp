// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// VideoToolbox H.264 / HEVC backend (macOS).
//
// Layout::Strips (the one to use): one VTCompressionSession per strip (5 x 4096x448), all
// submitted together. Each strip is its own stream (SPS/PPS, IDR, frame_num), which maps onto
// the headset's decoder per strip and needs no slice-count control (VideoToolbox has none).
// Layout::Slices: one full-size session with kVTCompressionPropertyKey_MaxH264SliceBytes, kept
// as a comparison; macOS 26 rejects that property for H.264 (both encoders), so it is 1 slice.
//
// Input: NV12/BGRA rows (CPU memory, or a locked CVPixelBuffer: unified memory, so a row copy
// beats VTPixelTransferSession crops) are copied into IOSurface-backed strip buffers.
// Keyframes are forced by this backend (every gop frames) so all strips switch together.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "../encoder.hpp"
#include "../h264.hpp"

namespace qb::enc {

namespace {

template <class T>
struct CfRef {
    T ref = nullptr;
    CfRef() = default;
    explicit CfRef(T r) : ref(r) {}
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    ~CfRef() {
        if (ref) CFRelease(ref);
    }
    operator T() const { return ref; }
};

CFNumberRef num(int64_t v) { return CFNumberCreate(nullptr, kCFNumberSInt64Type, &v); }
CFNumberRef num(double v) { return CFNumberCreate(nullptr, kCFNumberDoubleType, &v); }

std::string os_err(const char* what, OSStatus st) { return std::string(what) + " failed (" + std::to_string(st) + ")"; }

OSType cv_format(PixelFormat f) {
    return f == PixelFormat::BGRA ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
}

// IOSurface-backed pixel buffer pool of one strip size.
CVPixelBufferPoolRef make_pool(int w, int h, OSType format) {
    CfRef<CFMutableDictionaryRef> attrs(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks));
    CfRef<CFNumberRef> fw(num(int64_t(w))), fh(num(int64_t(h))), ff(num(int64_t(format)));
    CfRef<CFDictionaryRef> io(CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(attrs, kCVPixelBufferWidthKey, fw);
    CFDictionarySetValue(attrs, kCVPixelBufferHeightKey, fh);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, ff);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, io);
    CVPixelBufferPoolRef pool = nullptr;
    CVPixelBufferPoolCreate(nullptr, nullptr, attrs, &pool);
    return pool;
}

class VtEncoder final : public Encoder {
public:
    explicit VtEncoder(const EncoderConfig& c) : cfg_(c) {
        if (cfg_.layout == Layout::Auto) cfg_.layout = Layout::Strips;  // no slice-count control
    }

    ~VtEncoder() override {
        for (auto& s : strips_) {
            for (auto* session : {s.session, s.refits[0], s.refits[1], s.refits[2]}) {
                if (!session) continue;
                VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
                VTCompressionSessionInvalidate(session);
                CFRelease(session);
            }
            for (auto* p : s.pools)
                if (p) CVPixelBufferPoolRelease(p);
        }
    }

    bool init(std::string* error) {
        const bool strips = cfg_.layout == Layout::Strips;
        const int n = strips ? std::max(cfg_.slices, 1) : 1;
        if (cfg_.height % (16 * n)) return fail(error, "height must split into whole MB rows per strip");
        strip_h_ = cfg_.height / n;
        strips_.resize(n);
        for (int i = 0; i < n; i++) {
            if (!create_session(&strips_[i].session, -1, error)) return false;
            strips_[i].pools[0] = make_pool(cfg_.width, strip_h_, cv_format(PixelFormat::NV12));
            strips_[i].pools[1] = make_pool(cfg_.width, strip_h_, cv_format(PixelFormat::BGRA));
            if (!strips_[i].pools[0] || !strips_[i].pools[1]) return fail(error, "CVPixelBufferPoolCreate failed");
            for (int k = 0; k < 3; k++) {
                std::string why;
                if (!create_session(&strips_[i].refits[k], kRefitQp[k], &why)) refit_note_ = ", no slice cap fallback: " + why;
            }
        }
        if (refit_note_.empty() && !warm_up_refits(error)) return false;
        // ave.avc = the hardware encoder; low-latency RC runs h264.rtvc (long-term refs + MMCO)
        std::string id = "?";
        CFStringRef eid = nullptr;
        if (VTSessionCopyProperty(strips_[0].session, kVTCompressionPropertyKey_EncoderID, nullptr, &eid) == noErr && eid) {
            char buf[128];
            if (CFStringGetCString(eid, buf, sizeof buf, kCFStringEncodingUTF8)) id = buf;
            CFRelease(eid);
        }
        name_ = "VideoToolbox " + id + ", " +
                (strips ? std::to_string(n) + " strips of " + std::to_string(cfg_.width) + "x" + std::to_string(strip_h_)
                        : "1 session, MaxH264SliceBytes " + std::to_string(cfg_.max_slice_bytes)) +
                (low_latency_ ? ", low-latency RC" : "") + (cbr_ ? ", CBR" : ", ABR") + rc_note_ + refit_note_;
        return true;
    }

    bool encode(const CpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        const int fmt = f.format == PixelFormat::BGRA ? 1 : 0;
        std::vector<CVPixelBufferRef> bufs(strips_.size(), nullptr);
        for (size_t i = 0; i < strips_.size(); i++) {
            if (CVPixelBufferPoolCreatePixelBuffer(nullptr, strips_[i].pools[fmt], &bufs[i]) != kCVReturnSuccess)
                return release_fail(bufs, "CVPixelBufferPoolCreatePixelBuffer failed");
        }
        // One thread per strip: a 4096x448 BGRA strip is 7.3 MB
        std::vector<std::thread> copy;
        for (size_t i = 0; i < strips_.size(); i++)
            copy.emplace_back([&, i] { copy_strip(f, int(i), bufs[i]); });
        for (auto& t : copy) t.join();
        return submit(bufs, ts, force_idr, out);
    }

    bool encode(const GpuFrame& g, int64_t ts, bool force_idr, EncodedFrame& out) override {
        if (g.strip_count) return encode_strips(g, ts, force_idr, out);
        auto src = static_cast<CVPixelBufferRef>(g.texture);
        if (!src || int(CVPixelBufferGetWidth(src)) != cfg_.width || int(CVPixelBufferGetHeight(src)) != cfg_.height)
            return fail(nullptr, "GpuFrame needs a CVPixelBuffer of the configured size");
        OSType t = CVPixelBufferGetPixelFormatType(src);
        CpuFrame f;
        if (t == kCVPixelFormatType_32BGRA) {
            f.format = PixelFormat::BGRA;
        } else if (t != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && t != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
            return fail(nullptr, "CVPixelBuffer must be BGRA or NV12");
        }
        if (CVPixelBufferLockBaseAddress(src, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return fail(nullptr, "CVPixelBufferLockBaseAddress failed");
        if (f.format == PixelFormat::BGRA) {
            f.plane[0] = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(src));
            f.stride[0] = int(CVPixelBufferGetBytesPerRow(src));
        } else {
            for (size_t p = 0; p < 2; p++) {
                f.plane[p] = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(src, p));
                f.stride[p] = int(CVPixelBufferGetBytesPerRowOfPlane(src, p));
            }
        }
        bool ok = encode(f, ts, force_idr, out);
        CVPixelBufferUnlockBaseAddress(src, kCVPixelBufferLock_ReadOnly);
        return ok;
    }

    std::string name() const override { return name_; }
    Layout layout() const override { return cfg_.layout; }
    const std::string& error() const override { return error_; }

private:
    struct Strip {
        VTCompressionSessionRef session = nullptr;
        VTCompressionSessionRef refits[3] = {};  // fixed QP kRefitQp[k], for the slice cap fallback
        bool need_key = false;                    // `session`'s last frame was replaced by a refit
        CVPixelBufferPoolRef pools[2] = {};  // NV12, BGRA
        // Output of the frame in flight (callback thread)
        OSStatus status = noErr;
        Bytes csd;
        std::vector<Bytes> nals;  // slice NALs, start codes stripped
        std::vector<int> nal_qp;  // slice QP per NAL (H.264), -1 = unknown
        bool idr = false;
        int qp = -1;              // largest slice QP of the last output
        int refit_qp = 0;         // kRefitQp level of the last refit, 0 = none
    };

    // Caller-filled strip buffers (e.g. Metal render targets over IOSurfaces) go straight in
    bool encode_strips(const GpuFrame& g, int64_t ts, bool force_idr, EncodedFrame& out) {
        if (cfg_.layout != Layout::Strips || g.strip_count != int(strips_.size()))
            return fail(nullptr, "GpuFrame strips need Layout::Strips and one buffer per strip");
        std::vector<CVPixelBufferRef> bufs;
        for (int i = 0; i < g.strip_count; i++) {
            auto b = static_cast<CVPixelBufferRef>(g.strips[i]);
            if (!b || int(CVPixelBufferGetWidth(b)) != cfg_.width || int(CVPixelBufferGetHeight(b)) != strip_h_)
                return release_fail(bufs, "GpuFrame strip " + std::to_string(i) + " has the wrong size");
            bufs.push_back(CVPixelBufferRetain(b));  // submit() releases
        }
        return submit(bufs, ts, force_idr, out);
    }

    bool fail(std::string* error, const std::string& msg) {
        error_ = msg;
        if (error) *error = msg;
        return false;
    }

    bool release_fail(std::vector<CVPixelBufferRef>& bufs, const std::string& msg) {
        for (auto* b : bufs)
            if (b) CVPixelBufferRelease(b);
        return fail(nullptr, msg);
    }

    bool set(VTCompressionSessionRef s, CFStringRef key, CFTypeRef value) {
        return VTSessionSetProperty(s, key, value) == noErr;
    }

    // fixed_qp >= 0: a session without rate control for the slice cap fallback, at the
    // kVTCompressionPropertyKey_Quality giving that QP. The encoder reads Quality only up to its
    // first frame (later changes, even with PrepareToEncodeFrames, are ignored): one session per QP.
    // Each process gets ~35 hardware sessions, and a session's first frame costs ~40 ms extra,
    // hence a few levels per strip, warmed up in init().
    static constexpr int kRefitQp[3] = {39, 45, 51};
    bool create_session(VTCompressionSessionRef* out, int fixed_qp, std::string* error) {
        const bool strips = cfg_.layout == Layout::Strips;
        const CMVideoCodecType codec = hevc() ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
        CfRef<CFMutableDictionaryRef> spec(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                      &kCFTypeDictionaryValueCallBacks));
        // Low-latency rate control (macOS 11.3+): no frame delay, frame sizes held to the rate
        if (cfg_.low_latency_rc && fixed_qp < 0)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);
        if (cfg_.hardware && !cfg_.allow_software)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        else if (cfg_.hardware)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        OSStatus st = VTCompressionSessionCreate(nullptr, cfg_.width, strip_h_, codec, spec, nullptr,
                                                 nullptr, &VtEncoder::on_output, this, out);
        if (fixed_qp < 0) low_latency_ = st == noErr && cfg_.low_latency_rc;
        if (st != noErr) {  // retry without low-latency RC (e.g. a size it doesn't support)
            CFDictionaryRemoveValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl);
            st = VTCompressionSessionCreate(nullptr, cfg_.width, strip_h_, codec, spec, nullptr,
                                            nullptr, &VtEncoder::on_output, this, out);
        }
        if (st != noErr) return fail(error, os_err("VTCompressionSessionCreate", st));
        auto s = *out;
        const int n = int(strips_.size());
        set(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        set(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
        if (!set(s, kVTCompressionPropertyKey_ProfileLevel,
                 hevc() ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel))
            return fail(error, hevc() ? "HEVC Main profile not supported" : "Constrained Baseline profile not supported");
        set(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        set(s, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
        CfRef<CFNumberRef> fps(num(int64_t(cfg_.fps))), zero(num(int64_t(0))), huge(num(int64_t(1) << 30));
        set(s, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
        set(s, kVTCompressionPropertyKey_MaxFrameDelayCount, zero);
        // Keyframes are forced in submit() so every strip switches on the same frame
        set(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, huge);
        if (fixed_qp >= 0) {
            CfRef<CFNumberRef> q(num(quality_for_qp(fixed_qp)));
            if (!set(s, kVTCompressionPropertyKey_Quality, q) || (st = VTCompressionSessionPrepareToEncodeFrames(s)) != noErr) {
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                *out = nullptr;
                return fail(error, "Quality not supported");
            }
            return true;
        }
        CfRef<CFNumberRef> rate(num(int64_t(cfg_.bitrate / uint32_t(n))));
        cbr_ = cfg_.cbr && set(s, kVTCompressionPropertyKey_ConstantBitRate, rate);
        if (!cbr_) set(s, kVTCompressionPropertyKey_AverageBitRate, rate);
        // Hard cap per frame duration: one strip (or, for Slices, the whole picture) per frame
        CfRef<CFNumberRef> cap_bytes(num(int64_t(strips ? cfg_.max_slice_bytes : cfg_.max_slice_bytes * size_t(cfg_.slices))));
        CfRef<CFNumberRef> cap_secs(num(1.0 / cfg_.fps));
        const void* limits[2] = {cap_bytes.ref, cap_secs.ref};
        CfRef<CFArrayRef> arr(CFArrayCreate(nullptr, limits, 2, &kCFTypeArrayCallBacks));
        if (!set(s, kVTCompressionPropertyKey_DataRateLimits, arr)) rc_note_ = ", no DataRateLimits";
        if (!strips) {
            CfRef<CFNumberRef> sb(num(int64_t(cfg_.max_slice_bytes)));
            if (!set(s, kVTCompressionPropertyKey_MaxH264SliceBytes, sb)) rc_note_ += ", MaxH264SliceBytes rejected";
        }
        st = VTCompressionSessionPrepareToEncodeFrames(s);
        if (st != noErr) return fail(error, os_err("VTCompressionSessionPrepareToEncodeFrames", st));
        return true;
    }

    void copy_strip(const CpuFrame& f, int i, CVPixelBufferRef dst) {
        CVPixelBufferLockBaseAddress(dst, 0);
        const int planes = f.format == PixelFormat::BGRA ? 1 : 2;
        for (int p = 0; p < planes; p++) {
            const int rows = p == 0 ? strip_h_ : strip_h_ / 2;
            const size_t row_bytes = f.format == PixelFormat::BGRA ? size_t(cfg_.width) * 4 : size_t(cfg_.width);
            const uint8_t* src = f.plane[p] + size_t(f.stride[p]) * size_t(rows) * size_t(i);
            auto* d = static_cast<uint8_t*>(planes == 1 ? CVPixelBufferGetBaseAddress(dst)
                                                        : CVPixelBufferGetBaseAddressOfPlane(dst, size_t(p)));
            size_t dstride = planes == 1 ? CVPixelBufferGetBytesPerRow(dst) : CVPixelBufferGetBytesPerRowOfPlane(dst, size_t(p));
            for (int y = 0; y < rows; y++) memcpy(d + dstride * size_t(y), src + size_t(f.stride[p]) * size_t(y), row_bytes);
        }
        CVPixelBufferUnlockBaseAddress(dst, 0);
    }

    // Encode one buffer per strip (consumed), wait for all outputs, assemble the frame.
    // A strip over max_slice_bytes would end the headset session. VideoToolbox overshoots
    // DataRateLimits (and ignores it next to ConstantBitRate) and its rate control ignores
    // MinAllowedFrameQP, so the strip is encoded again from the same buffer on its fixed-QP
    // refit session as an IDR, +6 QP per halving needed. The strip's main session then starts
    // over with an IDR, since its last frame (its reference) never reaches the headset.
    bool submit(std::vector<CVPixelBufferRef>& bufs, int64_t ts, bool force_idr, EncodedFrame& out) {
        struct Release {
            std::vector<CVPixelBufferRef>& b;
            ~Release() {
                for (auto* p : b)
                    if (p) CVPixelBufferRelease(p);
            }
        } release{bufs};
        bool key = force_idr || frame_ == 0 || (cfg_.gop > 0 && frame_ % cfg_.gop == 0);
        frame_++;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<size_t> all(strips_.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = i;
        if (!encode_strips(all, bufs, ts, key, false)) return false;

        out = {};
        std::vector<size_t> over;
        for (size_t i = 0; i < strips_.size(); i++)
            if (strip_bytes(strips_[i]) > cfg_.max_slice_bytes) over.push_back(i);
        for (size_t i : over) out.first_try_bytes = std::max(out.first_try_bytes, strip_bytes(strips_[i]));
        for (size_t i : over) strips_[i].refit_qp = 0;
        for (int attempt = 1; !over.empty(); attempt++) {
            auto& w = strips_[over[0]];
            if (w.refit_qp >= kRefitQp[2] || !refit_note_.empty())
                return fail(nullptr, "strip " + std::to_string(over[0]) + " of " + std::to_string(strip_bytes(w)) + " B over the " +
                                         std::to_string(cfg_.max_slice_bytes) + " B cap" +
                                         (refit_note_.empty() ? " at QP " + std::to_string(w.refit_qp) : refit_note_));
            for (size_t i : over) {
                auto& s = strips_[i];
                int qp = std::max({s.qp, s.refit_qp, 20});  // QP unknown (HEVC): from 20
                double ratio = double(strip_bytes(s)) / double(cfg_.max_slice_bytes);
                int want = qp + int(std::ceil(6 * std::log2(ratio))) + 2;
                int level = kRefitQp[2];
                for (int k = 2; k >= 0; k--)
                    if (kRefitQp[k] >= want && kRefitQp[k] > s.refit_qp) level = kRefitQp[k];
                s.refit_qp = level;
                s.need_key = true;
            }
            if (!encode_strips(over, bufs, ts, true, true)) return false;
            out.refits = attempt;
            std::erase_if(over, [&](size_t i) { return strip_bytes(strips_[i]) <= cfg_.max_slice_bytes; });
        }

        out.timestamp_ns = ts;
        out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool strips = cfg_.layout == Layout::Strips;
        out.idr = true;
        for (size_t i = 0; i < strips_.size(); i++) {
            auto& s = strips_[i];
            out.idr &= s.idr;
            if (i == 0) out.csd = s.csd;
            for (size_t k = 0; k < s.nals.size(); k++) {
                const auto& nal = s.nals[k];
                Slice sl;
                sl.index = int(out.slices.size());
                if (hevc()) {
                    // first_slice_segment_in_pic_flag (first bit after the 2-byte header); a later
                    // segment would need slice_segment_address, which strips never have
                    sl.first_mb = nal.size() > 2 && (nal[2] & 0x80) ? 0 : -1;
                } else {
                    h264::SliceHeader sh;
                    h264::parse_slice_header(nal, sh);
                    sl.first_mb = sh.first_mb;
                }
                sl.qp = s.nal_qp[k];
                h264::append_annexb(sl.nal, nal);
                if (strips) sl.csd = s.csd;
                out.slices.push_back(std::move(sl));
            }
        }
        return true;
    }

    // Encode bufs[i] (not consumed) on the sessions in `which`, wait for all outputs, check them.
    // refit: on the strips' fixed-QP sessions at their refit_qp, as IDR.
    bool encode_strips(const std::vector<size_t>& which, std::vector<CVPixelBufferRef>& bufs, int64_t ts, bool key,
                       bool refit) {
        // Refit sessions run on their own timeline, one frame duration per encode
        CMTime pts = CMTimeMake(refit ? refit_pts_ : ts, 1'000'000'000);
        if (refit) refit_pts_ += 1'000'000'000 / cfg_.fps;
        CMTime dur = CMTimeMake(1'000'000'000 / cfg_.fps, 1'000'000'000);
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = int(which.size());
        }
        OSStatus bad = noErr;
        for (size_t i : which) {
            auto& s = strips_[i];
            s.status = noErr;
            s.nals.clear();
            s.nal_qp.clear();
            CfRef<CFMutableDictionaryRef> props(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                           &kCFTypeDictionaryValueCallBacks));
            if (key || s.need_key) CFDictionarySetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
            VTCompressionSessionRef session = s.session;
            if (refit) {
                for (int k = 0; k < 3; k++)
                    if (kRefitQp[k] == s.refit_qp) session = s.refits[k];
            } else {
                s.need_key = false;
            }
            OSStatus st = VTCompressionSessionEncodeFrame(session, bufs[i], pts, dur, props,
                                                          reinterpret_cast<void*>(i), nullptr);
            if (st != noErr) {
                bad = st;
                std::lock_guard<std::mutex> l(mu_);
                pending_--;
            }
        }
        {
            std::unique_lock<std::mutex> l(mu_);
            if (!cv_.wait_for(l, std::chrono::seconds(2), [this] { return pending_ == 0; }))
                return fail(nullptr, "VideoToolbox output timed out");
        }
        if (bad != noErr) return fail(nullptr, os_err("VTCompressionSessionEncodeFrame", bad));
        const bool strips = cfg_.layout == Layout::Strips;
        for (size_t i : which) {
            auto& s = strips_[i];
            if (s.status != noErr) return fail(nullptr, os_err("VideoToolbox encode", s.status));
            if (s.nals.empty()) return fail(nullptr, "VideoToolbox dropped a frame");
            if (strips && s.nals.size() != 1)
                return fail(nullptr, "strip " + std::to_string(i) + " came out as " + std::to_string(s.nals.size()) + " slices");
            s.qp = -1;
            for (int q : s.nal_qp) s.qp = std::max(s.qp, q);
        }
        return true;
    }

    // One gray frame through every refit session, so a real refit doesn't pay the first-frame cost.
    bool warm_up_refits(std::string* error) {
        std::vector<CVPixelBufferRef> bufs(strips_.size(), nullptr);
        std::vector<size_t> all(strips_.size());
        bool ok = true;
        for (size_t i = 0; i < strips_.size() && ok; i++) {
            all[i] = i;
            ok = CVPixelBufferPoolCreatePixelBuffer(nullptr, strips_[i].pools[0], &bufs[i]) == kCVReturnSuccess;
            if (!ok) break;
            CVPixelBufferLockBaseAddress(bufs[i], 0);
            for (size_t p = 0; p < 2; p++)
                memset(CVPixelBufferGetBaseAddressOfPlane(bufs[i], p), 128,
                       CVPixelBufferGetBytesPerRowOfPlane(bufs[i], p) * CVPixelBufferGetHeightOfPlane(bufs[i], p));
            CVPixelBufferUnlockBaseAddress(bufs[i], 0);
        }
        for (int k = 0; k < 3 && ok; k++) {
            for (auto& s : strips_) s.refit_qp = kRefitQp[k];
            ok = encode_strips(all, bufs, 0, true, true);
        }
        for (auto& s : strips_) s.refit_qp = 0;
        for (auto* b : bufs)
            if (b) CVPixelBufferRelease(b);
        return ok || fail(error, "slice cap fallback warm-up: " + error_);
    }

    // Largest slice NAL of a strip's last output, as sent (with the Annex-B start code)
    static size_t strip_bytes(const Strip& s) {
        size_t n = 0;
        for (auto& nal : s.nals) n = std::max(n, nal.size() + 4);
        return n;
    }

    // Quality -> slice QP of ave.avc (M5 Pro, macOS 26.7), measured on 4096x448 IDR strips
    static double quality_for_qp(int qp) {
        static const std::pair<int, double> kTable[] = {{10, 0.9}, {18, 0.8}, {23, 0.7}, {28, 0.6}, {33, 0.5}, {37, 0.4},
                                                        {40, 0.3}, {42, 0.25}, {44, 0.2}, {46, 0.15}, {48, 0.08},
                                                        {49, 0.05}, {50, 0.02}, {51, 0.0}};
        if (qp <= kTable[0].first) return kTable[0].second;
        for (size_t i = 1; i < std::size(kTable); i++) {
            auto [q1, v1] = kTable[i];
            auto [q0, v0] = kTable[i - 1];
            if (qp <= q1) return v0 + (v1 - v0) * (qp - q0) / (q1 - q0);
        }
        return 0.0;
    }

    static void on_output(void* refcon, void* frame_refcon, OSStatus status, VTEncodeInfoFlags flags,
                          CMSampleBufferRef sample) {
        auto* self = static_cast<VtEncoder*>(refcon);
        auto& s = self->strips_[reinterpret_cast<size_t>(frame_refcon)];
        s.status = status;
        if (status == noErr && sample && !(flags & kVTEncodeInfo_FrameDropped)) self->collect(s, sample);
        std::lock_guard<std::mutex> l(self->mu_);
        if (--self->pending_ == 0) self->cv_.notify_all();
    }

    bool hevc() const { return cfg_.codec == Codec::HEVC; }

    // AVCC/HVCC sample -> csd (Annex-B parameter sets; an H.264 SPS gets a VUI) + slice NALs.
    void collect(Strip& s, CMSampleBufferRef sample) {
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sample);
        auto param_set = [&](size_t k, const uint8_t** p, size_t* n, size_t* count, int* len) {
            return hevc() ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, k, p, n, count, len)
                          : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, k, p, n, count, len);
        };
        size_t count = 0;
        int len_size = 4;
        param_set(0, nullptr, nullptr, &count, &len_size);
        s.csd.clear();
        h264::Sps vt_sps;  // as VideoToolbox wrote it (POC type 0)
        for (size_t k = 0; k < count; k++) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (param_set(k, &p, &n, nullptr, nullptr) != noErr) continue;
            Bytes ps(p, p + n);
            if (!hevc() && h264::nal_type(ps) == h264::kSps) {
                // The Quest rejects POC type 0 (xr_video_codecs): rewrite to 2, slices below
                h264::parse_sps(ps, vt_sps);
                h264::set_poc_type2(ps);
                if (!h264::add_vui(ps, cfg_.fps)) h264::set_fixed_frame_rate(ps);
            }
            h264::append_annexb(s.csd, ps);
        }
        h264::Sps sps;
        h264::Pps pps;
        const bool have_ps = !hevc() && h264::parse_csd(s.csd, sps, pps);
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sample);
        Bytes data(CMBlockBufferGetDataLength(bb));
        CMBlockBufferCopyDataBytes(bb, 0, data.size(), data.data());
        s.idr = false;
        for (size_t o = 0; o + size_t(len_size) <= data.size();) {
            size_t n = 0;
            for (int b = 0; b < len_size; b++) n = n << 8 | data[o + size_t(b)];
            o += size_t(len_size);
            if (o + n > data.size()) break;
            std::span<const uint8_t> nal(data.data() + o, n);
            if (hevc()) {
                uint8_t t = n ? (nal[0] >> 1) & 0x3F : 63;
                if (t < 32) {  // VCL (7.4.2.2)
                    s.nals.emplace_back(nal.begin(), nal.end());
                    s.nal_qp.push_back(-1);
                }
                if (t == 19 || t == 20) s.idr = true;                       // IDR_W_RADL, IDR_N_LP
            } else {
                uint8_t t = h264::nal_type(nal);
                if (t == h264::kSlice || t == h264::kIdr) {
                    s.nals.emplace_back(nal.begin(), nal.end());
                    if (have_ps && !h264::drop_poc_lsb(s.nals.back(), vt_sps, pps)) s.status = kVTParameterErr;
                    h264::SliceHeader sh;
                    h264::parse_slice_header(s.nals.back(), sh, have_ps ? &sps : nullptr, have_ps ? &pps : nullptr);
                    s.nal_qp.push_back(sh.qp);
                }
                if (t == h264::kIdr) s.idr = true;
            }
            o += n;
        }
    }

    EncoderConfig cfg_;
    std::string name_, error_, rc_note_, refit_note_;
    std::vector<Strip> strips_;
    int strip_h_ = 0;
    int64_t frame_ = 0;
    int64_t refit_pts_ = 0;
    bool low_latency_ = false, cbr_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    int pending_ = 0;
};

}  // namespace

std::unique_ptr<Encoder> create_encoder(const EncoderConfig& config, std::string* error) {
    auto enc = std::make_unique<VtEncoder>(config);
    if (!enc->init(error)) return nullptr;
    return enc;
}

}  // namespace qb::enc
