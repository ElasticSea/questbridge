// OpenH264 software encoder (Cisco's BSD-2 encoder), the fallback when no hardware encoder
// produces the headset's format. One picture, one slice, Constrained Baseline; wrap it in
// make_strip_encoder for Layout::Strips. The DLL is Cisco's prebuilt binary, loaded at run
// time (fetch.ps1 puts it in %LOCALAPPDATA%\QuestBridge\openh264; QB_OPENH264 overrides the path).
#pragma once

#include <memory>
#include <string>

#include "../encoder.hpp"

namespace qb::enc {

// Loads the library once. False (with *why) if it is missing or another version than our headers.
bool openh264_available(std::string* why);

// config.slices must be 1 (or 0). CpuFrame input only.
std::unique_ptr<Encoder> create_openh264(const EncoderConfig& config, std::string* error);

}  // namespace qb::enc
