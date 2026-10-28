// Encoder selection (encoders/select.hpp) and the strip layout wrapper (encoders/strips.hpp)
// with fake encoders and synthetic H.264 headers: no GPU or codec needed.
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"
#include "h264.hpp"
#include "select.hpp"
#include "strips.hpp"

using namespace qb::enc;

namespace {

class Writer {
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
    void se(int v) { ue(v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * v)); }
    // rbsp_trailing_bits, emulation prevention, Annex-B start code
    Bytes nal(uint8_t header) {
        u(1, 1);
        while (bits_ % 8) u(0, 1);
        Bytes out{0, 0, 0, 1, header};
        int zeros = 0;
        for (uint8_t b : d_) {
            if (zeros >= 2 && b <= 3) {
                out.push_back(3);
                zeros = 0;
            }
            out.push_back(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
        return out;
    }

private:
    Bytes d_;
    size_t bits_ = 0;
};

Bytes sps(int width_mbs, int height_mbs, int profile = 66, int constraints = 0xC0) {
    Writer w;
    w.u(uint32_t(profile), 8);
    w.u(uint32_t(constraints), 8);
    w.u(42, 8);
    w.ue(0);   // sps id
    w.ue(4);   // log2_max_frame_num - 4
    w.ue(2);   // poc type 2
    w.ue(1);   // max_num_ref_frames
    w.u(0, 1);
    w.ue(uint32_t(width_mbs - 1));
    w.ue(uint32_t(height_mbs - 1));
    w.u(1, 1);  // frame_mbs_only
    w.u(1, 1);  // direct_8x8
    w.u(0, 1);  // cropping
    w.u(0, 1);  // vui
    return w.nal(0x67);
}

Bytes pps() {
    Writer w;
    w.ue(0);
    w.ue(0);
    w.u(0, 1);  // CAVLC
    w.u(0, 1);
    w.ue(0);    // slice groups
    w.ue(0);
    w.ue(0);
    w.u(0, 1);
    w.u(0, 2);
    w.se(0);    // pic_init_qp 26
    w.se(0);
    w.se(0);
    w.u(1, 1);  // deblocking_filter_control_present
    w.u(0, 1);
    w.u(0, 1);
    return w.nal(0x68);
}

Bytes slice(int first_mb, bool idr, int deblock_idc) {
    Writer w;
    w.ue(uint32_t(first_mb));
    w.ue(idr ? 7 : 5);  // I / P, all slices of the picture alike
    w.ue(0);            // pps id
    w.u(0, 8);          // frame_num
    if (idr) w.ue(0);   // idr_pic_id
    if (!idr) {
        w.u(0, 1);      // num_ref_idx_active_override
        w.u(0, 1);      // ref_pic_list_modification_l0
    }
    if (idr) w.u(0, 2); // no_output_of_prior_pics, long_term_reference
    else w.u(0, 1);     // adaptive_ref_pic_marking
    w.se(4);            // QP 30
    w.ue(uint32_t(deblock_idc));
    for (int i = 0; i < 8; i++) w.u(0xA5, 8);  // "macroblock data"
    return w.nal(idr ? 0x65 : 0x61);
}

struct Shape {
    Layout layout = Layout::Slices;
    int slices = 5;
    int deblock_idc = 1;
    bool idr = true;
    int profile = 66, constraints = 0xC0;
    bool strip_csd = true;
    int first_mb_shift = 0;
};

// What a correct (or deliberately broken) encoder outputs for cfg
EncodedFrame frame_for(const EncoderConfig& cfg, const Shape& s) {
    EncodedFrame f;
    const bool strips = s.layout == Layout::Strips;
    const int wm = cfg.width / 16, hm = (strips ? cfg.height / std::max(cfg.slices, 1) : cfg.height) / 16;
    Bytes csd = sps(wm, hm, s.profile, s.constraints);
    Bytes p = pps();
    csd.insert(csd.end(), p.begin(), p.end());
    f.idr = s.idr;
    if (!strips) f.csd = csd;
    for (int i = 0; i < s.slices; i++) {
        Slice sl;
        sl.index = i;
        sl.first_mb = strips ? 0 : i * (cfg.height / 16 / std::max(cfg.slices, 1)) * wm + s.first_mb_shift;
        sl.nal = slice(sl.first_mb, s.idr, s.deblock_idc);
        if (strips && s.strip_csd) sl.csd = csd;
        f.slices.push_back(sl);
    }
    if (strips) f.csd = csd;
    return f;
}

class FakeEncoder final : public Encoder {
public:
    FakeEncoder(const EncoderConfig& c, Shape s, std::string name) : cfg_(c), shape_(s), name_(std::move(name)) {}
    bool encode(const CpuFrame& f, int64_t ts, bool idr, EncodedFrame& out) override {
        planes.push_back(f.plane[0]);
        idrs.push_back(idr);
        if (fail_encode) {
            error_ = "fake failure";
            return false;
        }
        Shape s = shape_;
        s.idr = idr || shape_.idr && idrs.size() == 1;
        out = frame_for(cfg_, s);
        out.timestamp_ns = ts;
        return true;
    }
    bool encode(const GpuFrame& g, int64_t ts, bool idr, EncodedFrame& out) override {
        gpu.push_back(g.texture);
        CpuFrame c;
        return encode(c, ts, idr, out);
    }
    std::string name() const override { return name_; }
    const std::string& error() const override { return error_; }

    EncoderConfig cfg_;
    Shape shape_;
    std::string name_, error_;
    bool fail_encode = false;
    std::vector<const uint8_t*> planes;
    std::vector<const void*> gpu;
    std::vector<bool> idrs;
};

Candidate cand(Backend b, Vendor v, bool hw, std::string name, bool on_gpu = true) {
    Candidate c;
    c.backend = b;
    c.vendor = v;
    c.hardware = hw;
    c.name = std::move(name);
    c.on_gpu_adapter = on_gpu;
    return c;
}

std::string summary(const std::vector<Attempt>& a) {
    std::string s;
    for (const auto& x : a) s += (s.empty() ? "" : ",") + x.tag + (x.layout == Layout::Strips ? "/strips" : "/slices");
    return s;
}

std::vector<Candidate> laptop() {
    // Intel iGPU listed first by MF, NVIDIA dGPU holds the game's device, plus an AMD card
    return {cand(Backend::MediaFoundation, Vendor::Intel, true, "Intel Quick Sync Video H.264 Encoder MFT", false),
            cand(Backend::MediaFoundation, Vendor::Amd, true, "AMDh264Encoder", false),
            cand(Backend::MediaFoundation, Vendor::Nvidia, true, "NVIDIA H.264 Encoder MFT", true),
            cand(Backend::MediaFoundation, Vendor::Microsoft, false, "H264 Encoder MFT"),
            cand(Backend::OpenH264, Vendor::Unknown, false, "OpenH264")};
}

}  // namespace

TEST(encoder_vendor_detection) {
    CHECK(vendor_from_pci(0x10DE) == Vendor::Nvidia);
    CHECK(vendor_from_pci(0x1002) == Vendor::Amd);
    CHECK(vendor_from_pci(0x8086) == Vendor::Intel);
    CHECK(vendor_from_pci(0x1234) == Vendor::Unknown);
    CHECK(vendor_from_text("VEN_10DE") == Vendor::Nvidia);
    CHECK(vendor_from_text("ven_1002") == Vendor::Amd);
    CHECK(vendor_from_text("VEN_8086") == Vendor::Intel);
    CHECK(vendor_from_text("NVIDIA H.264 Encoder MFT") == Vendor::Nvidia);
    CHECK(vendor_from_text("AMDh264Encoder") == Vendor::Amd);
    CHECK(vendor_from_text("Intel\xC2\xAE Quick Sync Video H.264 Encoder MFT") == Vendor::Intel);
    CHECK(vendor_from_text("H264 Encoder MFT") == Vendor::Unknown);
    CHECK_EQ(std::string(vendor_name(Vendor::Amd)), std::string("amd"));
}

TEST(encoder_plan_order_and_vendor_layouts) {
    EncoderConfig cfg;
    cfg.layout = Layout::Auto;
    auto a = plan(laptop(), cfg);
    // NVIDIA on the device's adapter first (slices, its verified layout), then the other
    // adapters by vendor (AMD strips first), then OpenH264 before the Microsoft MFT (strips only)
    CHECK_EQ(summary(a), std::string("mf-nvidia/slices,mf-nvidia/strips,mf-amd/strips,mf-amd/slices,"
                                     "mf-intel/slices,mf-intel/strips,openh264/strips,mf-software/strips"));
    for (const auto& x : a) CHECK(x.config.layout == x.layout);
    CHECK(a[0].settings.quality_vs_speed < 0);   // NVIDIA: proven defaults untouched
    CHECK(a[2].settings.quality_vs_speed >= 0);  // AMD: speed preset
    CHECK(a[0].settings.deblock_off);
}

TEST(encoder_plan_filters) {
    EncoderConfig cfg;
    cfg.layout = Layout::Auto;
    cfg.backend = "openh264";
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("openh264/strips"));
    cfg.backend = "amd";
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("mf-amd/strips,mf-amd/slices"));
    cfg.backend = "mf-intel";
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("mf-intel/slices,mf-intel/strips"));
    cfg.backend = "software";
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("openh264/strips,mf-software/strips"));
    cfg.backend = "mf";
    CHECK_EQ(plan(laptop(), cfg).size(), size_t(6));
    cfg.backend = "auto";
    cfg.layout = Layout::Slices;  // the Microsoft MFT and OpenH264 have no Slices
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("mf-nvidia/slices,mf-amd/slices,mf-intel/slices"));
    cfg.layout = Layout::Auto;
    cfg.hardware = false;
    CHECK_EQ(summary(plan(laptop(), cfg)), std::string("openh264/strips,mf-software/strips"));
    cfg.hardware = true;
    cfg.allow_software = false;
    CHECK_EQ(plan(laptop(), cfg).size(), size_t(6));
    CHECK(plan({}, cfg).empty());
}

