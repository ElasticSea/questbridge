#include "h264.hpp"

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
    b.ue();   // max_num_ref_frames
    b.u(1);   // gaps_in_frame_num_value_allowed_flag
    out.width_mbs = int(b.ue()) + 1;
    int map_units = int(b.ue()) + 1;
    int frame_mbs_only = int(b.u(1));
    out.height_mbs = map_units * (2 - frame_mbs_only);
    return b.ok();
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
