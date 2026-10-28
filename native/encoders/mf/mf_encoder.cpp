// Media Foundation H.264 encoder: the GPU's hardware MFT (async) if present, else a software MFT.
//
// Slices: CODECAPI_AVEncSliceControlMode/Size. Mode 2 (MB rows) with size = height_mbs / slices
// is tried first, then mode 0 (MB count). The layout actually produced is checked per frame
// from first_mb_in_slice; callers see it in Slice::first_mb.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <execution>
#include <mutex>
#include <numeric>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <strmif.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "../encoder.hpp"
#include "../h264.hpp"

using Microsoft::WRL::ComPtr;

namespace ql::enc {

namespace {

std::string hr_text(const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s failed (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// BT.709 limited range, 2x2 chroma average.
void bgra_to_nv12(const uint8_t* src, int src_stride, int w, int h, uint8_t* y, uint8_t* uv) {
    std::vector<int> rows(size_t(h / 2));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int r) {
        const uint8_t* s0 = src + size_t(2 * r) * src_stride;
        const uint8_t* s1 = s0 + src_stride;
        uint8_t* y0 = y + size_t(2 * r) * w;
        uint8_t* y1 = y0 + w;
        uint8_t* c = uv + size_t(r) * w;
        for (int x = 0; x < w; x += 2) {
            int sb = 0, sg = 0, sr = 0;
            const uint8_t* px[4] = {s0 + 4 * x, s0 + 4 * x + 4, s1 + 4 * x, s1 + 4 * x + 4};
            uint8_t* py[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; k++) {
                int b = px[k][0], g = px[k][1], rr = px[k][2];
                *py[k] = uint8_t((47 * rr + 157 * g + 16 * b + 128) / 256 + 16);
                sb += b;
                sg += g;
                sr += rr;
            }
            sb /= 4;
            sg /= 4;
            sr /= 4;
            c[x] = uint8_t(std::clamp((-26 * sr - 87 * sg + 112 * sb + 128) / 256 + 128, 0, 255));
            c[x + 1] = uint8_t(std::clamp((112 * sr - 102 * sg - 10 * sb + 128) / 256 + 128, 0, 255));
        }
    });
}

// Collects MFT events from BeginGetEvent so the encode loop can wait with a timeout.
class EventQueue : public IMFAsyncCallback {
public:
    explicit EventQueue(IMFMediaEventGenerator* gen) : gen_(gen) {}

    HRESULT start() { return gen_->BeginGetEvent(this, nullptr); }

    void stop() {
        std::lock_guard lock(m_);
        stopped_ = true;
    }

    // Wait for the next event type; returns 0 on timeout.
    MediaEventType wait(std::chrono::milliseconds timeout, HRESULT* status) {
        std::unique_lock lock(m_);
        if (!cv_.wait_for(lock, timeout, [&] { return !events_.empty(); })) return 0;
        auto [type, hr] = events_.front();
        events_.pop_front();
        if (status) *status = hr;
        return type;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = gen_->EndGetEvent(result, &ev);
        std::lock_guard lock(m_);
        if (FAILED(hr) || stopped_) return S_OK;
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        ev->GetType(&type);
        ev->GetStatus(&status);
        events_.emplace_back(type, status);
        cv_.notify_all();
        gen_->BeginGetEvent(this, nullptr);
        return S_OK;
    }

private:
    ULONG refs_ = 1;
    IMFMediaEventGenerator* gen_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<MediaEventType, HRESULT>> events_;
    bool stopped_ = false;
};

class MfEncoder final : public Encoder {
public:
    explicit MfEncoder(const EncoderConfig& c) : cfg_(c) {}

    ~MfEncoder() override {
        if (events_) {
            events_->stop();
            events_->Release();
        }
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (activate_) activate_->ShutdownObject();
        mft_.Reset();
        activate_.Reset();
        if (mf_started_) MFShutdown();
    }