TEST(encoder_level_for) {
    CHECK_EQ(level_for(4096, 2240, 72), 60);  // Quest 3 picture
    CHECK_EQ(level_for(4096, 448, 72), 42);   // one Quest 3 strip
    CHECK_EQ(level_for(4096, 432, 72), 42);   // one Quest Pro strip (4096x2160 / 5)
    CHECK_EQ(level_for(4096, 2240, 90), 60);
    CHECK_EQ(level_for(1920, 1088, 30), 40);
    CHECK_EQ(level_for(16384, 16384, 120), 0);
}

TEST(encoder_check_frame_slices) {
    EncoderConfig cfg;
    std::string why;
    CHECK(check_frame(frame_for(cfg, {}), cfg, Layout::Slices, &why));
    CHECK_EQ(why, std::string());

    Shape s;
    s.deblock_idc = 0;  // NVIDIA's default before the fix: wedges the headset's decoders
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));
    CHECK(why.find("deblocks across slice edges") != std::string::npos);

    s = {};
    s.slices = 16;  // the Microsoft MFT's own slicing
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));
    CHECK(why.find("16 slices") != std::string::npos);

    s = {};
    s.first_mb_shift = 256;
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));
    CHECK(why.find("first_mb") != std::string::npos);

    s = {};
    s.constraints = 0;  // plain Baseline
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));
    s.profile = 77;
    s.constraints = 0xC0;
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));
    CHECK(why.find("Constrained Baseline") != std::string::npos);

    s = {};
    s.idr = false;
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));

    EncoderConfig pro = cfg;  // the device description's size, not a fixed 4096x2240
    pro.height = 2160;
    CHECK(check_frame(frame_for(pro, {}), pro, Layout::Slices, &why));
    CHECK(!check_frame(frame_for(pro, {}), cfg, Layout::Slices, &why));
}

