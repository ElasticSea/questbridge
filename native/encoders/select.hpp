// Encoder selection: which backend, layout and settings to use on this machine.
//
// Platform-neutral so it can be tested with fake encoders. The platform code lists the encoders
// it found (Candidate), plan() orders them into attempts with per-vendor settings, and select()
// creates each attempt in turn, encodes one probe frame and keeps the first whose output has the
// layout the headset needs (check_frame). Every skipped attempt and why ends up in selection().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "encoder.hpp"

namespace qb::enc {

enum class Vendor { Unknown, Nvidia, Amd, Intel, Microsoft, Apple };

Vendor vendor_from_pci(uint32_t vendor_id);
// From an MFT friendly name or a hardware vendor id string ("VEN_10DE").
Vendor vendor_from_text(std::string_view text);
const char* vendor_name(Vendor v);  // "nvidia", "amd", ...

enum class Backend { MediaFoundation, OpenH264 };

struct Candidate {
    Backend backend = Backend::MediaFoundation;
    Vendor vendor = Vendor::Unknown;
    bool hardware = false;
    std::string name;            // friendly name
    int index = 0;               // backend-specific: position in the MFT enumeration
    bool on_gpu_adapter = true;  // on the adapter of EncoderConfig::gpu_device (or no device given)
};

// Per-vendor encoder settings that keep the stream format the headset needs.
struct VendorSettings {
    std::vector<Layout> layouts;     // in order of preference
    int quality_vs_speed = -1;       // CODECAPI_AVEncCommonQualityVsSpeed, -1 = leave the default
    bool deblock_off = true;         // CODECAPI_AVEnableInLoopDeblockFilter 0 (Slices: one decoder per strip)
    bool refit_min_qp = true;        // slice cap re-encode with CODECAPI_AVEncVideoMinQP
};

VendorSettings settings_for(const Candidate& c);

struct Attempt {
    Candidate candidate;
    Layout layout = Layout::Slices;
    VendorSettings settings;
    EncoderConfig config;  // the caller's, with layout resolved
    std::string tag;       // short backend id for logs and qb_info: "mf-nvidia", "openh264", ...
};

// Attempts in order: hardware on the device's adapter first (NVIDIA, AMD, Intel, others),
// then other hardware, then software (OpenH264, then the Microsoft MFT). config.backend
// narrows it to one kind; config.layout other than Auto to that layout; config.hardware /
// allow_software drop whole groups.
std::vector<Attempt> plan(const std::vector<Candidate>& found, const EncoderConfig& config);

// Smallest H.264 level (Table A-1, level_idc) for one picture of w x h at fps; 0 if none.
int level_for(int width, int height, int fps);

// Does a probe frame (forced IDR) have the format the headset needs? Slices: `slices` NALs
// in one picture, first_mb at equal MB-row bands, no deblocking across slice edges.
// Strips: `slices` pictures with their own SPS/PPS and first_mb 0. Both: Constrained Baseline
// (profile 66, constraint_set1), IDR, frame size matching the config.
bool check_frame(const EncodedFrame& f, const EncoderConfig& config, Layout layout, std::string* why);

using Factory = std::function<std::unique_ptr<Encoder>(const Attempt& a, std::string* error)>;

// Create attempts in order until one passes the probe. The result forces an IDR on its first
// frame (the probe was the encoder's first) and reports the attempt's tag in name().
std::unique_ptr<Encoder> select(const std::vector<Attempt>& attempts, const Factory& factory, std::string* error);

}  // namespace qb::enc
