// macOS: draw the engine's Metal texture into one IOSurface-backed BGRA CVPixelBuffer per strip
// (render thread) and hand those buffers to the VideoToolbox strip sessions as they are (encode
// thread). The GPU does the split; nothing goes through the CPU.
//
// Under Unity the passes are encoded on Unity's current command buffer (IUnityGraphicsMetalV1
// from UnityPluginLoad), so they run after the frame's rendering. Without it (tests, other
// hosts) they go on our own queue, after work already committed on the texture's device.
// The encode thread waits for the buffer's completion handler before encoding a slot.
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "encoder.hpp"
#include "texture_encoder.hpp"

#if !__has_feature(objc_arc)
#  error texture_encoder_metal.mm needs -fobjc-arc
#endif

// The parts of Unity's plugin interface (PluginAPI/IUnityInterface.h, IUnityGraphicsMetal.h)
// this file uses, by layout.
struct IUnityInterfaces {
    void* (*GetInterface)(void*);
    void (*RegisterInterface)(void*, void*);
    void* (*GetInterfaceSplit)(unsigned long long high, unsigned long long low);
    void (*RegisterInterfaceSplit)(unsigned long long, unsigned long long, void*);
};
struct IUnityGraphicsMetalV1 {
    NSBundle* (*MetalBundle)();
    id<MTLDevice> (*MetalDevice)();
    id<MTLCommandBuffer> (*CurrentCommandBuffer)();
    id<MTLCommandEncoder> (*CurrentCommandEncoder)();
    void (*EndCurrentCommandEncoder)();
};
constexpr unsigned long long kUnityMetalV1High = 0x29F8F3D03833465EULL, kUnityMetalV1Low = 0x92138551C15D823DULL;

static std::atomic<IUnityGraphicsMetalV1*> g_unity_metal{nullptr};

extern "C" __attribute__((visibility("default"))) void UnityPluginLoad(IUnityInterfaces* unity) {
    if (unity && unity->GetInterfaceSplit)
        g_unity_metal = static_cast<IUnityGraphicsMetalV1*>(unity->GetInterfaceSplit(kUnityMetalV1High, kUnityMetalV1Low));
}

extern "C" __attribute__((visibility("default"))) void UnityPluginUnload() { g_unity_metal = nullptr; }

namespace ql {

namespace {

constexpr int kSlots = 3;
constexpr int kMaxStrips = 8;

// Full-screen triangle; each fragment reads its texel from the strip's rows of the source.
// read() decodes sRGB and the sRGB target re-encodes it, so the bytes pass through unchanged.
const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; };
vertex V ql_strip_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    V o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
fragment half4 ql_strip_fs(V in [[stage_in]], texture2d<half> src [[texture(0)]], constant uint& y0 [[buffer(0)]]) {
    return src.read(uint2(in.pos.xy) + uint2(0, y0));
}
)";

bool is_srgb(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatBGRA10_XR_sRGB:
        case MTLPixelFormatBGR10_XR_sRGB: return true;
        default: return false;
    }
}

class MetalTextureEncoder final : public TextureEncoder {
public:
    explicit MetalTextureEncoder(const TextureEncoderConfig& cfg) : cfg_(cfg) {}

    ~MetalTextureEncoder() override {
        for (auto& s : slots_)
            for (auto* b : s.buffers)
                if (b) CVPixelBufferRelease(b);
    }

    int capture(void* native_texture) override {
        id<MTLTexture> src = (__bridge id<MTLTexture>)native_texture;
        if (!src) return -1;
        if (!device_ && !init(src)) return -1;
        if (src.device != device_ || int(src.width) != width_ || int(src.height) != height_ ||
            is_srgb(src.pixelFormat) != srgb_ || src.textureType != MTLTextureType2D) {
            set_error("texture changed device, size, format or type; frames dropped");
            return -1;
        }
        int slot = -1;
        {
            // A dropped slot's passes may still be in flight; skip it until they finish
            std::lock_guard<std::mutex> l(gpu_mu_);
            for (int i = 0; i < kSlots && slot < 0; ++i) {
                int expected = kFree;
                if (gpu_[i] != kGpuPending && state_[i].compare_exchange_strong(expected, kCapturing)) slot = i;
            }
        }
        if (slot < 0) return -1;  // encoder is behind: drop this frame

        id<MTLCommandBuffer> cb = nil;
        IUnityGraphicsMetalV1* unity = g_unity_metal;
        if (unity && unity->MetalDevice() == device_) {
            unity->EndCurrentCommandEncoder();
            cb = unity->CurrentCommandBuffer();
        }
        const bool own = cb == nil;
        if (own) cb = [queue_ commandBuffer];
        for (int i = 0; i < strips_; ++i) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = slots_[slot].targets[i];
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:pipeline_];
            [e setFragmentTexture:src atIndex:0];
            uint32_t y0 = uint32_t(i * strip_h_);
            [e setFragmentBytes:&y0 length:sizeof y0 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        }
        {
            std::lock_guard<std::mutex> l(gpu_mu_);
            gpu_[slot] = kGpuPending;
        }
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            {
                std::lock_guard<std::mutex> l(gpu_mu_);
                gpu_[slot] = done.status == MTLCommandBufferStatusCompleted ? kGpuDone : kGpuFailed;
            }
            gpu_cv_.notify_all();
        }];
        if (own) [cb commit];
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

    std::string name() const override { return encoder_ ? encoder_->name() : ""; }

    std::string error() const override {
        std::lock_guard<std::mutex> l(err_mu_);
        return error_;
    }