TEST(encoder_check_frame_strips) {
    EncoderConfig cfg;
    Shape s;
    s.layout = Layout::Strips;
    s.deblock_idc = 0;  // fine inside a strip picture
    std::string why;
    CHECK(check_frame(frame_for(cfg, s), cfg, Layout::Strips, &why));
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Slices, &why));  // first_mb 0 everywhere
    s.strip_csd = false;
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Strips, &why));
    CHECK(why.find("own") != std::string::npos);
    s.strip_csd = true;
    s.slices = 4;
    CHECK(!check_frame(frame_for(cfg, s), cfg, Layout::Strips, &why));
}

TEST(encoder_select_skips_and_probes) {
    EncoderConfig cfg;
    cfg.layout = Layout::Auto;
    auto attempts = plan(laptop(), cfg);
    std::vector<std::string> created;
    FakeEncoder* chosen = nullptr;
    // nvidia slices: creation fails; nvidia strips: probe has 4 slices; amd strips: works
    auto factory = [&](const Attempt& a, std::string* e) -> std::unique_ptr<Encoder> {
        created.push_back(a.tag);
        if (a.tag == "mf-nvidia" && a.layout == Layout::Slices) {
            *e = "SetOutputType failed";
            return nullptr;
        }
        Shape s;
        s.layout = a.layout;
        s.idr = false;
        if (a.tag == "mf-nvidia") s.slices = 4;
        auto f = std::make_unique<FakeEncoder>(a.config, s, a.candidate.name);
        chosen = f.get();
        return f;
    };
    std::string err;
    auto enc = select(attempts, factory, &err);
    REQUIRE(enc);
    CHECK_EQ(created.size(), size_t(3));
    CHECK(enc->layout() == Layout::Strips);
    CHECK_EQ(enc->name(), std::string("mf-amd: AMDh264Encoder"));
    CHECK(enc->selection().find("mf-nvidia slices (NVIDIA H.264 Encoder MFT): SetOutputType failed") != std::string::npos);
    CHECK(enc->selection().find("mf-nvidia strips (NVIDIA H.264 Encoder MFT): wrong format: 4 slices") != std::string::npos);
    // Probe was forced IDR; the first real frame is IDR too, later ones only when asked
    CpuFrame f;
    EncodedFrame out;
    CHECK(enc->encode(f, 1, false, out));
    CHECK(enc->encode(f, 2, false, out));
    CHECK(enc->encode(f, 3, true, out));
    CHECK(chosen->idrs == (std::vector<bool>{true, true, false, true}));
}

