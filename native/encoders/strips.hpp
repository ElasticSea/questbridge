// Layout::Strips out of single-picture encoders: one encoder per width x (height/slices) strip,
// encoded in parallel, each strip its own stream (own SPS/PPS, first_mb 0). Used by Media
// Foundation (hardware without usable slice control) and OpenH264.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "encoder.hpp"

namespace ql::enc {

// Creates the encoder for one strip; `config` has the strip's size, slices 1, Layout::Slices.
using StripFactory = std::function<std::unique_ptr<Encoder>(const EncoderConfig& config, int strip, std::string* error)>;

// GpuFrame input, platform-specific: split into one GpuFrame per strip (hardware encoders
// taking textures), or read back to a CPU frame of the full size. Null = GpuFrame unsupported.
struct GpuInput {
    std::function<bool(const GpuFrame& in, std::vector<GpuFrame>& strips, std::string* error)> split;
    std::function<bool(const GpuFrame& in, CpuFrame& out, std::string* error)> read_back;
};

std::unique_ptr<Encoder> make_strip_encoder(const EncoderConfig& config, const StripFactory& factory,
                                            GpuInput gpu, std::string* error);

}  // namespace ql::enc
