// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Graphics texture -> H.264 / HEVC slices, for qb_submit_frame_texture.
//
// capture() runs on the graphics (render) thread and must finish copying before it returns,
// because the engine reuses the texture next frame. encode() runs on the runtime's encode
// thread. Backends: D3D11 + Media Foundation (Windows); Metal + VideoToolbox (macOS).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xrsp.hpp"

namespace qb {

struct TextureEncoderConfig {
    int fps = 72;
    int slices = 5;
    uint32_t bitrate = 100'000'000;
    int gop = 1;  // 1 = all intra (what the headset is proven to play); >1 = IDR every gop frames
    bool hevc = false;  // the session's CONFIG codec; VideoToolbox only
    size_t max_slice_bytes = 100'000;  // the headset's slice cap (qb_device_info.max_slice_bytes)
};

struct EncodedSlices {
    Bytes csd;                  // Annex-B SPS + PPS (HEVC: VPS + SPS + PPS)
    std::vector<Bytes> slices;  // Annex-B slice NALs, in picture order
    std::vector<Bytes> slice_csd;  // per slice when each is its own stream (strips); empty = csd
    bool idr = false;
    int refits = 0;               // re-encodes to fit the per-message slice cap (see enc::EncoderConfig)
    size_t first_try_bytes = 0;   // refits > 0: largest slice of the rejected encode
    size_t largest = 0;           // largest slice NAL sent
    int qp = -1;                  // largest slice QP, -1 = unknown
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
    // Encoder backend in use ("" until the first encode() created it), and the ones skipped
    virtual std::string name() const { return {}; }
    virtual std::string selection() const { return {}; }
};

// The texture encoder's backend for qb_info::encoder (process-wide; the runtime sets it).
void set_encoder_info(const std::string& name);
std::string encoder_info();

// Platform backend; null with *error set when unavailable.
std::unique_ptr<TextureEncoder> create_texture_encoder(const TextureEncoderConfig& cfg, std::string* error);

}  // namespace qb
