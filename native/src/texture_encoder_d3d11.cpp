// Windows: copy the engine's D3D11 texture on its immediate context (render thread), then
// encode the copy with the Media Foundation encoder on the same device (encode thread). The
// device is made multithread-protected, so both threads' context calls are serialized and the
// GPU sees copy before convert/encode.
#include <d3d11.h>
#include <d3d10.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "encoder.hpp"
#include "texture_encoder.hpp"

using Microsoft::WRL::ComPtr;

namespace ql {

namespace {

constexpr int kSlots = 3;

// Copy targets use the UNORM member of the source's format group (sRGB bytes stay as they
// are, which is what the video wants; the video processor needs a typed format)
DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return f;
    }
}

class D3D11TextureEncoder final : public TextureEncoder {
public:
    explicit D3D11TextureEncoder(const TextureEncoderConfig& cfg) : cfg_(cfg) {}

    int capture(void* native_texture) override {
        auto* src = static_cast<ID3D11Texture2D*>(native_texture);
        if (!src) return -1;
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (!device_ && !init(src, sd)) return -1;
        ComPtr<ID3D11Device> dev;
        src->GetDevice(&dev);
        if (dev.Get() != device_.Get() || sd.Width != desc_.Width || sd.Height != desc_.Height ||
            unorm_of(sd.Format) != desc_.Format) {
            set_error("texture changed device, size or format; frames dropped");
            return -1;
        }
        int slot = -1;
        for (int i = 0; i < kSlots && slot < 0; ++i) {
            int expected = kFree;
            if (state_[i].compare_exchange_strong(expected, kCapturing)) slot = i;
        }
        if (slot < 0) return -1;  // encoder is behind: drop this frame
        if (sd.SampleDesc.Count > 1)
            ctx_->ResolveSubresource(slots_[slot].Get(), 0, src, 0, desc_.Format);
        else
            ctx_->CopySubresourceRegion(slots_[slot].Get(), 0, 0, 0, 0, src, 0, nullptr);
        state_[slot] = kCaptured;
        return slot;
    }

    bool encode(int slot, bool force_idr, EncodedSlices& out) override {
        if (slot < 0 || slot >= kSlots || state_[slot] != kCaptured) return false;
        bool ok = encode_slot(slot, force_idr, out);
        state_[slot] = kFree;
        return ok;
    }

    void release(int slot) override {
        if (slot >= 0 && slot < kSlots) state_[slot] = kFree;
    }

    std::string error() const override {
        std::lock_guard<std::mutex> l(err_mu_);
        return error_;
    }

    // Encode thread only (the encoder is created there)
    std::string name() const override { return encoder_ ? encoder_->name() : ""; }
    std::string selection() const override { return encoder_ ? encoder_->selection() : ""; }

private:
    enum { kFree, kCapturing, kCaptured };

    TextureEncoderConfig cfg_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    D3D11_TEXTURE2D_DESC desc_{};
    ComPtr<ID3D11Texture2D> slots_[kSlots];
    std::atomic<int> state_[kSlots] = {};
    std::unique_ptr<enc::Encoder> encoder_;  // encode thread only
    mutable std::mutex err_mu_;
    std::string error_;

    void set_error(std::string e) {
        std::lock_guard<std::mutex> l(err_mu_);
        error_ = std::move(e);
    }

    bool init(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd) {
        src->GetDevice(&device_);
        device_->GetImmediateContext(&ctx_);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt)) || SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        desc_ = {};
        desc_.Width = sd.Width;
        desc_.Height = sd.Height;
        desc_.MipLevels = 1;
        desc_.ArraySize = 1;
        desc_.Format = unorm_of(sd.Format);
        desc_.SampleDesc.Count = 1;
        desc_.Usage = D3D11_USAGE_DEFAULT;
        desc_.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& t : slots_) {
            if (FAILED(device_->CreateTexture2D(&desc_, nullptr, &t))) {
                set_error("CreateTexture2D for the capture pool failed");
                device_.Reset();
                return false;
            }
        }
        return true;
    }

    bool encode_slot(int slot, bool force_idr, EncodedSlices& out) {
        if (!encoder_) {
            enc::EncoderConfig ec;
            ec.width = int(desc_.Width);
            ec.height = int(desc_.Height);
            ec.fps = cfg_.fps;
            ec.slices = cfg_.slices;
            ec.bitrate = cfg_.bitrate;
            ec.gop = cfg_.gop;
            ec.gpu_device = device_.Get();
            ec.codec = cfg_.hevc ? enc::Codec::HEVC : enc::Codec::H264;  // HEVC: refused below
            // Backend and layout per machine (GPU vendor, probe frame); QL_ENCODER forces one:
            // nvidia | amd | intel | mf | mf-software | openh264 | software
            ec.layout = enc::Layout::Auto;
            if (const char* b = std::getenv("QL_ENCODER"); b && *b) ec.backend = b;
            std::string err;
            encoder_ = enc::create_encoder(ec, &err);
            if (!encoder_) {
                set_error("encoder: " + err);
                return false;
            }
        }
        enc::GpuFrame f;
        f.texture = slots_[slot].Get();
        enc::EncodedFrame ef;
        int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
        if (!encoder_->encode(f, ts, force_idr, ef)) {
            set_error("encode: " + encoder_->error());
            return false;
        }
        std::sort(ef.slices.begin(), ef.slices.end(), [](auto& a, auto& b) { return a.index < b.index; });
        out.csd = std::move(ef.csd);
        out.idr = ef.idr;
        out.refits = ef.refits;
        out.first_try_bytes = ef.first_try_bytes;
        out.largest = 0;
        out.qp = -1;
        out.slices.clear();
        out.slice_csd.clear();
        const bool strips = encoder_->layout() == enc::Layout::Strips;
        for (auto& s : ef.slices) {
            out.largest = (std::max)(out.largest, s.nal.size());
            out.qp = (std::max)(out.qp, s.qp);
            out.slices.push_back(std::move(s.nal));
            if (strips) out.slice_csd.push_back(std::move(s.csd));  // each strip its own stream
        }
        if (cfg_.slices && int(out.slices.size()) != cfg_.slices) {
            set_error("encoder produced " + std::to_string(out.slices.size()) + " slices");
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig& cfg, std::string*) {
    return std::make_unique<D3D11TextureEncoder>(cfg);
}

}  // namespace ql