    bool init(std::string* err) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return fail(err, hr_text("CoInitializeEx", hr));
        if (FAILED(hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return fail(err, hr_text("MFStartup", hr));
        mf_started_ = true;
        if (cfg_.gpu_device && !init_d3d(err)) return false;

        std::string tried;
        if (cfg_.hardware && open(true, &tried)) return true;
        if (cfg_.allow_software && open(false, &tried)) return true;
        return fail(err, "no usable H.264 encoder MFT: " + tried);
    }

    bool encode(const CpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        const int w = cfg_.width, h = cfg_.height;
        const DWORD size = DWORD(w) * h * 3 / 2;
        if (!cpu_buffer_) {
            HRESULT hr = MFCreateMemoryBuffer(size, &cpu_buffer_);
            if (FAILED(hr)) return fail(nullptr, hr_text("MFCreateMemoryBuffer", hr));
        }
        BYTE* dst = nullptr;
        HRESULT hr = cpu_buffer_->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) return fail(nullptr, hr_text("IMFMediaBuffer::Lock", hr));
        if (f.format == PixelFormat::NV12) {
            for (int r = 0; r < h; r++) memcpy(dst + size_t(r) * w, f.plane[0] + size_t(r) * f.stride[0], w);
            for (int r = 0; r < h / 2; r++)
                memcpy(dst + size_t(w) * h + size_t(r) * w, f.plane[1] + size_t(r) * f.stride[1], w);
        } else {
            bgra_to_nv12(f.plane[0], f.stride[0], w, h, dst, dst + size_t(w) * h);
        }
        cpu_buffer_->Unlock();
        cpu_buffer_->SetCurrentLength(size);
        return submit(cpu_buffer_.Get(), ts, force_idr, out);
    }

    bool encode(const GpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        if (!device_) return fail(nullptr, "no GPU device configured");
        if (!dxgi_manager_set_) return fail(nullptr, "encoder MFT has no D3D11 support");
        auto* tex = static_cast<ID3D11Texture2D*>(f.texture);
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        ID3D11Texture2D* src = tex;
        unsigned sub = f.subresource;
        if (desc.Format != DXGI_FORMAT_NV12) {
            if (!convert_on_gpu(tex, f.subresource, desc)) return false;
            src = nv12_.Get();
            sub = 0;
        }
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), src, sub, FALSE, &buf);
        if (FAILED(hr)) return fail(nullptr, hr_text("MFCreateDXGISurfaceBuffer", hr));
        ComPtr<IMF2DBuffer> b2;
        DWORD len = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
        return submit(buf.Get(), ts, force_idr, out);
    }

    std::string name() const override { return name_; }
    const std::string& error() const override { return error_; }

private:
    bool fail(std::string* err, std::string msg) {
        error_ = std::move(msg);
        if (err) *err = error_;
        return false;
    }

