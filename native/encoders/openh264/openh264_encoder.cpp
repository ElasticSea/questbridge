// OpenH264 behind encoder.hpp. Rate control in bitrate mode with frame skipping, scene-change
// IDRs and background/adaptive tools off, so every frame comes out; IDRs come from our own
// frame count (gop), so all strips of a frame switch together whichever encoder runs.
// Slice cap: OpenH264's rate control never goes above QP 42 (noise at 42 is ~1.4 MB per
// 4096x448 strip), so an oversized picture is re-encoded as an IDR by an extra encoder without
// rate control at a fixed QP (created on first use). That encoder then keeps the strip (P
// frames, same QP) while the content stays hard, and hands back to rate control with an IDR
// after half a second of slices under a quarter of the cap.
#include "openh264_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "../h264.hpp"
#include "../select.hpp"
#include "wels/codec_api.h"
#include "wels/codec_ver.h"

namespace ql::enc {

namespace {

using CreateFn = int (*)(ISVCEncoder**);
using DestroyFn = void (*)(ISVCEncoder*);
using VersionFn = void (*)(OpenH264Version*);

struct Library {
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    std::string path, why;
};

std::string dll_name() {
#ifdef _WIN32
    return "openh264-" + std::to_string(OPENH264_MAJOR) + "." + std::to_string(OPENH264_MINOR) + "." +
           std::to_string(OPENH264_REVISION) + "-win64.dll";
#elif defined(__APPLE__)
    return "libopenh264-" + std::to_string(OPENH264_MAJOR) + "." + std::to_string(OPENH264_MINOR) + "." +
           std::to_string(OPENH264_REVISION) + "-mac-arm64.dylib";
#else
    return "libopenh264.so." + std::to_string(OPENH264_MAJOR) + "." + std::to_string(OPENH264_MINOR) + "." +
           std::to_string(OPENH264_REVISION);
#endif
}

void* open_lib(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// Candidate paths: QL_OPENH264, next to the module holding this code (questlink.dll, qlenc.exe),
// then where fetch.ps1 puts it.
std::vector<std::string> search_paths() {
    std::vector<std::string> out;
    if (const char* p = std::getenv("QL_OPENH264"); p && *p) out.push_back(p);
#ifdef _WIN32
    HMODULE self = nullptr;
    wchar_t buf[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&search_paths), &self) &&
        GetModuleFileNameW(self, buf, MAX_PATH)) {
        std::wstring dir(buf);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(std::max(n, 1)) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, s.data(), n, nullptr, nullptr);
        out.push_back(s + dll_name());
    }
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        out.push_back(std::string(la) + "\\QuestLink\\openh264\\" + dll_name());
#endif
    return out;
}

const Library& library() {
    static Library lib;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string tried;
        for (const auto& path : search_paths()) {
            void* h = open_lib(path);
            if (!h) {
                tried += path + " (not found); ";
                continue;
            }
            auto create = reinterpret_cast<CreateFn>(symbol(h, "WelsCreateSVCEncoder"));
            auto destroy = reinterpret_cast<DestroyFn>(symbol(h, "WelsDestroySVCEncoder"));
            auto version = reinterpret_cast<VersionFn>(symbol(h, "WelsGetCodecVersionEx"));
            OpenH264Version v{};
            if (version) version(&v);
            if (!create || !destroy || !version || v.uMajor != OPENH264_MAJOR || v.uMinor != OPENH264_MINOR) {
                tried += path + " (version " + std::to_string(v.uMajor) + "." + std::to_string(v.uMinor) + ", want " +
                         std::to_string(OPENH264_MAJOR) + "." + std::to_string(OPENH264_MINOR) + "); ";
                continue;  // kept loaded: unloading a DLL we called into is not worth the risk
            }
            lib.create = create;
            lib.destroy = destroy;
            lib.path = path;
            return;
        }
        lib.why = dll_name() + " not loaded (run native\\encoders\\openh264\\fetch.ps1): " + tried;
    });
    return lib;
}

class OpenH264Encoder final : public Encoder {
public:
    explicit OpenH264Encoder(const EncoderConfig& c) : cfg_(c) {}

    ~OpenH264Encoder() override {
        destroy(main_);
        for (auto& [qp, e] : fixed_) destroy(e);
    }

