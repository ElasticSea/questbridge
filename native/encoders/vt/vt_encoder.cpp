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

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "../encoder.hpp"
#include "../h264.hpp"

namespace ql::enc {

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
    explicit VtEncoder(const EncoderConfig& c) : cfg_(c) {}

    ~VtEncoder() override {
        for (auto& s : strips_) {
            if (s.session) {
                VTCompressionSessionCompleteFrames(s.session, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s.session);
                CFRelease(s.session);
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
            if (!create_session(strips_[i], error)) return false;
            strips_[i].pools[0] = make_pool(cfg_.width, strip_h_, cv_format(PixelFormat::NV12));
            strips_[i].pools[1] = make_pool(cfg_.width, strip_h_, cv_format(PixelFormat::BGRA));
            if (!strips_[i].pools[0] || !strips_[i].pools[1]) return fail(error, "CVPixelBufferPoolCreate failed");
        }
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
                (low_latency_ ? ", low-latency RC" : "") + (cbr_ ? ", CBR" : ", ABR") + rc_note_;
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
    const std::string& error() const override { return error_; }

private:
    struct Strip {
        VTCompressionSessionRef session = nullptr;
        CVPixelBufferPoolRef pools[2] = {};  // NV12, BGRA
        // Output of the frame in flight (callback thread)
        OSStatus status = noErr;
        Bytes csd;
        std::vector<Bytes> nals;  // slice NALs, start codes stripped
        bool idr = false;
    };

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

    bool create_session(Strip& strip, std::string* error) {
        const bool strips = cfg_.layout == Layout::Strips;
        const CMVideoCodecType codec = hevc() ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
        CfRef<CFMutableDictionaryRef> spec(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                      &kCFTypeDictionaryValueCallBacks));
        // Low-latency rate control (macOS 11.3+): no frame delay, frame sizes held to the rate
        if (cfg_.low_latency_rc)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);
        if (cfg_.hardware && !cfg_.allow_software)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        else if (cfg_.hardware)
            CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        OSStatus st = VTCompressionSessionCreate(nullptr, cfg_.width, strip_h_, codec, spec, nullptr,
                                                 nullptr, &VtEncoder::on_output, this, &strip.session);
        low_latency_ = st == noErr && cfg_.low_latency_rc;
        if (st != noErr) {  // retry without low-latency RC (e.g. a size it doesn't support)
            CFDictionaryRemoveValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl);
            st = VTCompressionSessionCreate(nullptr, cfg_.width, strip_h_, codec, spec, nullptr,
                                            nullptr, &VtEncoder::on_output, this, &strip.session);
        }
        if (st != noErr) return fail(error, os_err("VTCompressionSessionCreate", st));
        auto s = strip.session;
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
    bool submit(std::vector<CVPixelBufferRef>& bufs, int64_t ts, bool force_idr, EncodedFrame& out) {
        bool key = force_idr || frame_ == 0 || (cfg_.gop > 0 && frame_ % cfg_.gop == 0);
        frame_++;
        CfRef<CFDictionaryRef> props;
        if (key) {
            const void* k[1] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
            const void* v[1] = {kCFBooleanTrue};
            props.ref = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        CMTime pts = CMTimeMake(ts, 1'000'000'000);
        CMTime dur = CMTimeMake(1'000'000'000 / cfg_.fps, 1'000'000'000);
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = int(strips_.size());
        }
        auto t0 = std::chrono::steady_clock::now();
        OSStatus bad = noErr;
        for (size_t i = 0; i < strips_.size(); i++) {
            auto& s = strips_[i];
            s.status = noErr;
            s.nals.clear();
            OSStatus st = VTCompressionSessionEncodeFrame(s.session, bufs[i], pts, dur, props, reinterpret_cast<void*>(i), nullptr);
            CVPixelBufferRelease(bufs[i]);
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
        out = {};
        out.timestamp_ns = ts;
        out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool strips = cfg_.layout == Layout::Strips;
        out.idr = true;
        for (size_t i = 0; i < strips_.size(); i++) {
            auto& s = strips_[i];
            if (s.status != noErr) return fail(nullptr, os_err("VideoToolbox encode", s.status));
            if (s.nals.empty()) return fail(nullptr, "VideoToolbox dropped a frame");
            if (strips && s.nals.size() != 1)
                return fail(nullptr, "strip " + std::to_string(i) + " came out as " + std::to_string(s.nals.size()) + " slices");
            out.idr &= s.idr;
            if (i == 0) out.csd = s.csd;
            for (auto& nal : s.nals) {
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
                h264::append_annexb(sl.nal, nal);
                if (strips) sl.csd = s.csd;
                out.slices.push_back(std::move(sl));
            }
        }
        return true;
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
        for (size_t k = 0; k < count; k++) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            if (param_set(k, &p, &n, nullptr, nullptr) != noErr) continue;
            Bytes ps(p, p + n);
            if (!hevc() && h264::nal_type(ps) == h264::kSps && !h264::add_vui(ps, cfg_.fps))
                h264::set_fixed_frame_rate(ps);
            h264::append_annexb(s.csd, ps);
        }
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
                if (t < 32) s.nals.emplace_back(nal.begin(), nal.end());  // VCL (7.4.2.2)
                if (t == 19 || t == 20) s.idr = true;                       // IDR_W_RADL, IDR_N_LP
            } else {
                uint8_t t = h264::nal_type(nal);
                if (t == h264::kSlice || t == h264::kIdr) s.nals.emplace_back(nal.begin(), nal.end());
                if (t == h264::kIdr) s.idr = true;
            }
            o += n;
        }
    }

    EncoderConfig cfg_;
    std::string name_, error_, rc_note_;
    std::vector<Strip> strips_;
    int strip_h_ = 0;
    int64_t frame_ = 0;
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

}  // namespace ql::enc