    bool init_d3d(std::string* err) {
        device_ = static_cast<ID3D11Device*>(cfg_.gpu_device);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &dxgi_manager_);
        if (FAILED(hr)) return fail(err, hr_text("MFCreateDXGIDeviceManager", hr));
        if (FAILED(hr = dxgi_manager_->ResetDevice(device_.Get(), reset_token_)))
            return fail(err, hr_text("IMFDXGIDeviceManager::ResetDevice", hr));
        return true;
    }

    // Try every MFT of one kind (hardware or software) until one accepts our configuration.
    bool open(bool hardware, std::string* tried) {
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outt{MFMediaType_Video, MFVideoFormat_H264};
        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER |
                       (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT);
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outt, &list, &count))) return false;
        bool ok = false;
        for (UINT32 i = 0; i < count; i++) {
            if (!ok) {
                activate_ = list[i];
                wchar_t* friendly = nullptr;
                UINT32 len = 0;
                list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len);
                name_ = narrow(friendly) + (hardware ? " (hardware)" : " (software)");
                CoTaskMemFree(friendly);
                std::string why;
                ok = configure(&why);
                if (!ok) {
                    *tried += name_ + ": " + why + "; ";
                    if (events_) {
                        events_->stop();
                        events_->Release();
                        events_ = nullptr;
                    }
                    mft_.Reset();
                    activate_->ShutdownObject();
                    activate_.Reset();
                }
            }
            list[i]->Release();
        }
        CoTaskMemFree(list);
        return ok;
    }

    bool configure(std::string* why) {
        HRESULT hr = activate_->ActivateObject(IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) return fail(why, hr_text("ActivateObject", hr));

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
            UINT32 is_async = 0;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
            if (is_async) {
                async_ = true;
                if (FAILED(hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))
                    return fail(why, hr_text("MF_TRANSFORM_ASYNC_UNLOCK", hr));
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (dxgi_manager_) {
            UINT32 d3d_aware = 0;
            if (attrs) attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d_aware);
            if (d3d_aware && SUCCEEDED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                            ULONG_PTR(dxgi_manager_.Get()))))
                dxgi_manager_set_ = true;
        }

        if (FAILED(hr = mft_.As(&codec_))) return fail(why, hr_text("ICodecAPI", hr));
        set_codec(CODECAPI_AVLowLatencyMode, 1, VT_BOOL);
        set_codec(CODECAPI_AVEncCommonRateControlMode,
                  cfg_.cbr ? eAVEncCommonRateControlMode_CBR : eAVEncCommonRateControlMode_PeakConstrainedVBR);
        set_codec(CODECAPI_AVEncCommonMeanBitRate, cfg_.bitrate);
        if (!cfg_.cbr) set_codec(CODECAPI_AVEncCommonMaxBitRate, cfg_.bitrate / 2 * 3);
        set_codec(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        set_codec(CODECAPI_AVEncVideoMaxNumRefFrame, 1);
        set_codec(CODECAPI_AVEncVideoLTRBufferControl, 0);
        set_codec(CODECAPI_AVEncH264CABACEnable, 0, VT_BOOL);
        // Each slice goes to its own decoder on the Quest, so no filtering across slice edges.
        // NVIDIA then writes disable_deblocking_filter_idc 1 (like x264; Meta's NVENC uses 2).
        set_codec(CODECAPI_AVEnableInLoopDeblockFilter, 0);
        set_codec(CODECAPI_AVEncMPVGOPSize, cfg_.gop > 0 ? cfg_.gop : 0xFFFFFFFFu);

        // Output type must be set before the input type.
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, cfg_.bitrate);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase);
        out->SetUINT32(MF_MT_MPEG2_LEVEL, 60);  // level 6.0: 4096x2240 @ 72 exceeds 5.2's MB rate
        if (FAILED(hr = mft_->SetOutputType(0, out.Get(), 0))) {
            // Some MFTs don't know ConstrainedBase or level 6.0; Baseline + their own level choice.
            out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
            out->DeleteItem(MF_MT_MPEG2_LEVEL);
            if (FAILED(hr = mft_->SetOutputType(0, out.Get(), 0))) return fail(why, hr_text("SetOutputType", hr));
            profile_note_ = "Baseline (ConstrainedBase/level 6.0 refused)";
        }

        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(hr = mft_->SetInputType(0, in.Get(), 0))) return fail(why, hr_text("SetInputType", hr));

        // Slices: some MFTs only accept these after the media types are set.
        if (cfg_.slices > 0) {
            int rows = cfg_.height / 16 / cfg_.slices;
            int mbs = rows * cfg_.width / 16;
            if (set_codec(CODECAPI_AVEncSliceControlMode, 2) && set_codec(CODECAPI_AVEncSliceControlSize, rows))
                slice_note_ = "SliceControlMode=2 (MB rows) size " + std::to_string(rows);
            else if (set_codec(CODECAPI_AVEncSliceControlMode, 0) && set_codec(CODECAPI_AVEncSliceControlSize, mbs))
                slice_note_ = "SliceControlMode=0 (MBs) size " + std::to_string(mbs);
            else
                slice_note_ = "slice control refused";
        }

        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(0, &info);
        provides_samples_ = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        out_size_ = std::max<DWORD>(info.cbSize, DWORD(cfg_.width) * cfg_.height);

        if (async_) {
            ComPtr<IMFMediaEventGenerator> gen;
            if (FAILED(hr = mft_.As(&gen))) return fail(why, hr_text("IMFMediaEventGenerator", hr));
            gen_ = gen;
            events_ = new EventQueue(gen_.Get());
            if (FAILED(hr = events_->start())) return fail(why, hr_text("BeginGetEvent", hr));
        }
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        name_ += ", " + (slice_note_.empty() ? std::string("no slice control") : slice_note_);
        if (!profile_note_.empty()) name_ += ", " + profile_note_;
        if (dxgi_manager_set_) name_ += ", D3D11";
        return true;
    }

    bool set_codec(const GUID& key, uint32_t value, VARTYPE vt = VT_UI4) {
        VARIANT v;
        VariantInit(&v);
        v.vt = vt;
        if (vt == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else v.ulVal = value;
        return SUCCEEDED(codec_->SetValue(&key, &v));
    }

    bool submit(IMFMediaBuffer* buffer, int64_t ts, bool force_idr, EncodedFrame& out) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = submit_once(buffer, ts, force_idr || frames_ == 0, out);
        // A slice over the cap would end the headset session. Re-encode the same picture as an
        // IDR (the rejected frame is already the encoder's reference, the headset never gets it)
        // under a QP floor: +6 QP roughly halves the size. The floor is lifted afterwards.
        if (ok && largest(out) > cfg_.max_slice_bytes) {
            const size_t first = largest(out);
            int qp = std::max(max_qp(out), 20);
            double ratio = double(first) / double(cfg_.max_slice_bytes);
            int floor = std::min(51, qp + int(std::ceil(6 * std::log2(ratio))) + 2);
            for (int attempt = 1; ok && largest(out) > cfg_.max_slice_bytes; attempt++) {
                if (attempt > 4 || !set_codec(CODECAPI_AVEncVideoMinQP, uint32_t(floor))) {
                    set_codec(CODECAPI_AVEncVideoMinQP, 0);
                    return fail(nullptr, "slice of " + std::to_string(largest(out)) + " B over the " +
                                             std::to_string(cfg_.max_slice_bytes) + " B cap at QP floor " +
                                             std::to_string(floor));
                }
                ok = submit_once(buffer, ts + 100 * attempt, true, out);  // 1 MF tick later
                out.refits = attempt;
                floor = std::min(51, floor + 6);
            }
            set_codec(CODECAPI_AVEncVideoMinQP, 0);
            out.first_try_bytes = first;
        }
        out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return ok;
    }

    static size_t largest(const EncodedFrame& f) {
        size_t n = 0;
        for (auto& s : f.slices) n = std::max(n, s.nal.size());
        return n;
    }

    static int max_qp(const EncodedFrame& f) {
        int qp = -1;
        for (auto& s : f.slices) qp = std::max(qp, s.qp);
        return qp;
    }

    bool submit_once(IMFMediaBuffer* buffer, int64_t ts, bool key, EncodedFrame& out) {
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer);
        sample->SetSampleTime(ts / 100);
        sample->SetSampleDuration(10'000'000 / cfg_.fps);

        if (key) set_codec(CODECAPI_AVEncVideoForceKeyFrame, 1);
        out = EncodedFrame{};
        out.timestamp_ns = ts;
        bool ok = async_ ? submit_async(sample.Get(), out) : submit_sync(sample.Get(), out);
        frames_++;
        return ok;
    }

    bool submit_sync(IMFSample* sample, EncodedFrame& out) {
        HRESULT hr = mft_->ProcessInput(0, sample, 0);
        if (FAILED(hr)) return fail(nullptr, hr_text("ProcessInput", hr));
        for (;;) {
            bool got = false;
            if (!drain_one(&got, out)) return false;
            if (!got) break;
            if (!out.slices.empty()) break;
        }
        return out.slices.empty() ? fail(nullptr, "encoder buffered the frame (no output)") : true;
    }

    bool submit_async(IMFSample* sample, EncodedFrame& out) {
        using namespace std::chrono_literals;
        HRESULT status = S_OK;
        while (need_input_ == 0) {
            MediaEventType t = events_->wait(2000ms, &status);
            if (!t) return fail(nullptr, "timeout waiting for METransformNeedInput");
            if (t == METransformNeedInput) need_input_++;
            else if (t == METransformHaveOutput && !drain_one(nullptr, out)) return false;
            else if (t == MEError) return fail(nullptr, hr_text("MFT MEError", status));
        }
        HRESULT hr = mft_->ProcessInput(0, sample, 0);
        if (FAILED(hr)) return fail(nullptr, hr_text("ProcessInput", hr));
        need_input_--;
        while (out.slices.empty()) {
            MediaEventType t = events_->wait(2000ms, &status);
            if (!t) return fail(nullptr, "timeout waiting for METransformHaveOutput");
            if (t == METransformNeedInput) need_input_++;
            else if (t == METransformHaveOutput && !drain_one(nullptr, out)) return false;
            else if (t == MEError) return fail(nullptr, hr_text("MFT MEError", status));
        }
        return true;
    }

    // ProcessOutput once and append its NALs to `out`. *got = false when the MFT needs more input.
    bool drain_one(bool* got, EncodedFrame& out) {
        MFT_OUTPUT_DATA_BUFFER db{};
        ComPtr<IMFSample> own;
        if (!provides_samples_) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateMemoryBuffer(out_size_, &b);
            MFCreateSample(&own);
            own->AddBuffer(b.Get());
            db.pSample = own.Get();
        }
        DWORD st = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &db, &st);
        if (db.pEvents) db.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (got) *got = false;
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &t))) mft_->SetOutputType(0, t.Get(), 0);
            if (got) *got = true;
            return true;
        }
        if (FAILED(hr)) return fail(nullptr, hr_text("ProcessOutput", hr));
        if (got) *got = true;
        ComPtr<IMFSample> s;
        s.Attach(db.pSample);
        if (!provides_samples_) s->AddRef();  // keep `own`'s reference balanced
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(hr = s->ConvertToContiguousBuffer(&buf))) return fail(nullptr, hr_text("ConvertToContiguousBuffer", hr));
        BYTE* data = nullptr;
        DWORD len = 0;
        buf->Lock(&data, nullptr, &len);
        parse_output({data, len}, out);
        buf->Unlock();
        return true;
    }

    void parse_output(std::span<const uint8_t> es, EncodedFrame& out) {
        Bytes sps, pps;
        for (auto nal : h264::split_annexb(es)) {
            switch (h264::nal_type(nal)) {
            case h264::kSps:
                sps.assign(nal.begin(), nal.end());
                h264::set_fixed_frame_rate(sps);  // NVIDIA writes fixed_frame_rate_flag 0 at a constant rate
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
                if (!have_ps_ && !csd_.empty()) have_ps_ = h264::parse_csd(csd_, sps_, pps_);
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
        } else if (csd_.empty()) {
            csd_from_media_type();
        }
        out.csd = csd_;
    }

    void csd_from_media_type() {
        ComPtr<IMFMediaType> t;
        UINT32 size = 0;
        if (FAILED(mft_->GetOutputCurrentType(0, &t)) || FAILED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)))
            return;
        Bytes blob(size);
        t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, nullptr);
        for (auto nal : h264::split_annexb(blob)) {
            Bytes n(nal.begin(), nal.end());
            if (h264::nal_type(n) == h264::kSps) h264::set_fixed_frame_rate(n);
            h264::append_annexb(csd_, n);
        }
    }

    // BGRA (or any RGB) texture -> internal NV12 texture with the D3D11 video processor.
    bool convert_on_gpu(ID3D11Texture2D* src, unsigned sub, const D3D11_TEXTURE2D_DESC& desc) {
        HRESULT hr;
        if (!vp_) {
            ComPtr<ID3D11DeviceContext> ctx;
            device_->GetImmediateContext(&ctx);
            if (FAILED(hr = device_.As(&vdev_)) || FAILED(hr = ctx.As(&vctx_)))
                return fail(nullptr, hr_text("ID3D11VideoDevice", hr));
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = desc.Width;
            cd.InputHeight = desc.Height;
            cd.OutputWidth = cfg_.width;
            cd.OutputHeight = cfg_.height;
            cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
            if (FAILED(hr = vdev_->CreateVideoProcessorEnumerator(&cd, &vp_enum_)) ||
                FAILED(hr = vdev_->CreateVideoProcessor(vp_enum_.Get(), 0, &vp_)))
                return fail(nullptr, hr_text("CreateVideoProcessor", hr));
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = cfg_.width;
            nd.Height = cfg_.height;
            nd.MipLevels = 1;
            nd.ArraySize = 1;
            nd.Format = DXGI_FORMAT_NV12;
            nd.SampleDesc.Count = 1;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
            if (FAILED(hr = device_->CreateTexture2D(&nd, nullptr, &nv12_))) {
                nd.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (FAILED(hr = device_->CreateTexture2D(&nd, nullptr, &nv12_)))
                    return fail(nullptr, hr_text("CreateTexture2D(NV12)", hr));
            }
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(hr = vdev_->CreateVideoProcessorOutputView(nv12_.Get(), vp_enum_.Get(), &od, &vp_out_)))
                return fail(nullptr, hr_text("CreateVideoProcessorOutputView", hr));
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};   // full-range RGB
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
            out_cs.YCbCr_Matrix = 1;                     // BT.709
            out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in_cs);
            vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out_cs);
        }
        if (src != vp_src_tex_ || sub != vp_src_sub_) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
            id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            id.Texture2D.ArraySlice = sub;
            vp_in_.Reset();
            if (FAILED(hr = vdev_->CreateVideoProcessorInputView(src, vp_enum_.Get(), &id, &vp_in_)))
                return fail(nullptr, hr_text("CreateVideoProcessorInputView", hr));
            vp_src_tex_ = src;
            vp_src_sub_ = sub;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = vp_in_.Get();
        if (FAILED(hr = vctx_->VideoProcessorBlt(vp_.Get(), vp_out_.Get(), 0, 1, &stream)))
            return fail(nullptr, hr_text("VideoProcessorBlt", hr));
        return true;
    }

    EncoderConfig cfg_;
    std::string name_, error_, slice_note_, profile_note_;
    bool mf_started_ = false;
    bool async_ = false;
    bool provides_samples_ = false;
    DWORD out_size_ = 0;
    int need_input_ = 0;
    uint64_t frames_ = 0;
    Bytes csd_;
    h264::Sps sps_;  // in effect, for slice QPs
    h264::Pps pps_;
    bool have_ps_ = false;

    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFMediaEventGenerator> gen_;
    EventQueue* events_ = nullptr;
    ComPtr<IMFMediaBuffer> cpu_buffer_;

    ComPtr<ID3D11Device> device_;
    ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    UINT reset_token_ = 0;
    bool dxgi_manager_set_ = false;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorOutputView> vp_out_;
    ComPtr<ID3D11VideoProcessorInputView> vp_in_;
    ID3D11Texture2D* vp_src_tex_ = nullptr;
    unsigned vp_src_sub_ = ~0u;
    ComPtr<ID3D11Texture2D> nv12_;
};

}  // namespace

std::unique_ptr<Encoder> create_encoder(const EncoderConfig& config, std::string* error) {
    if (config.layout != Layout::Slices) {
        if (error) *error = "Media Foundation backend only encodes Layout::Slices";
        return nullptr;
    }
    if (config.codec != Codec::H264) {
        if (error) *error = "Media Foundation backend only encodes H.264";
        return nullptr;
    }
    auto enc = std::make_unique<MfEncoder>(config);
    if (!enc->init(error)) return nullptr;
    return enc;
}

}  // namespace ql::enc
