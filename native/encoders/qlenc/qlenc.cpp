// qlenc: encode a moving test pattern with the platform encoder, check the slice layout,
// and write an Annex-B .h264 plus a host/replay/<name>.json clip (same format as
// tools/encode_pattern.py, so `XRSP_REPLAY=... py host/handshake.py` streams it unchanged).
//
// Usage: qlenc [--frames N] [--name NAME] [--mbps 100] [--vbr] [--gop 72]
//              [--input nv12|bgra|d3d11|cvpixelbuffer] [--layout slices|strips] [--software] [--no-low-latency]
//              [--slices 5] [--max-slice-bytes 120000] [--csd every|idr] [--root REPO]
// Strips (VideoToolbox): one .h264 per strip, capture/runs/<name>-<i>.h264.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif
#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif

#include "../encoder.hpp"
#include "../h264.hpp"

namespace fs = std::filesystem;
using ql::enc::Bytes;

namespace {

// ---- Minimal JSON (enough for the replay template; numbers keep their source text) ----

struct Json {
    char kind = 'n';  // n null, b bool, d number, s string, a array, o object
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* get(const std::string& k) const {
        for (auto& [key, v] : members)
            if (key == k) return &v;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    Json value() {
        ws();
        Json v;
        char c = s_[p_];
        if (c == '{') {
            v.kind = 'o';
            p_++;
            for (ws(); s_[p_] != '}'; ws()) {
                std::string k = str();
                ws();
                p_++;  // ':'
                v.members.emplace_back(k, value());
                ws();
                if (s_[p_] == ',') p_++;
            }
            p_++;
        } else if (c == '[') {
            v.kind = 'a';
            p_++;
            for (ws(); s_[p_] != ']'; ws()) {
                v.items.push_back(value());
                ws();
                if (s_[p_] == ',') p_++;
            }
            p_++;
        } else if (c == '"') {
            v.kind = 's';
            v.text = str();
        } else {
            size_t b = p_;
            while (p_ < s_.size() && !strchr(",]} \t\r\n", s_[p_])) p_++;
            v.text = s_.substr(b, p_ - b);
            v.kind = v.text == "null" ? 'n' : (v.text == "true" || v.text == "false") ? 'b' : 'd';
        }
        return v;
    }

private:
    void ws() {
        while (p_ < s_.size() && strchr(" \t\r\n", s_[p_])) p_++;
    }
    std::string str() {
        std::string out;
        p_++;  // opening quote; the template only holds plain ASCII strings
        while (s_[p_] != '"') {
            if (s_[p_] == '\\') p_++;
            out += s_[p_++];
        }
        p_++;
        return out;
    }

    const std::string& s_;
    size_t p_ = 0;
};

void write_json(std::ostream& o, const Json& v) {
    switch (v.kind) {
    case 'o': {
        o << '{';
        bool first = true;
        for (auto& [k, m] : v.members) {
            o << (first ? "" : ", ") << '"' << k << "\": ";
            write_json(o, m);
            first = false;
        }
        o << '}';
        break;
    }
    case 'a':
        o << '[';
        for (size_t i = 0; i < v.items.size(); i++) {
            if (i) o << ", ";
            write_json(o, v.items[i]);
        }
        o << ']';
        break;
    case 's': o << '"' << v.text << '"'; break;
    default: o << v.text; break;
    }
}

Bytes from_hex(const std::string& h) {
    Bytes out(h.size() / 2);
    for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    return out;
}

std::string to_hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s(b.size() * 2, '0');
    for (size_t i = 0; i < b.size(); i++) {
        s[2 * i] = d[b[i] >> 4];
        s[2 * i + 1] = d[b[i] & 15];
    }
    return s;
}

// ---- Replay clip (mirrors tools/encode_pattern.py) ----

struct SliceTemplate {
    std::string seg_table_json;  // the recorded 8-byte capnp segment table message, re-emitted as is
    Bytes header;                // recorded EncodedSlice header segment
};

struct Message {
    int topic;
    int flags;
    Bytes body;
};

// Meta's padding: bodies 4-byte aligned; flag 0x08 + last byte = pad count, filler 0xDE.
Message padded(int topic, Bytes body) {
    size_t n = (4 - body.size() % 4) % 4;
    if (!n) return {topic, 0x10, std::move(body)};
    body.insert(body.end(), n - 1, 0xDE);
    body.push_back(uint8_t(n));
    return {topic, 0x18, std::move(body)};
}

void put_u32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i));
}

