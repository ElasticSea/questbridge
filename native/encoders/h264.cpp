#include "h264.hpp"

#include <algorithm>

namespace ql::enc::h264 {

namespace {

// RBSP of a NAL (header byte dropped, emulation prevention removed), at most `limit` bytes.
Bytes rbsp(std::span<const uint8_t> nal, size_t limit) {
    Bytes out;
    int zeros = 0;
    for (size_t i = 1; i < nal.size() && out.size() < limit; i++) {
        uint8_t b = nal[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

class Bits {
public:
    explicit Bits(const Bytes& d) : d_(d) {}
    bool ok() const { return ok_; }
    size_t pos() const { return p_; }

    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if ((p_ >> 3) >= d_.size()) {
                ok_ = false;
                return 0;
            }
            v = (v << 1) | ((d_[p_ >> 3] >> (7 - (p_ & 7))) & 1);
            p_++;
        }
        return v;
    }

    uint32_t ue() {
        int z = 0;
        while (ok_ && u(1) == 0 && z < 32) z++;
        return (1u << z) - 1 + u(z);
    }

    int32_t se() {
        uint32_t v = ue();
        return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2);
    }

private:
    const Bytes& d_;
    size_t p_ = 0;
    bool ok_ = true;
};

class BitWriter {
public:
    void u(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if (bits_ % 8 == 0) d_.push_back(0);
            if ((v >> i) & 1) d_.back() |= uint8_t(0x80 >> (bits_ % 8));
            bits_++;
        }
    }
    void ue(uint32_t v) {
        int len = 0;
        while ((v + 1) >> (len + 1)) len++;
        u(0, len);
        u(v + 1, len + 1);
    }
    const Bytes& bytes() const { return d_; }  // zero-padded to a byte

private:
    Bytes d_;
    size_t bits_ = 0;
};

// NAL from header byte + RBSP (7.4.1): 00 00 0x with x <= 3 gets an emulation_prevention_three_byte.
Bytes escape(uint8_t header, const Bytes& data) {
    Bytes out{header};
    int zeros = 0;
    for (uint8_t b : data) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

}  // namespace

std::vector<std::span<const uint8_t>> split_annexb(std::span<const uint8_t> es) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 2 < es.size(); i++) {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
            starts.push_back(i + 3);
            i += 2;
        }
    }
    std::vector<std::span<const uint8_t>> nals;
    for (size_t k = 0; k < starts.size(); k++) {
        size_t s = starts[k];
        size_t e = k + 1 < starts.size() ? starts[k + 1] - 3 : es.size();
        while (e > s && es[e - 1] == 0) e--;  // trailing zeros / next 4-byte start code
        if (e > s) nals.push_back(es.subspan(s, e - s));
    }
    return nals;
}

bool parse_sps(std::span<const uint8_t> nal, Sps& out) {
    Bytes data = rbsp(nal, 256);
    Bits b(data);
    out.profile = int(b.u(8));
    out.constraints = int(b.u(8));
    out.level = int(b.u(8));
    b.ue();  // seq_parameter_set_id
    switch (out.profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128:
    case 138: case 139: case 134: case 135:
        if (b.ue() == 3) b.u(1);
        b.ue();
        b.ue();
        b.u(1);
        if (b.u(1)) return false;  // scaling matrices not handled
        break;
    default:
        break;
    }
    out.log2_max_frame_num = int(b.ue()) + 4;
    uint32_t poc_type = b.ue();
    if (poc_type == 0) {
        b.ue();
    } else if (poc_type == 1) {
        b.u(1);
        b.se();
        b.se();
        for (uint32_t n = b.ue(), i = 0; i < n && b.ok(); i++) b.se();
    }
    out.max_num_ref_frames = int(b.ue());
    b.u(1);   // gaps_in_frame_num_value_allowed_flag
    out.width_mbs = int(b.ue()) + 1;
    int map_units = int(b.ue()) + 1;
    int frame_mbs_only = int(b.u(1));
    out.height_mbs = map_units * (2 - frame_mbs_only);
    if (!frame_mbs_only) b.u(1);
    b.u(1);  // direct_8x8_inference_flag
    if (b.u(1))
        for (int i = 0; i < 4; i++) b.ue();  // frame_crop offsets
    out.fixed_frame_rate_bit = -1;
    out.vui_flag_bit = int(b.pos());
    if (b.u(1)) {  // VUI (E.1.1) up to fixed_frame_rate_flag
        if (b.u(1) && b.u(8) == 255) b.u(32);  // aspect_ratio_idc, sar
        if (b.u(1)) b.u(1);                    // overscan
        if (b.u(1)) {                          // video_signal_type_present_flag
            b.u(3);                            // video_format
            b.u(1);                            // video_full_range_flag
            if (b.u(1)) b.u(24);               // colour description
        }
        if (b.u(1)) b.ue(), b.ue();            // chroma_sample_loc
        if (b.u(1)) {                          // timing_info_present_flag
            b.u(32);
            b.u(32);
            out.fixed_frame_rate_bit = int(b.pos());
        }
    }
    return b.ok();
}

bool set_fixed_frame_rate(Bytes& nal) {
    Sps sps;
    if (!parse_sps(nal, sps) || sps.fixed_frame_rate_bit < 0) return false;
    Bytes data = rbsp(nal, nal.size());
    size_t bit = size_t(sps.fixed_frame_rate_bit);
    data[bit >> 3] |= uint8_t(0x80 >> (bit & 7));
    nal = escape(nal[0], data);
    return true;
}

bool add_vui(Bytes& nal, int fps) {
    Sps sps;
    if (!parse_sps(nal, sps) || sps.vui_flag_bit < 0) return false;
    Bytes data = rbsp(nal, nal.size());
    size_t at = size_t(sps.vui_flag_bit);
    if ((data[at >> 3] >> (7 - (at & 7))) & 1) return false;
    BitWriter w;
    for (size_t i = 0; i < at; i++) w.u((data[i >> 3] >> (7 - (i & 7))) & 1, 1);
    w.u(1, 1);              // vui_parameters_present_flag
    w.u(0, 4);              // aspect_ratio, overscan, video_signal_type, chroma_loc: absent
    w.u(1, 1);              // timing_info_present_flag
    w.u(1000, 32);          // num_units_in_tick
    w.u(uint32_t(2000 * fps), 32);  // time_scale
    w.u(1, 1);              // fixed_frame_rate_flag
    w.u(0, 3);              // nal_hrd, vcl_hrd, pic_struct_present
    w.u(1, 1);              // bitstream_restriction_flag
    w.u(1, 1);              // motion_vectors_over_pic_boundaries_flag
    w.ue(0);                // max_bytes_per_pic_denom
    w.ue(0);                // max_bits_per_mb_denom
    w.ue(15);               // log2_max_mv_length_horizontal
    w.ue(15);               // log2_max_mv_length_vertical
    w.ue(0);                // max_num_reorder_frames
    w.ue(uint32_t(std::max(sps.max_num_ref_frames, 1)));  // max_dec_frame_buffering
    w.u(1, 1);              // rbsp_stop_one_bit
    nal = escape(nal[0], w.bytes());
    return true;
}

bool parse_slice_header(std::span<const uint8_t> nal, SliceHeader& out) {
    Bytes data = rbsp(nal, 16);
    Bits b(data);
    out.first_mb = int(b.ue());
    out.slice_type = int(b.ue() % 5);
    return b.ok();
}

void append_annexb(Bytes& out, std::span<const uint8_t> nal) {
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), nal.begin(), nal.end());
}

}  // namespace ql::enc::h264
