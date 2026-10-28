// D3D11 texture input for encoders that can't take the whole texture: split it into strip
// textures (hardware MFTs per strip), or read it back to CPU memory (software encoders, or a
// hardware MFT on another adapter than the texture's device).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../encoder.hpp"
#include "../strips.hpp"

namespace ql::enc {

class D3D11Input {
public:
    D3D11Input();
    ~D3D11Input();
    // Copies of `strips` equal horizontal bands, same format, on the texture's device.
    bool split(const GpuFrame& in, int strips, std::vector<GpuFrame>& out, std::string* error);
    // CPU view of a mapped staging copy (BGRA, RGBA or NV12 as the texture). Valid until the next call.
    bool read_back(const GpuFrame& in, CpuFrame& out, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GpuInput for make_strip_encoder backed by one shared D3D11Input.
GpuInput d3d11_split_input(int strips);
GpuInput d3d11_read_back_input();

}  // namespace ql::enc
