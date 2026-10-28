// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// H.264 Annex-B helpers shared by all encoder backends (ITU-T H.264 7.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "encoder.hpp"

namespace qb::enc::h264 {

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
    int poc_type_bit = -1;          // RBSP bit offset of pic_order_cnt_type
    int poc_end_bit = -1;           // first bit after the POC fields
    int max_num_ref_frames = 0;
    int poc_type = 0;
    int log2_max_poc_lsb = 0;
    bool delta_pic_order_always_zero = false;
    int vui_flag_bit = -1;          // RBSP bit offset of vui_parameters_present_flag
    int fixed_frame_rate_bit = -1;  // RBSP bit offset of vui fixed_frame_rate_flag; -1 = no timing info
};

bool parse_sps(std::span<const uint8_t> nal, Sps& out);

// Set the VUI fixed_frame_rate_flag (encoders that can't be told the rate is constant).
// False if the SPS has no VUI timing info.
bool set_fixed_frame_rate(Bytes& sps_nal);

// Give an SPS without VUI one (encoders that write none, e.g. VideoToolbox): timing 1000 /
// (2000 * fps) fixed like Meta's, and bitstream_restriction with no reordering and a DPB of
// max_num_ref_frames, so the decoder outputs each picture at once. False if it already has VUI.
bool add_vui(Bytes& sps_nal, int fps);

// Baseline SPS -> Constrained Baseline (constraint_set0/1, what Meta sends) for encoders that
// write plain Baseline; they use no FMO/ASO/redundant slices. False if not profile 66.
bool set_constrained_baseline(Bytes& sps_nal);

// The PPS fields a slice header depends on (7.3.2.2), single slice group only.
struct Pps {
    int pic_init_qp = 26;
    bool cabac = false;
    bool bottom_field_pic_order = false;
    bool weighted_pred = false;
    bool redundant_pic_cnt = false;
    bool deblocking_control = false;  // deblocking_filter_control_present_flag
};

bool parse_pps(std::span<const uint8_t> nal, Pps& out);

// The Quest's bitstream parser (xr_video_codecs) rejects pic_order_cnt_type != 2. Rewrite a
// poc_type 0 SPS (VideoToolbox) to 2; its slices then need drop_poc_lsb with the ORIGINAL SPS.
// Valid while output order = decode order and no two non-reference pictures follow each other
// (no B frames, every P a reference). False for other poc types.
bool set_poc_type2(Bytes& sps_nal);

// Remove pic_order_cnt_lsb (and delta_pic_order_cnt_bottom) from a slice of a poc_type 0 stream
// whose SPS went through set_poc_type2. `sps` is the original (poc_type 0) SPS.
bool drop_poc_lsb(Bytes& slice_nal, const Sps& sps, const Pps& pps);

struct SliceHeader {
    int first_mb = 0;
    int slice_type = 0;  // 0 P, 1 B, 2 I (mod 5)
    int qp = -1;         // SliceQP_Y; -1 without SPS/PPS or for headers not handled (B, weighted pred)
    int deblock_idc = -1;  // disable_deblocking_filter_idc (0 = filter across slice edges); -1 = not parsed
};

// With sps and pps also reads the slice QP.
bool parse_slice_header(std::span<const uint8_t> nal, SliceHeader& out, const Sps* sps = nullptr,
                        const Pps* pps = nullptr);

// Parse the SPS and PPS out of an Annex-B csd. False if either is missing.
bool parse_csd(std::span<const uint8_t> csd, Sps& sps, Pps& pps);

// Append 00 00 00 01 + nal.
void append_annexb(Bytes& out, std::span<const uint8_t> nal);

}  // namespace qb::enc::h264
