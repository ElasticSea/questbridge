// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// HELLO device description: every tests/fixtures/devices/*.json (written by
// tools/device_info.py, also checked by tests/test_device_info.py) and what the host derives
// from it (CONFIG refresh and codec, strip geometry).
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

#include "frame.hpp"
#include "harness.hpp"
#include "session.hpp"

using namespace qb;
namespace es = qb::encoded_slice;

namespace {

std::vector<std::filesystem::path> device_files() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(qbtest::repo("tests/fixtures/devices")))
        if (e.path().extension() == ".json") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

DeviceInfo parse_body(const Bytes& body) {
    auto info = parse_device_info(capnp::decode(std::span<const uint8_t>(body).subspan(8)));
    if (!info) throw std::runtime_error("no device description");
    return *info;
}

template <class T>
std::vector<T> numbers(const json::Value& v) {
    std::vector<T> out;
    for (const auto& x : v.items()) out.push_back(T(x.as_u64()));
    return out;
}

// The Quest's HELLO v1 / v3 from the recorded handshake, raw
Bytes quest_hello(uint16_t version) {
    for (const auto& e : qbtest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == "quest_to_pc" && e.has("capnp") && e["inner"]["type"].as_u64() == control::kHello &&
            (e["capnp"]["tree"]["s"][0].as_u64() & 0xFFFF) == version)
            return e["raw"].as_hex();
    return {};
}

// The recorded HELLO v3 with its codec list (device description p6.p0) replaced
Bytes hello_v3_with_codecs(const Bytes& raw, const std::vector<uint16_t>& codecs) {
    Message m;
    if (!decode_message(raw, m)) throw std::runtime_error("bad HELLO");
    auto body = unpad_body(m.flags, m.body);
    capnp::Ptr tree = capnp::decode(body.subspan(8));
    capnp::Node* list = tree->ptrs.at(3)->ptrs.at(0)->ptrs.at(6)->ptrs.at(0).get();
    list->esz = 3;  // u16 elements
    list->count = uint32_t(codecs.size());
    list->bytes.assign(codecs.size() * 2, 0);
    for (size_t i = 0; i < codecs.size(); ++i) wr16(&list->bytes[2 * i], codecs[i]);
    Bytes out(body.begin(), body.begin() + 8);
    Bytes seg = capnp::encode(capnp::strip_offsets(tree));
    out.insert(out.end(), seg.begin(), seg.end());
    return encode_message(pad_body(out), m.topic, m.seq, out);
}

// Drive a session through both handshake phases with the recorded Quest messages and keep
// the data words 0 (codec) and 2 (refresh rate) of the CONFIG v3 it sent
struct Run {
    int64_t now = 0;
    std::unique_ptr<Session> s;
    uint64_t config_word0 = 0, config_word2 = 0;
    Bytes config_v3;

    explicit Run(SessionConfig cfg, std::function<Bytes(const Bytes&)> patch_hello_v3 = {})
        : s(std::make_unique<Session>(cfg, [this] { return now += 1'000'000; })) {
        s->start();
        Bytes v3 = quest_hello(3);
        for (const auto& e : qbtest::fixture("handshake", "control").items()) {
            if (e["dir"].as_string() != "quest_to_pc") continue;
            Bytes raw = e["raw"].as_hex();
            if (patch_hello_v3 && raw == v3) raw = patch_hello_v3(raw);
            s->on_message(raw);
            for (auto& xfer : s->take_outgoing())
                for (auto& m : xfer) {
                    if (m[1] != topic::kControl || m[8] != control::kConfig) continue;
                    auto tree = capnp::decode(std::span<const uint8_t>(m).subspan(16));
                    if ((tree->word(0) & 0xFFFF) != 3) continue;
                    config_word0 = tree->word(0);
                    config_word2 = tree->word(2);
                    config_v3 = m;
                }
        }
    }
};

// Meta's recorded CONFIG v3 as sent (PC->Quest raw)
Bytes meta_config_v3() {
    for (const auto& e : qbtest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == "pc_to_quest" && e.has("capnp") && e["inner"]["type"].as_u64() == control::kConfig &&
            (e["capnp"]["tree"]["s"][0].as_u64() & 0xFFFF) == 3)
            return e["raw"].as_hex();
    return {};
}

}  // namespace