TEST(encoder_select_reports_every_failure) {
    EncoderConfig cfg;
    cfg.layout = Layout::Auto;
    auto attempts = plan({cand(Backend::MediaFoundation, Vendor::Nvidia, true, "NVIDIA H.264 Encoder MFT"),
                          cand(Backend::OpenH264, Vendor::Unknown, false, "OpenH264")},
                         cfg);
    auto factory = [&](const Attempt& a, std::string*) -> std::unique_ptr<Encoder> {
        auto f = std::make_unique<FakeEncoder>(a.config, Shape{}, a.candidate.name);
        f->fail_encode = true;
        return f;
    };
    std::string err;
    CHECK(!select(attempts, factory, &err));
    CHECK(err.find("mf-nvidia slices") != std::string::npos);
    CHECK(err.find("mf-nvidia strips") != std::string::npos);
    CHECK(err.find("openh264 strips (OpenH264): probe encode failed: fake failure") != std::string::npos);
    CHECK(!select({}, factory, &err));
    CHECK_EQ(err, std::string("no H.264 encoder found"));
}

TEST(encoder_strips_wrapper) {
    EncoderConfig cfg;  // 4096x2240, 5 strips
    std::vector<FakeEncoder*> subs;
    std::vector<EncoderConfig> configs;
    auto factory = [&](const EncoderConfig& sc, int i, std::string*) -> std::unique_ptr<Encoder> {
        configs.push_back(sc);
        Shape s;
        s.slices = 1;
        s.idr = false;
        auto f = std::make_unique<FakeEncoder>(sc, s, "fake " + std::to_string(i));
        subs.push_back(f.get());
        return f;
    };
    std::string err;
    auto enc = make_strip_encoder(cfg, factory, {}, &err);
    REQUIRE(enc);
    REQUIRE(subs.size() == 5);
    CHECK_EQ(configs[0].height, 448);
    CHECK_EQ(configs[0].width, 4096);
    CHECK_EQ(configs[0].slices, 1);
    CHECK_EQ(configs[0].bitrate, uint32_t(20'000'000));
    CHECK(enc->layout() == Layout::Strips);
    CHECK_EQ(enc->name(), std::string("fake 0, 5 strips of 4096x448"));

    std::vector<uint8_t> nv12(size_t(4096) * 2240 * 3 / 2);
    CpuFrame f;
    f.plane[0] = nv12.data();
    f.plane[1] = nv12.data() + size_t(4096) * 2240;
    f.stride[0] = f.stride[1] = 4096;
    EncodedFrame out;
    REQUIRE(enc->encode(f, 7, true, out));
    REQUIRE(out.slices.size() == 5);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(out.slices[size_t(i)].index, i);
        CHECK_EQ(out.slices[size_t(i)].first_mb, 0);
        CHECK(!out.slices[size_t(i)].csd.empty());
        CHECK(subs[size_t(i)]->planes[0] == nv12.data() + size_t(i) * 448 * 4096);  // the strip's rows, no copy
        CHECK(subs[size_t(i)]->idrs[0]);
    }
    CHECK(out.idr);
    std::string why;
    CHECK(check_frame(out, cfg, Layout::Strips, &why));

    // P frame: intra flag only when every strip is an IDR
    REQUIRE(enc->encode(f, 8, false, out));
    CHECK(!out.idr);

    subs[3]->fail_encode = true;
    CHECK(!enc->encode(f, 9, false, out));
    CHECK_EQ(enc->error(), std::string("strip 3: fake failure"));
    subs[3]->fail_encode = false;
    subs[2]->shape_.slices = 2;
    CHECK(!enc->encode(f, 10, false, out));
    CHECK_EQ(enc->error(), std::string("strip 2: 2 slices, want 1"));

    GpuFrame g;
    CHECK(!enc->encode(g, 11, false, out));  // no GPU input hook
}