std::string message_json(const Message& m) {
    return "{\"topic\": " + std::to_string(m.topic) + ", \"flags\": " + std::to_string(m.flags) +
           ", \"body\": \"" + to_hex(m.body) + "\"}";
}

// ---- Test pattern (BGRA): tiles, slice boundary lines, a moving bar and a frame counter ----

void draw_pattern(std::vector<uint8_t>& bgra, int w, int h, int frame, int slices) {
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &bgra[(size_t(y) * w + x) * 4];
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool tile = ((x >> 8) + (y >> 8)) & 1;
            uint8_t v = uint8_t(tile ? 96 : 48);
            px(x, y, uint8_t(v + (x * 64 / w)), v, uint8_t(v + (y * 64 / h)));
        }
    }
    int band = slices > 0 ? h / slices : h;
    for (int s = 1; s < std::max(slices, 1); s++)
        for (int y = s * band - 2; y < s * band + 2; y++)
            for (int x = 0; x < w; x++) px(x, y, 255, 255, 0);
    int bar = (frame * 32) % w;
    for (int y = 0; y < h; y++)
        for (int x = bar; x < std::min(bar + 64, w); x++) px(x, y, 255, 255, 255);
    // frame counter: 12 bits as blocks top-left of every slice band
    for (int s = 0; s < std::max(slices, 1); s++)
        for (int bit = 0; bit < 12; bit++) {
            bool on = (frame >> (11 - bit)) & 1;
            for (int y = s * band + 64; y < s * band + 192; y++)
                for (int x = 128 + bit * 160; x < 128 + bit * 160 + 128; x++)
                    px(x, y, on ? 0 : 255, on ? 255 : 0, 0);
        }
}

void bgra_to_nv12(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& nv12) {
    nv12.resize(size_t(w) * h * 3 / 2);
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            int sr = 0, sg = 0, sb = 0;
            for (int k = 0; k < 4; k++) {
                const uint8_t* p = &src[(size_t(y + k / 2) * w + x + k % 2) * 4];
                nv12[size_t(y + k / 2) * w + x + k % 2] = uint8_t((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) / 256 + 16);
                sb += p[0];
                sg += p[1];
                sr += p[2];
            }
            sr /= 4, sg /= 4, sb /= 4;
            uint8_t* c = &nv12[size_t(w) * h + size_t(y / 2) * w + x];
            c[0] = uint8_t(std::clamp((-26 * sr - 87 * sg + 112 * sb + 128) / 256 + 128, 0, 255));
            c[1] = uint8_t(std::clamp((112 * sr - 102 * sg - 10 * sb + 128) / 256 + 128, 0, 255));
        }
    }
}

fs::path find_root(fs::path p) {
    for (; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "host" / "replay" / "meta-idr.json")) return p;
        if (p == p.parent_path()) break;
    }
    return {};
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 144, gop = 72, slices = 5;
    double mbps = 100;
    bool vbr = false, software = false, csd_every = true, low_latency = true;
    std::string name = "qlenc", input = "nv12", root_arg;
#ifdef __APPLE__
    std::string layout_arg = "strips";  // VideoToolbox has no slice-count control
#else
    std::string layout_arg = "slices";