// Quest 3 keeps its verified rate; the Pro gets 70% of what it advertises at 72 Hz
TEST(default_bitrate_per_headset) {
    for (const auto& path : device_files()) {
        auto fx = json::parse_file(path.string());
        DeviceInfo d = parse_body(fx["hello"].as_hex());
        if (d.product == "Meta Quest 3") CHECK_EQ(default_bitrate(d, 72), uint32_t(100'000'000));
        if (d.product == "Meta Quest Pro") {
            uint64_t advertised = 0;
            for (const auto& m : d.modes)
                if (m.hz == 72) advertised = m.bitrate;
            CHECK(advertised > 0);
            CHECK_EQ(default_bitrate(d, 72), uint32_t(advertised * 7 / 10));
            CHECK(default_bitrate(d, 72) < 90'000'000);
        }
    }
    DeviceInfo unknown;
    CHECK_EQ(default_bitrate(unknown, 72), uint32_t(100'000'000));
}

TEST(device_fixtures_match_expected) {
    auto files = device_files();
    CHECK(files.size() >= 2);  // quest3, quest_pro
    for (const auto& path : files) {
        auto fx = json::parse_file(path.string());
        const auto& x = fx["expected"];
        DeviceInfo d = parse_body(fx["hello"].as_hex());
        std::string name = path.stem().string();
        CHECK_EQ(name + " " + d.product, name + " " + x["product"].as_string());
        CHECK_EQ(d.version, uint16_t(x["version"].as_u64()));
        CHECK_EQ(d.manufacturer, x["manufacturer"].as_string());
        CHECK_EQ(d.firmware, x["firmware"].as_string());
        CHECK_EQ(d.panel_width, uint32_t(x["panel"][0].as_u64()));
        CHECK_EQ(d.panel_height, uint32_t(x["panel"][1].as_u64()));
        CHECK_EQ(d.encode_width, uint32_t(x["encode"][0].as_u64()));
        CHECK_EQ(d.encode_height, uint32_t(x["encode"][1].as_u64()));
        CHECK_EQ(d.refresh_hz, float(x["default_refresh_hz"].as_double()));
        CHECK_EQ(d.ipd_m, float(x["ipd_m"].as_double()));
        for (int eye = 0; eye < 2; ++eye)
            for (int i = 0; i < 4; ++i)
                CHECK_EQ(d.fov_deg[eye][i], float(x["fov_deg"][eye ? "right" : "left"][i].as_double()));
        CHECK_EQ(d.slices, uint32_t(x["slices"].as_u64()));
        CHECK(d.refresh_rates == numbers<uint32_t>(x["refresh_rates"]));
        CHECK(d.codecs == numbers<uint16_t>(x["codecs"]));
        CHECK(d.features == numbers<uint16_t>(x["features"]));
        REQUIRE(d.modes.size() == x["modes"].size());
        for (size_t i = 0; i < d.modes.size(); ++i) {
            const auto& m = x["modes"][i];
            CHECK_EQ(d.modes[i].hz, uint32_t(m["hz"].as_u64()));
            CHECK_EQ(d.modes[i].a, m["a"].as_double());
            CHECK_EQ(d.modes[i].b, m["b"].as_double());
            CHECK_EQ(d.modes[i].bitrate, m["bitrate"].as_u64());
        }
    }
}

TEST(device_hello_v1_has_slices_and_same_display) {
    Bytes v1 = quest_hello(1), v3 = quest_hello(3);
    REQUIRE(!v1.empty() && !v3.empty());
    DeviceInfo a = parse_body(Bytes(v1.begin() + kHeaderSize, v1.end()));
    DeviceInfo b = parse_body(Bytes(v3.begin() + kHeaderSize, v3.end()));
    CHECK_EQ(a.version, uint16_t(1));
    CHECK_EQ(a.slices, uint32_t(5));
    CHECK_EQ(b.slices, uint32_t(0));
    CHECK_EQ(a.encode_width, b.encode_width);
    CHECK_EQ(a.encode_height, b.encode_height);
    CHECK_EQ(a.fov_deg[1][2], b.fov_deg[1][2]);
    CHECK(a.refresh_rates == b.refresh_rates);  // same rates; v1 lists them unsorted
}

TEST(config_refresh_from_device) {
    // Default: the device's default rate (72 on Quest 3) = Meta's recorded CONFIG
    Run def({});
    CHECK_EQ(uint32_t(def.config_word2), uint32_t(72));
    CHECK_EQ(def.s->refresh_hz(), uint32_t(72));
    // A requested rate the HELLO offers
    SessionConfig c;
    c.refresh_hz = 90;
    Run r90(c);
    CHECK_EQ(uint32_t(r90.config_word2), uint32_t(90));
    CHECK_EQ(r90.config_word2 >> 32, def.config_word2 >> 32);  // rest of the word untouched
    CHECK_EQ(r90.s->refresh_hz(), uint32_t(90));
    // One it doesn't offer falls back to the default
    c.refresh_hz = 60;
    CHECK_EQ(uint32_t(Run(c).config_word2), uint32_t(72));
}