TEST(encoder_strips_gpu_input) {
    EncoderConfig cfg;
    std::vector<FakeEncoder*> subs;
    auto factory = [&](const EncoderConfig& sc, int, std::string*) -> std::unique_ptr<Encoder> {
        Shape s;
        s.slices = 1;
        auto f = std::make_unique<FakeEncoder>(sc, s, "fake");
        subs.push_back(f.get());
        return f;
    };
    int textures[5] = {};
    GpuInput split;
    split.split = [&](const GpuFrame&, std::vector<GpuFrame>& out, std::string*) {
        out.assign(5, GpuFrame{});
        for (int i = 0; i < 5; i++) out[size_t(i)].texture = &textures[i];
        return true;
    };
    std::string err;
    auto enc = make_strip_encoder(cfg, factory, split, &err);
    REQUIRE(enc);
    EncodedFrame out;
    REQUIRE(enc->encode(GpuFrame{}, 0, true, out));
    for (int i = 0; i < 5; i++) CHECK(subs[size_t(i)]->gpu.at(0) == &textures[i]);

    subs.clear();
    std::vector<uint8_t> bgra(size_t(4096) * 2240 * 4);
    GpuInput rb;
    rb.read_back = [&](const GpuFrame&, CpuFrame& c, std::string*) {
        c.format = PixelFormat::BGRA;
        c.plane[0] = bgra.data();
        c.stride[0] = 4096 * 4;
        return true;
    };
    enc = make_strip_encoder(cfg, factory, rb, &err);
    REQUIRE(enc);
    REQUIRE(enc->encode(GpuFrame{}, 0, true, out));
    CHECK(subs[4]->planes.at(0) == bgra.data() + size_t(4) * 448 * 4096 * 4);
    CHECK(subs[4]->gpu.empty());
}

TEST(encoder_constrained_baseline_patch) {
    Bytes s = sps(256, 28, 66, 0);
    s.erase(s.begin(), s.begin() + 4);  // NAL without start code
    CHECK(h264::set_constrained_baseline(s));
    h264::Sps parsed;
    CHECK(h264::parse_sps(s, parsed));
    CHECK_EQ(parsed.constraints, 0xC0);
    CHECK_EQ(parsed.width_mbs, 256);
    Bytes main = sps(256, 28, 77, 0);
    main.erase(main.begin(), main.begin() + 4);
    CHECK(!h264::set_constrained_baseline(main));
}