#endif
    size_t max_slice_bytes = 120'000;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--frames") frames = std::stoi(next());
        else if (a == "--name") name = next();
        else if (a == "--mbps") mbps = std::stod(next());
        else if (a == "--vbr") vbr = true;
        else if (a == "--gop") gop = std::stoi(next());
        else if (a == "--input") input = next();
        else if (a == "--software") software = true;
        else if (a == "--no-low-latency") low_latency = false;
        else if (a == "--slices") slices = std::stoi(next());
        else if (a == "--layout") layout_arg = next();
        else if (a == "--max-slice-bytes") max_slice_bytes = std::stoul(next());
        else if (a == "--csd") csd_every = next() != "idr";
        else if (a == "--root") root_arg = next();
        else {
            fprintf(stderr, "usage: qlenc [--frames N] [--name NAME] [--mbps 100] [--vbr] [--gop 72] "
                            "[--input nv12|bgra|d3d11|cvpixelbuffer] [--layout slices|strips] [--software] [--no-low-latency] [--slices 5] "
                            "[--max-slice-bytes 120000] [--csd every|idr] [--root REPO]\n");
            return 2;
        }
    }
    fs::path root = root_arg.empty() ? find_root(fs::current_path()) : fs::path(root_arg);
    if (root.empty()) {
        fprintf(stderr, "repo root not found (run inside the repo or pass --root)\n");
        return 2;
    }

    ql::enc::EncoderConfig cfg;
    cfg.slices = slices;
    cfg.bitrate = uint32_t(mbps * 1e6);
    cfg.cbr = !vbr;
    cfg.gop = gop;
    cfg.hardware = !software;
    cfg.layout = layout_arg == "strips" ? ql::enc::Layout::Strips : ql::enc::Layout::Slices;
    cfg.max_slice_bytes = max_slice_bytes;
    cfg.low_latency_rc = low_latency;
    const bool strips = cfg.layout == ql::enc::Layout::Strips;
    const int W = cfg.width, H = cfg.height;

#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (input == "d3d11") {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D11CreateDevice failed 0x%08lx\n", unsigned long(hr));
            return 1;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&td, nullptr, &tex);
        cfg.gpu_device = device.Get();
    }
#endif
#ifdef __APPLE__
    CVPixelBufferRef pixbuf = nullptr;  // IOSurface-backed BGRA, like a Metal render target
    if (input == "cvpixelbuffer") {
        CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        const void* k[1] = {kCVPixelBufferIOSurfacePropertiesKey};
        const void* v[1] = {empty};
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferCreate(nullptr, W, H, kCVPixelFormatType_32BGRA, attrs, &pixbuf);
        CFRelease(attrs);
        CFRelease(empty);
    }
#endif

    std::string err;
    auto enc = ql::enc::create_encoder(cfg, &err);
    if (!enc) {
        fprintf(stderr, "encoder: %s\n", err.c_str());
        return 1;
    }
    printf("encoder: %s\ninput: %s, %dx%d @ %d fps, %.0f Mbit/s %s, gop %d, %s\n", enc->name().c_str(),
           input.c_str(), W, H, cfg.fps, mbps, vbr ? "VBR" : "CBR", gop, strips ? "strips" : "slices");

    const int height_mbs = H / 16, width_mbs = W / 16;
    std::vector<int> expected;
    for (int s = 0; s < slices; s++) expected.push_back(strips ? 0 : s * (height_mbs / slices) * width_mbs);

    // Slices: one stream. Strips: one stream per strip (each has its own SPS/PPS).
    fs::path es_path = root / "capture" / "runs" / (name + ".h264");
    fs::create_directories(es_path.parent_path());
    std::vector<std::ofstream> es;
    for (int i = 0; i < (strips ? slices : 1); i++)
        es.emplace_back(strips ? es_path.parent_path() / (name + "-" + std::to_string(i) + ".h264") : es_path,
                        std::ios::binary);

    std::vector<ql::enc::EncodedFrame> encoded;
    std::vector<double> ms, call_ms;  // encoder submit->output; whole encode() call
    std::map<std::string, int> layouts;
    std::vector<uint8_t> bgra(size_t(W) * H * 4), nv12;
    for (int f = 0; f < frames; f++) {
        draw_pattern(bgra, W, H, f, slices);
        int64_t ts = int64_t(f) * 1'000'000'000 / cfg.fps;
        ql::enc::EncodedFrame out;
        bool ok;
        if (input == "nv12") bgra_to_nv12(bgra, W, H, nv12);  // test input, not timed
        auto t0 = std::chrono::steady_clock::now();
        if (input == "bgra") {
            ql::enc::CpuFrame cf;
            cf.format = ql::enc::PixelFormat::BGRA;
            cf.plane[0] = bgra.data();
            cf.stride[0] = W * 4;
            ok = enc->encode(cf, ts, false, out);
#ifdef _WIN32
        } else if (input == "d3d11") {
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, bgra.data(), W * 4, 0);
            ql::enc::GpuFrame gf;
            gf.texture = tex.Get();
            ok = enc->encode(gf, ts, false, out);
#endif
#ifdef __APPLE__
        } else if (input == "cvpixelbuffer") {
            CVPixelBufferLockBaseAddress(pixbuf, 0);
            auto* d = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixbuf));
            size_t stride = CVPixelBufferGetBytesPerRow(pixbuf);
            for (int y = 0; y < H; y++) memcpy(d + stride * size_t(y), &bgra[size_t(y) * W * 4], size_t(W) * 4);
            CVPixelBufferUnlockBaseAddress(pixbuf, 0);
            t0 = std::chrono::steady_clock::now();  // the fill stands in for rendering, not timed
            ql::enc::GpuFrame gf;
            gf.texture = pixbuf;
            ok = enc->encode(gf, ts, false, out);
