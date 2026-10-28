// Platform-neutral H.264 encoder interface for the Link video stream.
//
// The Quest expects H.264 Constrained Baseline, 4096x2240, one picture in 5 slices of
// 28 macroblock rows (first_mb_in_slice 0/7168/14336/21504/28672), SPS+PPS as csd.
// Backends: Media Foundation (Windows, mf/), VideoToolbox (macOS, later).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ql::enc {

using Bytes = std::vector<uint8_t>;

enum class PixelFormat { NV12, BGRA };

struct EncoderConfig {
    int width = 4096;
    int height = 2240;
    int fps = 72;
    int slices = 5;                    // equal bands of MB rows; 0 = encoder default
    uint32_t bitrate = 100'000'000;    // bits/s
    bool cbr = true;                   // false = peak-constrained VBR (peak 1.5x)
    int gop = 72;                      // IDR interval in frames; 1 = all intra, 0 = first frame only
    bool hardware = true;              // try the GPU encoder first
    bool allow_software = true;        // fall back to a software encoder
    void* gpu_device = nullptr;        // ID3D11Device* (Windows) for texture input; null = CPU input only
};

// CPU frame. NV12: plane[0] = Y, plane[1] = interleaved UV. BGRA: plane[0] only.
struct CpuFrame {
    PixelFormat format = PixelFormat::NV12;
    const uint8_t* plane[2] = {};
    int stride[2] = {};
};

// GPU frame. Windows: ID3D11Texture2D* (NV12 or BGRA) on config.gpu_device.
// macOS (later): CVPixelBufferRef.
struct GpuFrame {
    void* texture = nullptr;
    unsigned subresource = 0;
};

struct Slice {
    int index = 0;      // position in the picture, 0..slices-1
    int first_mb = 0;   // first_mb_in_slice from the slice header
    Bytes nal;          // Annex-B: 00 00 00 01 + slice NAL
};

struct EncodedFrame {
    Bytes csd;                 // Annex-B SPS + PPS currently in effect
    std::vector<Slice> slices;
    bool idr = false;
    int64_t timestamp_ns = 0;
    double encode_ms = 0;      // submit-to-output wall time
};

class Encoder {
public:
    virtual ~Encoder() = default;

    // Encode one frame and block until its output is available.
    virtual bool encode(const CpuFrame& frame, int64_t timestamp_ns, bool force_idr, EncodedFrame& out) = 0;
    virtual bool encode(const GpuFrame& frame, int64_t timestamp_ns, bool force_idr, EncodedFrame& out) = 0;

    virtual std::string name() const = 0;           // backend + device, for logs
    virtual const std::string& error() const = 0;   // last failure
};

// Create the platform backend. Returns null and sets *error on failure.
std::unique_ptr<Encoder> create_encoder(const EncoderConfig& config, std::string* error);

}  // namespace ql::enc