    bool init(std::string* err) {
        const Library& lib = library();
        if (!lib.create) return fail(err, lib.why);
        if (cfg_.slices > 1) return fail(err, "OpenH264 backend encodes one slice per picture (use strips)");
        if (cfg_.width % 16 || cfg_.height % 16) return fail(err, "size must be whole macroblocks");
        if (!(main_ = make(-1, err))) return false;
        i420_.resize(size_t(cfg_.width) * cfg_.height * 3 / 2);
        name_ = "OpenH264 " + std::to_string(OPENH264_MAJOR) + "." + std::to_string(OPENH264_MINOR) + "." +
                std::to_string(OPENH264_REVISION) + " (software, " + lib.path + ")";
        return true;
    }

    bool encode(const CpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        auto t0 = std::chrono::steady_clock::now();
        to_i420(f);
        bool idr = force_idr || frames_ == 0 || restart_idr_ || (cfg_.gop > 0 && frames_ % unsigned(cfg_.gop) == 0);
        restart_idr_ = false;
        ISVCEncoder* e = floor_qp_ >= 0 ? fixed(floor_qp_) : main_;
        if (!e || !encode_once(e, ts, idr, out)) return false;
        // A slice over the cap would end the headset session: re-encode this picture as an IDR
        // at a fixed QP (+6 QP roughly halves the size), rising until it fits.
        if (largest(out) > cfg_.max_slice_bytes) {
            const size_t first = largest(out);
            int qp = std::max({out.slices.empty() ? -1 : out.slices[0].qp, floor_qp_, 20});
            int floor = std::min(51, qp + int(std::ceil(6 * std::log2(double(first) / double(cfg_.max_slice_bytes)))) + 2);
            for (int attempt = 1; largest(out) > cfg_.max_slice_bytes; attempt++) {
                if (attempt > 4 || qp >= 51)
                    return fail(nullptr, "slice of " + std::to_string(largest(out)) + " B over the " +
                                             std::to_string(cfg_.max_slice_bytes) + " B cap at QP " + std::to_string(qp));
                if (!(e = fixed(floor)) || !encode_once(e, ts, true, out)) return false;
                out.refits = attempt;
                qp = floor;
                floor = std::min(51, floor + 6);
            }
            out.first_try_bytes = first;
            floor_qp_ = qp;  // main_'s reference is the rejected picture: it restarts with an IDR
            calm_ = 0;
        } else if (floor_qp_ >= 0) {
            calm_ = largest(out) < cfg_.max_slice_bytes / 4 ? calm_ + 1 : 0;
            if (calm_ >= std::max(cfg_.fps / 2, 1)) {
                floor_qp_ = -1;
                restart_idr_ = true;
            }
        }
        frames_++;
        out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return true;
    }

    bool encode(const GpuFrame&, int64_t, bool, EncodedFrame&) override {
        return fail(nullptr, "OpenH264 takes CPU frames only");
    }

    std::string name() const override { return name_; }
    const std::string& error() const override { return error_; }

private:
    bool fail(std::string* err, std::string msg) {
        error_ = std::move(msg);
        if (err) *err = error_;
        return false;
    }

    static void destroy(ISVCEncoder* e) {
        if (!e) return;
        e->Uninitialize();
        library().destroy(e);
    }