TEST(config_codec_auto_and_override) {
    // Default (Windows) auto with Quest 3's empty list: H.264, Meta's recorded CONFIG byte for byte
    Bytes meta = meta_config_v3();
    REQUIRE(!meta.empty());
    Run def({});
    CHECK_EQ(def.s->codec(), codec::kH264);
    CHECK_EQ(def.config_word0 >> 48, uint64_t(0));
    CHECK(Bytes(def.config_v3.begin() + 8, def.config_v3.end()) == Bytes(meta.begin() + 8, meta.end()));  // seq aside
    // macOS auto with an empty list: HEVC (the Quest Pro lists nothing and decodes it)
    SessionConfig mac;
    mac.prefer_hevc = true;
    Run mac_empty(mac);
    CHECK_EQ(mac_empty.s->codec(), codec::kHevc);
    CHECK_EQ(mac_empty.config_word0, def.config_word0 | 1ull << 48);
    // A headset listing HEVC: auto picks it only where HEVC is preferred
    auto lists_hevc = [](const Bytes& raw) { return hello_v3_with_codecs(raw, {0, 1}); };
    Run mac_hevc(mac, lists_hevc);
    REQUIRE(mac_hevc.s->device_info());
    CHECK(mac_hevc.s->device_info()->codecs == (std::vector<uint16_t>{0, 1}));
    CHECK_EQ(mac_hevc.s->codec(), codec::kHevc);
    CHECK_EQ(mac_hevc.config_word0 >> 48, uint64_t(1));
    CHECK_EQ(Run({}, lists_hevc).s->codec(), codec::kH264);
    // H.264 only: auto stays on it
    CHECK_EQ(Run(mac, [](const Bytes& raw) { return hello_v3_with_codecs(raw, {0}); }).s->codec(), codec::kH264);
    // Override: sent as is; only u16 @ byte 6 differs from Meta's CONFIG
    SessionConfig forced;
    forced.codec = codec::kHevc;
    Run f(forced);
    CHECK_EQ(f.s->codec(), codec::kHevc);
    CHECK_EQ(f.config_word0, def.config_word0 | 1ull << 48);
    REQUIRE(f.config_v3.size() == def.config_v3.size());
    for (size_t i = 8; i < f.config_v3.size(); ++i)
        if (f.config_v3[i] != def.config_v3[i]) CHECK_EQ(i, size_t(16 + 8 + 8 + 6));  // headers, segment table, root ptr, byte 6
}

TEST(strips_follow_encode_height) {
    Bytes v(16, 0);
    std::vector<SliceData> slices(5, SliceData{{}, v});
    auto strips = [&](uint32_t height, size_t count) {
        FrameParams p;
        p.encode_height = height;
        std::vector<std::pair<uint32_t, uint32_t>> out;  // (yOffset px, height MB)
        auto msgs = build_frame(std::span(slices).first(count), p);
        for (size_t i = 1; i < msgs.size(); i += 3) {  // table, header, video per slice
            const Bytes& h = msgs[i].body;
            size_t d = 8 + 8 * (rd32(h.data()) >> 2);
            out.push_back({rd32(&h[d + es::kYOffset]), rd32(&h[d + es::kHeightMb])});
        }
        return out;
    };
    using S = std::vector<std::pair<uint32_t, uint32_t>>;
    CHECK(strips(0, 5) == (S{{0, 28}, {448, 28}, {896, 28}, {1344, 28}, {1792, 28}}));  // template
    CHECK(strips(2240, 5) == strips(0, 5));
    CHECK(strips(1600, 5) == (S{{0, 20}, {320, 20}, {640, 20}, {960, 20}, {1280, 20}}));
    CHECK(strips(2240, 1) == (S{{0, 140}}));
    CHECK(strips(2160, 5) == (S{{0, 27}, {432, 27}, {864, 27}, {1296, 27}, {1728, 27}}));  // Quest Pro
    CHECK(strips(1080, 5) == (S{{0, 14}, {224, 14}, {448, 14}, {672, 14}, {896, 12}}));  // 68 rows
}
