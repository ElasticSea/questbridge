// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
#include "select.hpp"

#include <algorithm>
#include <cctype>

#include "h264.hpp"

namespace qb::enc {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int vendor_rank(Vendor v) {
    switch (v) {
    case Vendor::Nvidia: return 0;  // the only one bench-verified on the headset
    case Vendor::Amd: return 1;
    case Vendor::Intel: return 2;
    default: return 3;
    }
}

std::string tag_for(const Candidate& c) {
    if (c.backend == Backend::OpenH264) return "openh264";
    if (!c.hardware) return "mf-software";
    return std::string("mf-") + vendor_name(c.vendor);
}

// Does config.backend allow this candidate?
bool wanted(const Candidate& c, const std::string& backend) {
    if (backend.empty() || backend == "auto") return true;
    if (backend == "software") return !c.hardware;
    if (backend == "mf") return c.backend == Backend::MediaFoundation && c.hardware;
    return tag_for(c) == backend || (c.hardware && backend == vendor_name(c.vendor));
}

// Hands the first encode() an IDR (the probe was the encoder's first frame) and names the backend.
class Selected final : public Encoder {
public:
    Selected(std::unique_ptr<Encoder> inner, const Attempt& a, std::string log)
        : inner_(std::move(inner)), layout_(a.layout), tag_(a.tag), log_(std::move(log)) {}

    bool encode(const CpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        bool idr = force_idr || first_;
        first_ = false;
        return inner_->encode(f, ts, idr, out);
    }
    bool encode(const GpuFrame& f, int64_t ts, bool force_idr, EncodedFrame& out) override {
        bool idr = force_idr || first_;
        first_ = false;
        return inner_->encode(f, ts, idr, out);
    }
    std::string name() const override { return tag_ + ": " + inner_->name(); }
    const std::string& error() const override { return inner_->error(); }
    Layout layout() const override { return layout_; }
    std::string selection() const override { return log_; }

private:
    std::unique_ptr<Encoder> inner_;
    Layout layout_;
    std::string tag_, log_;
    bool first_ = true;
};

}  // namespace

Vendor vendor_from_pci(uint32_t id) {
    switch (id) {
    case 0x10DE: return Vendor::Nvidia;
    case 0x1002:
    case 0x1022: return Vendor::Amd;
    case 0x8086:
    case 0x8087: return Vendor::Intel;
    case 0x1414: return Vendor::Microsoft;  // WARP / Basic Render Driver
    case 0x106B: return Vendor::Apple;
    default: return Vendor::Unknown;
    }
}