    // qp < 0: rate-controlled main encoder; else no rate control, every frame at qp.
    ISVCEncoder* make(int qp, std::string* err) {
        ISVCEncoder* e = nullptr;
        if (library().create(&e) || !e) {
            fail(err, "WelsCreateSVCEncoder failed");
            return nullptr;
        }
        SEncParamExt p;
        e->GetDefaultParams(&p);
        p.iUsageType = CAMERA_VIDEO_REAL_TIME;
        p.iPicWidth = cfg_.width;
        p.iPicHeight = cfg_.height;
        p.iRCMode = qp < 0 ? RC_BITRATE_MODE : RC_OFF_MODE;
        p.iTargetBitrate = int(cfg_.bitrate);
        p.iMaxBitrate = cfg_.cbr ? int(cfg_.bitrate) : int(cfg_.bitrate / 2 * 3);
        p.fMaxFrameRate = float(cfg_.fps);
        p.iTemporalLayerNum = 1;
        p.iSpatialLayerNum = 1;
        SSpatialLayerConfig& l = p.sSpatialLayers[0];
        l.iVideoWidth = cfg_.width;
        l.iVideoHeight = cfg_.height;
        l.fFrameRate = float(cfg_.fps);
        l.iSpatialBitrate = p.iTargetBitrate;
        l.iMaxSpatialBitrate = p.iMaxBitrate;
        l.uiProfileIdc = PRO_BASELINE;
        l.uiLevelIdc = ELevelIdc(level_for(cfg_.width, cfg_.height, cfg_.fps));
        l.iDLayerQp = qp < 0 ? 26 : qp;
        l.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
        l.sSliceArgument.uiSliceNum = 1;
        p.iComplexityMode = LOW_COMPLEXITY;
        p.uiIntraPeriod = 0;  // IDRs from encode() only
        p.iNumRefFrame = 1;
        p.eSpsPpsIdStrategy = CONSTANT_ID;
        p.bPrefixNalAddingCtrl = false;
        p.bEnableSSEI = false;
        p.iPaddingFlag = 0;
        p.iEntropyCodingModeFlag = 0;  // CAVLC
        p.bEnableFrameSkip = false;    // every frame of every strip must come out
        p.iMinQp = qp < 0 ? 0 : qp;
        p.iMaxQp = qp < 0 ? 51 : qp;
        p.bEnableLongTermReference = false;
        p.iMultipleThreadIdc = 1;  // the strip wrapper runs strips in parallel
        p.iLoopFilterDisableIdc = 0;  // one picture per strip: no slice edges to protect
        p.bEnableDenoise = false;
        p.bEnableBackgroundDetection = false;
        p.bEnableAdaptiveQuant = false;
        p.bEnableSceneChangeDetect = false;  // would insert IDRs in one strip only
        p.bEnableFrameCroppingFlag = true;
        int trace = WELS_LOG_QUIET;
        e->SetOption(ENCODER_OPTION_TRACE_LEVEL, &trace);
        if (int rc = e->InitializeExt(&p)) {
            library().destroy(e);
            fail(err, "InitializeExt failed (" + std::to_string(rc) + ")");
            return nullptr;
        }
        int fmt = videoFormatI420;
        e->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);
        return e;
    }

    ISVCEncoder* fixed(int qp) {
        auto it = fixed_.find(qp);
        if (it != fixed_.end()) return it->second;
        std::string err;
        ISVCEncoder* e = make(qp, &err);
        if (!e) return nullptr;
        fixed_[qp] = e;
        return e;
    }

    bool encode_once(ISVCEncoder* e, int64_t ts, bool idr, EncodedFrame& out) {
        if (idr) e->ForceIntraFrame(true);
        SSourcePicture pic{};
        pic.iColorFormat = videoFormatI420;
        pic.iPicWidth = cfg_.width;
        pic.iPicHeight = cfg_.height;
        pic.iStride[0] = cfg_.width;
        pic.iStride[1] = pic.iStride[2] = cfg_.width / 2;
        pic.pData[0] = i420_.data();
        pic.pData[1] = pic.pData[0] + size_t(cfg_.width) * cfg_.height;
        pic.pData[2] = pic.pData[1] + size_t(cfg_.width) * cfg_.height / 4;
        pic.uiTimeStamp = ts / 1'000'000;
        SFrameBSInfo info{};
        if (int rc = e->EncodeFrame(&pic, &info)) return fail(nullptr, "EncodeFrame failed (" + std::to_string(rc) + ")");
        if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid)
            return fail(nullptr, "OpenH264 skipped the frame");
        Bytes es;
        for (int i = 0; i < info.iLayerNum; i++) {
            const SLayerBSInfo& l = info.sLayerInfo[i];
            size_t n = 0;
            for (int k = 0; k < l.iNalCount; k++) n += size_t(l.pNalLengthInByte[k]);
            es.insert(es.end(), l.pBsBuf, l.pBsBuf + n);
        }
        out = EncodedFrame{};
        out.timestamp_ns = ts;
        parse(es, out);
        if (out.slices.empty()) return fail(nullptr, "OpenH264 returned no slice");
        return true;
    }

    void parse(std::span<const uint8_t> es, EncodedFrame& out) {
        Bytes sps, pps;
        for (auto nal : h264::split_annexb(es)) {
            switch (h264::nal_type(nal)) {
            case h264::kSps:
                sps.assign(nal.begin(), nal.end());
                h264::set_constrained_baseline(sps);
                if (!h264::add_vui(sps, cfg_.fps)) h264::set_fixed_frame_rate(sps);
                h264::parse_sps(sps, sps_);
                break;
            case h264::kPps:
                pps.assign(nal.begin(), nal.end());
                have_ps_ = h264::parse_pps(pps, pps_);
                break;
            case h264::kIdr:
            case h264::kSlice: {
                Slice sl;
                h264::SliceHeader sh;
                h264::parse_slice_header(nal, sh, have_ps_ ? &sps_ : nullptr, have_ps_ ? &pps_ : nullptr);
                sl.qp = sh.qp;
                sl.index = int(out.slices.size());
                sl.first_mb = sh.first_mb;
                h264::append_annexb(sl.nal, nal);
                out.idr = out.idr || h264::nal_type(nal) == h264::kIdr;
                out.slices.push_back(std::move(sl));
                break;
            }
            default: break;
            }
        }
        if (!sps.empty() && !pps.empty()) {
            csd_.clear();
            h264::append_annexb(csd_, sps);
            h264::append_annexb(csd_, pps);
        }
        out.csd = csd_;
    }

    static size_t largest(const EncodedFrame& f) {
        size_t n = 0;
        for (auto& s : f.slices) n = std::max(n, s.nal.size());
        return n;
    }

    // NV12, BGRA or RGBA (BT.709 limited range, 2x2 chroma average) -> I420
    void to_i420(const CpuFrame& f) {
        const int w = cfg_.width, h = cfg_.height;
        uint8_t* y = i420_.data();
        uint8_t* u = y + size_t(w) * h;
        uint8_t* v = u + size_t(w) * h / 4;
        if (f.format == PixelFormat::NV12) {
            for (int r = 0; r < h; r++) memcpy(y + size_t(r) * w, f.plane[0] + size_t(r) * f.stride[0], size_t(w));
            for (int r = 0; r < h / 2; r++) {
                const uint8_t* c = f.plane[1] + size_t(r) * f.stride[1];
                uint8_t* ur = u + size_t(r) * (w / 2);
                uint8_t* vr = v + size_t(r) * (w / 2);
                for (int x = 0; x < w / 2; x++) {
                    ur[x] = c[2 * x];
                    vr[x] = c[2 * x + 1];
                }
            }
            return;
        }
        const int bi = f.format == PixelFormat::RGBA ? 2 : 0, ri = 2 - bi;
        for (int r = 0; r < h / 2; r++) {
            const uint8_t* s0 = f.plane[0] + size_t(2 * r) * f.stride[0];
            const uint8_t* s1 = s0 + f.stride[0];
            uint8_t* y0 = y + size_t(2 * r) * w;
            uint8_t* y1 = y0 + w;
            for (int x = 0; x < w; x += 2) {
                int sb = 0, sg = 0, sr = 0;
                const uint8_t* px[4] = {s0 + 4 * x, s0 + 4 * x + 4, s1 + 4 * x, s1 + 4 * x + 4};
                uint8_t* py[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
                for (int k = 0; k < 4; k++) {
                    int b = px[k][bi], g = px[k][1], rr = px[k][ri];
                    *py[k] = uint8_t((47 * rr + 157 * g + 16 * b + 128) / 256 + 16);
                    sb += b;
                    sg += g;
                    sr += rr;
                }
                sb /= 4;
                sg /= 4;
                sr /= 4;
                u[size_t(r) * (w / 2) + x / 2] = uint8_t(std::clamp((-26 * sr - 87 * sg + 112 * sb + 128) / 256 + 128, 0, 255));
                v[size_t(r) * (w / 2) + x / 2] = uint8_t(std::clamp((112 * sr - 102 * sg - 10 * sb + 128) / 256 + 128, 0, 255));
            }
        }
    }

    EncoderConfig cfg_;
    ISVCEncoder* main_ = nullptr;
    std::map<int, ISVCEncoder*> fixed_;
    Bytes i420_, csd_;
    h264::Sps sps_;
    h264::Pps pps_;
    bool have_ps_ = false;
    bool restart_idr_ = false;
    int floor_qp_ = -1;  // >= 0: fixed(floor_qp_) holds the strip after a slice cap re-encode
    int calm_ = 0;       // consecutive frames well under the cap while on the floor
    uint64_t frames_ = 0;
    std::string name_, error_;
};

}  // namespace

bool openh264_available(std::string* why) {
    const Library& lib = library();
    if (!lib.create && why) *why = lib.why;
    return lib.create != nullptr;
}

std::unique_ptr<Encoder> create_openh264(const EncoderConfig& config, std::string* error) {
    auto enc = std::make_unique<OpenH264Encoder>(config);
    if (!enc->init(error)) return nullptr;
    return enc;
}

}  // namespace ql::enc