#endif
        } else {
            ql::enc::CpuFrame cf;
            cf.plane[0] = nv12.data();
            cf.plane[1] = nv12.data() + size_t(W) * H;
            cf.stride[0] = cf.stride[1] = W;
            ok = enc->encode(cf, ts, false, out);
        }
        if (!ok) {
            fprintf(stderr, "frame %d: %s\n", f, enc->error().c_str());
            return 1;
        }
        double call = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (f > 0) {  // first frame includes encoder warm-up
            ms.push_back(out.encode_ms);
            call_ms.push_back(call);
        }
        std::string layout;
        for (auto& s : out.slices) layout += (layout.empty() ? "" : "/") + std::to_string(s.first_mb);
        layouts[layout]++;
        if (strips) {
            for (auto& s : out.slices) {
                auto& o = es[std::min(size_t(s.index), es.size() - 1)];
                if (out.idr || csd_every) o.write(reinterpret_cast<const char*>(s.csd.data()), s.csd.size());
                o.write(reinterpret_cast<const char*>(s.nal.data()), s.nal.size());
            }
        } else {
            if (out.idr || csd_every) es[0].write(reinterpret_cast<const char*>(out.csd.data()), out.csd.size());
            for (auto& s : out.slices) es[0].write(reinterpret_cast<const char*>(s.nal.data()), s.nal.size());
        }
        encoded.push_back(std::move(out));
    }
    es.clear();

    // SPS summary
    ql::enc::h264::Sps sps;
    for (auto nal : ql::enc::h264::split_annexb(encoded[0].csd))
        if (ql::enc::h264::nal_type(nal) == ql::enc::h264::kSps) ql::enc::h264::parse_sps(nal, sps);
    printf("SPS: profile %d constraints 0x%02x level %d.%d, %dx%d MBs\n", sps.profile, sps.constraints,
           sps.level / 10, sps.level % 10, sps.width_mbs, sps.height_mbs);

    std::string want;
    for (int e : expected) want += (want.empty() ? "" : "/") + std::to_string(e);
    bool layout_ok = layouts.size() == 1 && layouts.begin()->first == want;
    printf("slice layouts (first_mb_in_slice): want %s\n", want.c_str());
    for (auto& [l, n] : layouts) printf("  %4d frames: %s%s\n", n, l.c_str(), l == want ? "  OK" : "");

    size_t total = 0, largest = 0, largest_idr = 0;
    int idrs = 0, over = 0;
    for (auto& e : encoded) {
        for (auto& s : e.slices) {
            total += s.nal.size();
            size_t& big = e.idr ? largest_idr : largest;
            big = std::max(big, s.nal.size());
            over += s.nal.size() > max_slice_bytes;
        }
        idrs += e.idr;
    }
    printf("frames %zu (IDR %d), avg %zu B/frame = %.1f Mbit/s at %d Hz\n", encoded.size(), idrs,
           total / encoded.size(), total * 8.0 * cfg.fps / encoded.size() / 1e6, cfg.fps);
    auto stats = [&](const char* label, const std::vector<double>& v) {
        double sum = 0;
        for (double x : v) sum += x;
        printf("%s ms/frame: mean %.2f, p50 %.2f, p95 %.2f, max %.2f\n", label, v.empty() ? 0 : sum / v.size(),
               percentile(v, 0.5), percentile(v, 0.95), v.empty() ? 0 : *std::max_element(v.begin(), v.end()));
    };
    printf("largest slice NAL: P %zu B, IDR %zu B; over %zu B: %d\n", largest, largest_idr, max_slice_bytes, over);
    printf("first frame (warm-up): %.2f ms\n", encoded[0].encode_ms);
    stats("encoder submit->output", ms);
    stats("encode() call incl. copy/convert", call_ms);
    printf("wrote %s%s\n", es_path.string().c_str(), strips ? " (one file per strip: -0..-N)" : "");

    if (!layout_ok || slices != 5) {
        printf("replay clip skipped: needs exactly the 5-slice layout\n");
        return layout_ok ? 0 : 3;
    }

    // Replay clip from the Meta template
    std::ifstream tf(root / "host" / "replay" / "meta-idr.json", std::ios::binary);
    std::string ttext((std::istreambuf_iterator<char>(tf)), {});
    Json meta = JsonParser(ttext).value();
    const Json& msgs = *meta.get("frame");
    std::map<int, SliceTemplate> tmpl;
    for (size_t i = 1; i < msgs.items.size(); i++) {
        const Json &prev = msgs.items[i - 1], &cur = msgs.items[i];
        if (prev.get("body")->text.size() == 16 && prev.get("topic")->text == cur.get("topic")->text) {
            std::ostringstream o;
            write_json(o, prev);
            tmpl[std::stoi(cur.get("topic")->text)] = {o.str(), from_hex(cur.get("body")->text)};
        }
    }

    fs::path clip_path = root / "host" / "replay" / (name + ".json");
    std::ofstream clip(clip_path, std::ios::binary);
    std::ostringstream setup;
    write_json(setup, *meta.get("setup"));
    clip << "{\"source\": \"qlenc " << enc->name() << " " << mbps << " Mbit/s gop " << gop
         << (strips ? " strips" : "")
         << "\", \"frame_index\": 1, \"setup\": " << setup.str() << ", \"frames\": [";
    for (size_t f = 0; f < encoded.size(); f++) {
        const auto& e = encoded[f];
        bool with_csd = csd_every || e.idr;
        clip << (f ? ", " : "") << '[';
        for (int i = 0; i < 5; i++) {
            int topic = 10 + i;
            const auto& st = tmpl.at(topic);
            Bytes hdr = st.header;
            uint32_t ptr = uint32_t(hdr[0]) | uint32_t(hdr[1]) << 8 | uint32_t(hdr[2]) << 16 | uint32_t(hdr[3]) << 24;
            size_t d = 8 + 8 * (ptr >> 2);
            hdr[d + 48] = uint8_t(i);                                         // slice.index
            hdr[d + 49] = uint8_t((e.idr ? 1 : 0) | (i == 4 ? 2 : 0));        // isIntraFrame, isLastSliceOfAFrame
            const Bytes& csd = e.slices[i].csd.empty() ? e.csd : e.slices[i].csd;
            put_u32(hdr, d + 60, with_csd ? uint32_t(csd.size()) : 0);        // csdLength
            put_u32(hdr, d + 64, uint32_t(e.slices[i].nal.size()));           // videoLength
            clip << (i ? ", " : "") << st.seg_table_json << ", " << message_json({topic, 0x10, hdr});
            if (with_csd) clip << ", " << message_json(padded(topic, csd));
            clip << ", " << message_json(padded(topic, e.slices[i].nal));
        }
        clip << ']';
    }
    clip << "]}";
    printf("wrote %s\n", clip_path.string().c_str());
    return 0;
}