Vendor vendor_from_text(std::string_view text) {
    std::string t = lower(text);
    if (auto at = t.find("ven_"); at != std::string::npos && at + 8 <= t.size()) {
        uint32_t id = 0;
        for (size_t i = at + 4; i < at + 8; i++) {
            char c = t[i];
            if (!std::isxdigit(static_cast<unsigned char>(c))) return Vendor::Unknown;
            id = id * 16 + uint32_t(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        return vendor_from_pci(id);
    }
    if (t.find("nvidia") != std::string::npos || t.find("nvenc") != std::string::npos) return Vendor::Nvidia;
    if (t.find("amd") != std::string::npos || t.find("radeon") != std::string::npos) return Vendor::Amd;
    if (t.find("intel") != std::string::npos || t.find("quick sync") != std::string::npos) return Vendor::Intel;
    if (t.find("microsoft") != std::string::npos) return Vendor::Microsoft;
    return Vendor::Unknown;
}

const char* vendor_name(Vendor v) {
    switch (v) {
    case Vendor::Nvidia: return "nvidia";
    case Vendor::Amd: return "amd";
    case Vendor::Intel: return "intel";
    case Vendor::Microsoft: return "microsoft";
    case Vendor::Apple: return "apple";
    default: return "unknown";
    }
}

VendorSettings settings_for(const Candidate& c) {
    VendorSettings s;
    if (c.backend == Backend::OpenH264 || !c.hardware) {
        // Software: one picture per strip, no slice control needed. The Microsoft MFT ignores
        // slice control (16 uneven slices at 4096x2240), so never Slices.
        s.layouts = {Layout::Strips};
        s.quality_vs_speed = 0;
        return s;
    }
    switch (c.vendor) {
    case Vendor::Nvidia:
        // Bench-verified: SliceControlMode 2 gives exact 28-row slices, deblocking off is honoured
        s.layouts = {Layout::Slices, Layout::Strips};
        break;
    case Vendor::Intel:
        // Quick Sync MFT documents MB-row slice control; strips if the probe says otherwise
        s.layouts = {Layout::Slices, Layout::Strips};
        s.quality_vs_speed = 33;
        break;
    default:
        // AMD (and unknown): slice control and deblocking-off support vary by driver, while one
        // slice per picture is every encoder's default, so independent strips first
        s.layouts = {Layout::Strips, Layout::Slices};
        s.quality_vs_speed = 33;
        break;
    }
    return s;
}

std::vector<Attempt> plan(const std::vector<Candidate>& found, const EncoderConfig& config) {
    std::vector<Candidate> c;
    for (const auto& x : found) {
        if (x.hardware ? !config.hardware : !config.allow_software) continue;
        if (!wanted(x, config.backend)) continue;
        c.push_back(x);
    }
    // Hardware on the device's adapter (texture input without a copy), then by vendor; then
    // other hardware; then OpenH264 before the Microsoft software MFT. Stable: keeps MFT order.
    auto group = [](const Candidate& x) {
        if (x.hardware) return x.on_gpu_adapter ? 0 : 1;
        return x.backend == Backend::OpenH264 ? 2 : 3;
    };
    std::stable_sort(c.begin(), c.end(), [&](const Candidate& a, const Candidate& b) {
        if (group(a) != group(b)) return group(a) < group(b);
        return a.hardware && vendor_rank(a.vendor) < vendor_rank(b.vendor);
    });
    std::vector<Attempt> out;
    for (const auto& x : c) {
        VendorSettings s = settings_for(x);
        for (Layout l : s.layouts) {
            if (config.layout != Layout::Auto && l != config.layout) continue;
            Attempt a;
            a.candidate = x;
            a.layout = l;
            a.settings = s;
            a.config = config;
            a.config.layout = l;
            a.tag = tag_for(x);
            out.push_back(std::move(a));
        }
    }
    return out;
}

int level_for(int width, int height, int fps) {
    struct L {
        int idc;
        long long mbps, fs;
    };
    static const L table[] = {{10, 1485, 99},       {11, 3000, 396},       {12, 6000, 396},
                              {13, 11880, 396},     {20, 11880, 396},      {21, 19800, 792},
                              {22, 20250, 1620},    {30, 40500, 1620},     {31, 108000, 3600},
                              {32, 216000, 5120},   {40, 245760, 8192},    {42, 522240, 8704},
                              {50, 589824, 22080},  {51, 983040, 36864},   {52, 2073600, 36864},
                              {60, 4177920, 139264}, {61, 8355840, 139264}, {62, 16711680, 139264}};
    const long long w = (width + 15) / 16, h = (height + 15) / 16, fs = w * h;
    for (const auto& l : table)
        if (fs <= l.fs && fs * fps <= l.mbps && w * w <= 8 * l.fs && h * h <= 8 * l.fs) return l.idc;
    return 0;
}

bool check_frame(const EncodedFrame& f, const EncoderConfig& cfg, Layout layout, std::string* why) {
    auto bad = [&](std::string m) {
        if (why) *why = std::move(m);
        return false;
    };
    const int n = std::max(cfg.slices, 1);
    const bool strips = layout == Layout::Strips;
    const int pic_h = strips ? cfg.height / n : cfg.height;
    const int width_mbs = (cfg.width + 15) / 16, height_mbs = (pic_h + 15) / 16;
    if (int(f.slices.size()) != n)
        return bad(std::to_string(f.slices.size()) + " slices, want " + std::to_string(n));
    if (!f.idr) return bad("probe frame is not an IDR");
    for (int i = 0; i < n; i++) {
        const Slice& s = f.slices[size_t(i)];
        const std::string at = "slice " + std::to_string(i) + ": ";
        if (s.index != i) return bad(at + "index " + std::to_string(s.index));
        auto nals = h264::split_annexb(s.nal);
        if (nals.size() != 1) return bad(at + std::to_string(nals.size()) + " NALs, want 1");
        if (h264::nal_type(nals[0]) != h264::kIdr) return bad(at + "not an IDR slice");
        const int want_mb = strips ? 0 : i * (cfg.height / 16 / n) * width_mbs;
        if (s.first_mb != want_mb)
            return bad(at + "first_mb " + std::to_string(s.first_mb) + ", want " + std::to_string(want_mb));
        const Bytes& csd = strips ? s.csd : f.csd;
        if (strips && csd.empty()) return bad(at + "no SPS/PPS of its own");
        if (!strips && i > 0) continue;  // one SPS/PPS for the picture
        h264::Sps sps;
        h264::Pps pps;
        if (!h264::parse_csd(csd, sps, pps)) return bad(at + "SPS/PPS missing or unparsed");
        if (sps.profile != 66 || !(sps.constraints & 0x40))
            return bad(at + "profile " + std::to_string(sps.profile) + " constraints " + std::to_string(sps.constraints) +
                       ", want Constrained Baseline");
        if (sps.width_mbs != width_mbs || sps.height_mbs != height_mbs)
            return bad(at + "picture " + std::to_string(sps.width_mbs) + "x" + std::to_string(sps.height_mbs) +
                       " MBs, want " + std::to_string(width_mbs) + "x" + std::to_string(height_mbs));
        if (!strips && n > 1) {
            // Each slice goes to its own decoder on the Quest: filtering across a slice edge
            // drifts (P frames wedged the headset's decoders before deblocking was turned off)
            for (const Slice& o : f.slices) {
                h264::SliceHeader sh;
                auto nal = h264::split_annexb(o.nal);
                h264::parse_slice_header(nal[0], sh, &sps, &pps);
                if (sh.deblock_idc == 0 || sh.deblock_idc < 0)
                    return bad("slice " + std::to_string(o.index) +
                               (sh.deblock_idc ? ": deblocking idc not parsed" : ": deblocks across slice edges"));
            }
        }
    }
    return true;
}

std::unique_ptr<Encoder> select(const std::vector<Attempt>& attempts, const Factory& factory, std::string* error) {
    std::string log;
    Bytes probe;
    for (const auto& a : attempts) {
        const std::string what = a.tag + " " + (a.layout == Layout::Strips ? "strips" : "slices") +
                                 (a.candidate.name.empty() ? "" : " (" + a.candidate.name + ")");
        std::string err;
        auto enc = factory(a, &err);
        if (!enc) {
            log += what + ": " + err + "; ";
            continue;
        }
        // Mid-gray NV12 probe frame, forced IDR
        const int w = a.config.width, h = a.config.height;
        probe.assign(size_t(w) * h * 3 / 2, 128);
        CpuFrame f;
        f.plane[0] = probe.data();
        f.plane[1] = probe.data() + size_t(w) * h;
        f.stride[0] = f.stride[1] = w;
        EncodedFrame out;
        if (!enc->encode(f, 0, true, out)) {
            log += what + ": probe encode failed: " + enc->error() + "; ";
            continue;
        }
        if (!check_frame(out, a.config, a.layout, &err)) {
            log += what + ": wrong format: " + err + "; ";
            continue;
        }
        return std::make_unique<Selected>(std::move(enc), a, log);
    }
    if (error) *error = attempts.empty() ? "no H.264 encoder found" : "no usable H.264 encoder: " + log;
    return nullptr;
}

}  // namespace qb::enc
