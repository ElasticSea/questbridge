// Graphics texture -> H.264 slices, for ql_submit_frame_texture.
//
// capture() runs on the graphics (render) thread and must finish copying before it returns,
// because the engine reuses the texture next frame. encode() runs on the runtime's encode
// thread. Backends: D3D11 + Media Foundation (Windows); Metal + VideoToolbox (macOS, to do).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xrsp.hpp"

namespace ql {

struct TextureEncoderConfig {
    int fps = 72;
    int slices = 5;
    uint32_t bitrate = 100'000'000;
    int gop = 1;  // 1 = all intra (what the headset is proven to play); >1 = IDR every gop frames
};

struct EncodedSlices {
    Bytes csd;                  // Annex-B SPS + PPS
    std::vector<Bytes> slices;  // Annex-B slice NALs, in picture order
    bool idr = false;
};

class TextureEncoder {
public:
    virtual ~TextureEncoder() = default;
    // Render thread: copy the texture into a free slot. Returns the slot, or -1 (frame dropped).
    virtual int capture(void* native_texture) = 0;
    // Encode thread: encode a captured slot and free it.
    virtual bool encode(int slot, bool force_idr, EncodedSlices& out) = 0;
    // Free a captured slot without encoding it.
    virtual void release(int slot) = 0;
    virtual std::string error() const = 0;
};

// Platform backend; null with *error set when unavailable.
std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig& cfg, std::string* error);

}  // namespace ql