private:
    enum { kFree, kCapturing, kCaptured };
    enum { kGpuDone, kGpuPending, kGpuFailed };

    struct Slot {
        CVPixelBufferRef buffers[kMaxStrips] = {};
        id<MTLTexture> targets[kMaxStrips];
    };

    TextureEncoderConfig cfg_;
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLRenderPipelineState> pipeline_;
    int width_ = 0, height_ = 0, strips_ = 0, strip_h_ = 0;
    bool srgb_ = false;
    Slot slots_[kSlots];
    std::atomic<int> state_[kSlots] = {};
    std::mutex gpu_mu_;
    std::condition_variable gpu_cv_;
    int gpu_[kSlots] = {};
    std::unique_ptr<enc::Encoder> encoder_;  // encode thread only
    mutable std::mutex err_mu_;
    std::string error_;

    void set_error(std::string e) {
        std::lock_guard<std::mutex> l(err_mu_);
        error_ = std::move(e);
    }

    bool init(id<MTLTexture> src) {
        width_ = int(src.width);
        height_ = int(src.height);
        strips_ = std::clamp(cfg_.slices, 1, kMaxStrips);
        strip_h_ = height_ / strips_;
        srgb_ = is_srgb(src.pixelFormat);
        if (width_ % 16 || height_ % (16 * strips_)) {
            set_error("texture size must split into " + std::to_string(strips_) + " strips of whole macroblock rows");
            return false;
        }
        id<MTLDevice> dev = src.device;
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kShader) options:nil error:&err];
        if (!lib) {
            set_error(std::string("Metal shader: ") + (err ? err.localizedDescription.UTF8String : "?"));
            return false;
        }
        const MTLPixelFormat fmt = srgb_ ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"ql_strip_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"ql_strip_fs"];
        pd.colorAttachments[0].pixelFormat = fmt;
        pipeline_ = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pipeline_) {
            set_error(std::string("Metal pipeline: ") + (err ? err.localizedDescription.UTF8String : "?"));
            return false;
        }
        NSDictionary* attrs = @{
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        };
        for (auto& s : slots_) {
            for (int i = 0; i < strips_; ++i) {
                if (CVPixelBufferCreate(nullptr, size_t(width_), size_t(strip_h_), kCVPixelFormatType_32BGRA,
                                        (__bridge CFDictionaryRef)attrs, &s.buffers[i]) != kCVReturnSuccess) {
                    set_error("CVPixelBufferCreate for the strip pool failed");
                    return false;
                }
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                             width:NSUInteger(width_)
                                                                                            height:NSUInteger(strip_h_)
                                                                                         mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModeShared;
                s.targets[i] = [dev newTextureWithDescriptor:td iosurface:CVPixelBufferGetIOSurface(s.buffers[i]) plane:0];
                if (!s.targets[i]) {
                    set_error("MTLTexture over the strip IOSurface failed");
                    return false;
                }
            }
        }
        queue_ = [dev newCommandQueue];
        device_ = dev;
        return true;
    }

    bool wait_gpu(int slot) {
        std::unique_lock<std::mutex> l(gpu_mu_);
        if (!gpu_cv_.wait_for(l, std::chrono::seconds(1), [&] { return gpu_[slot] != kGpuPending; })) {
            set_error("GPU strip copy did not complete");
            return false;
        }
        if (gpu_[slot] == kGpuFailed) {
            set_error("GPU strip copy failed");
            return false;
        }
        return true;
    }

    bool encode_slot(int slot, bool force_idr, EncodedSlices& out) {
        if (!wait_gpu(slot)) return false;
        if (!encoder_) {
            enc::EncoderConfig ec;
            ec.width = width_;
            ec.height = height_;
            ec.fps = cfg_.fps;
            ec.slices = strips_;
            ec.layout = enc::Layout::Strips;
            ec.bitrate = cfg_.bitrate;
            ec.gop = cfg_.gop;
            ec.max_slice_bytes = cfg_.max_slice_bytes;
            ec.codec = cfg_.hevc ? enc::Codec::HEVC : enc::Codec::H264;
            std::string err;
            encoder_ = enc::create_encoder(ec, &err);
            if (!encoder_) {
                set_error("encoder: " + err);
                return false;
            }
        }
        void* strips[kMaxStrips];
        for (int i = 0; i < strips_; ++i) strips[i] = slots_[slot].buffers[i];
        enc::GpuFrame f;
        f.strips = strips;
        f.strip_count = strips_;
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
        for (auto& s : ef.slices) {
            out.largest = std::max(out.largest, s.nal.size());
            out.qp = std::max(out.qp, s.qp);
            out.slices.push_back(std::move(s.nal));
            out.slice_csd.push_back(std::move(s.csd));
        }
        if (int(out.slices.size()) != strips_) {
            set_error("encoder produced " + std::to_string(out.slices.size()) + " slices");
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig& cfg, std::string*) {
    return std::make_unique<MetalTextureEncoder>(cfg);
}

}  // namespace ql
