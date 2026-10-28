// H.264 Annex-B helpers shared by all encoder backends (ITU-T H.264 7.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "encoder.hpp"

namespace ql::enc::h264 {

enum NalType : uint8_t { kSlice = 1, kIdr = 5, kSei = 6, kSps = 7, kPps = 8, kAud = 9 };

// Split an Annex-B stream into NAL payloads (start codes stripped).
std::vector<std::span<const uint8_t>> split_annexb(std::span<const uint8_t> es);

inline uint8_t nal_type(std::span<const uint8_t> nal) { return nal.empty() ? 0 : nal[0] & 0x1F; }

struct Sps {
    int profile = 0;
    int constraints = 0;  // constraint_set0..5 flags byte
    int level = 0;
    int width_mbs = 0;
    int height_mbs = 0;
    int log2_max_frame_num = 0;
    int fixed_frame_rate_bit = -1;  // RBSP bit offset of vui fixed_frame_rate_flag; -1 = no timing info
};

bool parse_sps(std::span<const uint8_t> nal, Sps& out);

// Set the VUI fixed_frame_rate_flag (encoders that can't be told the rate is constant).
// False if the SPS has no VUI timing info.
bool set_fixed_frame_rate(Bytes& sps_nal);

struct SliceHeader {
    int first_mb = 0;
    int slice_type = 0;  // 0 P, 1 B, 2 I (mod 5)
};

bool parse_slice_header(std::span<const uint8_t> nal, SliceHeader& out);

// Append 00 00 00 01 + nal.
void append_annexb(Bytes& out, std::span<const uint8_t> nal);

}  // namespace ql::enc::h264
