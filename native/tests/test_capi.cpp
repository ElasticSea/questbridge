// The shared library through its C API, over a fake transport that plays the Quest's side
// of Meta's recorded session. Our USB transfers must equal Meta's.
#include <cmath>
#include <deque>

#include "clip.hpp"
#include "harness.hpp"
#include "questlink.h"

using namespace ql;

namespace {

struct FakeUsb {
    Bytes rx;  // bytes the "Quest" will send
    size_t rx_pos = 0;
    std::vector<Bytes> written;
    bool fail = false;  // reads and writes fail (unplugged)
    int closed = 0;

    void queue_message(const Bytes& raw) {
        Bytes x = pack_transfer(std::vector<Bytes>{raw});
        rx.insert(rx.end(), x.begin(), x.end());
    }

    ql_transport transport() {
        ql_transport t{};
        t.user = this;
        t.write = [](void* u, const uint8_t* d, size_t n, int) {
            auto* f = static_cast<FakeUsb*>(u);
            if (f->fail) return -1;
            f->written.emplace_back(d, d + n);
            return 0;
        };
        t.read = [](void* u, uint8_t* buf, size_t cap, int) {
            auto* f = static_cast<FakeUsb*>(u);
            if (f->fail) return -1;
            size_t n = std::min(cap, f->rx.size() - f->rx_pos);
            std::copy(f->rx.begin() + f->rx_pos, f->rx.begin() + f->rx_pos + n, buf);
            f->rx_pos += n;
            return int(n);
        };
        t.close = [](void* u) { ++static_cast<FakeUsb*>(u)->closed; };
        return t;
    }
};

std::vector<Bytes> meta_transfers(const char* dir) {
    std::vector<Bytes> out;
    for (const auto& x : qltest::fixture("handshake", "transfers").items())
        if (x["dir"].as_string() == dir) out.push_back(x["raw"].as_hex());
    return out;
}

ql_head_pose pose_after(ql_host* h) {
    ql_head_pose p{};
    ql_host_get_head_pose(h, &p);
    return p;
}

}  // namespace

TEST(capi_handshake_transfers_match_meta) {
    FakeUsb usb;
    for (const auto& e : qltest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == "quest_to_pc") usb.queue_message(e["raw"].as_hex());
    ql_config cfg;
    ql_config_init(&cfg);
    cfg.codec = QL_CODEC_H264;  // Meta's recording is H.264 (auto picks HEVC on macOS)
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    CHECK_EQ(ql_host_get_state(h), QL_STATE_WAIT_HELLO);
    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    CHECK_EQ(ql_host_get_state(h), QL_STATE_SETUP);

    auto meta = meta_transfers("pc_to_quest");
    REQUIRE(usb.written.size() >= 9 && meta.size() >= 8);
    for (size_t i = 0; i < 6; ++i) CHECK_EQ(usb.written[i], meta[i]);  // handshake, byte-exact
    // Clock ping: same transfer except the timestamps
    CHECK_EQ(Bytes(usb.written[6].begin(), usb.written[6].begin() + 16), Bytes(meta[6].begin(), meta[6].begin() + 16));
    CHECK_EQ(usb.written[6].size(), meta[6].size());
    // First stream setup message (topic 26 segment table) as Meta sent it
    bool setup_seen = false;
    for (const auto& w : usb.written)
        if (w[1] == 26) {
            CHECK_EQ(w, meta[7]);
            setup_seen = true;
            break;
        }
    CHECK(setup_seen);

    ql_device_info dev;
    REQUIRE(ql_host_get_device_info(h, &dev) == QL_OK);
    CHECK_EQ(dev.encode_width, uint32_t(4096));
    CHECK_EQ(dev.encode_height, uint32_t(2240));
    CHECK_EQ(dev.refresh_hz, 72.0f);
    CHECK(std::abs(dev.fov_left.up - std::tan(44.0 * 3.14159265358979 / 180)) < 1e-6);
    CHECK_EQ(dev.fov_left.left, dev.fov_right.right);  // mirrored eyes
    CHECK_EQ(dev.fov_left.right, dev.fov_right.left);
    CHECK_EQ(dev.panel_width, uint32_t(4128));
    CHECK_EQ(dev.panel_height, uint32_t(2208));
    CHECK_EQ(dev.refresh_count, uint32_t(4));
    CHECK_EQ(dev.refresh_rates[0], uint32_t(72));
    CHECK_EQ(dev.refresh_rates[3], uint32_t(120));
    CHECK_EQ(dev.stream_refresh_hz, uint32_t(72));
    CHECK_EQ(dev.codecs, uint32_t(0));
    CHECK_EQ(dev.hello_version, uint32_t(3));
    CHECK_EQ(dev.features, uint64_t((1u << 1) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 13) | (1u << 14) |
                                    (1u << 15) | (1u << 19) | (1u << 20)));
    CHECK_EQ(std::string(dev.product), std::string("Meta Quest 3"));
    CHECK_EQ(std::string(dev.manufacturer), std::string("Oculus VR"));
    CHECK_EQ(std::string(dev.firmware), std::string("releases-oculus-14.0-v207"));

    ql_info info;
    REQUIRE(ql_host_get_info(h, &info) == QL_OK);
    CHECK_EQ(info.session_id, uint64_t(6936518381556450372ull));
    CHECK(info.has_clock_offset);
    CHECK(info.rx_counts[1] > 15);
    ql_host_close(h);
}

TEST(capi_attach_pose_and_frame) {
    FakeUsb usb;
    // A Quest clock ping and the recorded head pose (segment table + segment)
    Bytes ping(8 + 32);
    ping[0] = 0x06;
    ping[1] = 0x80;
    wr16(&ping[2], 2);
    wr64(&ping[8 + 16], uint64_t(ql_now_ns() + 71'400'000'000));
    usb.queue_message(encode_message(0x10, 1, 0, ping));
    for (const auto& g : qltest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == 34)
            for (const auto& s : g["samples"].items()) usb.queue_message(s["raw"].as_hex());

    ql_config cfg;
    ql_config_init(&cfg);
    cfg.attach = 1;
    cfg.attach_control_seq = 40;
    cfg.attach_video_seq = 7;
    cfg.first_frame_index = 1000;
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    CHECK_EQ(ql_host_get_state(h), QL_STATE_STREAMING);

    uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68};
    uint8_t nal[] = {0, 0, 0, 1, 0x65, 1, 2, 3, 4, 5};
    ql_slice slices[5];
    for (auto& s : slices) s = {csd, sizeof csd, nal, sizeof nal};
    CHECK_EQ(ql_host_submit_frame_slices(h, slices, 5, nullptr), QL_ERR_NO_CLOCK);
    ql_head_pose none;
    CHECK_EQ(ql_host_get_head_pose(h, &none), QL_ERR_NO_DATA);

    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    ql_head_pose p = pose_after(h);
    CHECK_EQ(p.count, uint64_t(1));
    CHECK_EQ(p.pose.time_ns, uint64_t(344814707924000));

    usb.written.clear();
    ql_frame_info fi{};
    fi.is_intra = 1;
    fi.has_render_pose = 1;
    fi.render_pose = p.pose;
    fi.rectify_mesh_id = 1001;
    fi.has_fov = 1;
    fi.fov_left = {0.9657f, 1.4281f, 1.3764f, 0.8391f};  // up, down, left, right
    fi.fov_right = {0.9657f, 1.4281f, 0.8391f, 1.3764f};

    // A slice over the message cap: nothing sent, error set
    std::vector<uint8_t> huge(130 * 1024, 0);
    ql_slice over[5];
    for (auto& s : over) s = slices[0];
    over[3].video = huge.data();
    over[3].video_len = huge.size();
    CHECK_EQ(ql_host_submit_frame_slices(h, over, 5, &fi), QL_ERR_ARGUMENT);
    CHECK(usb.written.empty());
    CHECK(std::string(ql_host_get_error(h)).find("frame dropped") != std::string::npos);

    REQUIRE(ql_host_submit_frame_slices(h, slices, 5, &fi) == QL_OK);
    REQUIRE(usb.written.size() == 1);
    const Bytes& x = usb.written[0];
    CHECK_EQ(x.size() % 1024, size_t(0));
    // First message: topic 10 segment table, seq continues from the attach value
    CHECK_EQ(x[1], uint8_t(10));
    CHECK_EQ(rd16(&x[4]), uint16_t(7));
    // Header: frame index 1000, render pose = the head pose
    const uint8_t* hdr = &x[16 + 8];
    CHECK_EQ(rd64(hdr + 8), uint64_t(1000));
    float q3;
    std::memcpy(&q3, hdr + 8 + 12 + 12, 4);
    CHECK_EQ(q3, p.pose.qw);
    // Timewarp fields: mesh 1001; FovPorts stored as left, right, up, down
    CHECK_EQ(rd32(hdr + 8 + 8), uint32_t(1001));
    float fov[8];
    for (size_t eye = 0; eye < 2; ++eye) {
        const uint8_t* ptr = hdr + 8 + 8 * (18 + eye);
        std::memcpy(&fov[4 * eye], ptr + 8 + 8 * (int32_t(rd32(ptr)) >> 2), 16);
    }
    CHECK_EQ(fov[0], 1.3764f);
    CHECK_EQ(fov[1], 0.8391f);
    CHECK_EQ(fov[2], 0.9657f);
    CHECK_EQ(fov[3], 1.4281f);
    CHECK_EQ(fov[4], 0.8391f);
    CHECK_EQ(fov[5], 1.3764f);

    ql_info info;
    ql_host_get_info(h, &info);
    CHECK_EQ(info.next_frame_index, uint64_t(1001));
    CHECK_EQ(info.video_seq, uint16_t(7 + 4));
    CHECK(info.quest_clock_offset_ns > 71'000'000'000);
    ql_host_close(h);
}

TEST(clip_splits_back_into_slices) {
    auto clip = qlcli::load_clip(qltest::repo("tests/fixtures/replay/idr-frame.json"));
    CHECK_EQ(clip.frame_index, uint64_t(2));
    REQUIRE(clip.frames.size() == 1);
    const auto& f = clip.frames[0];
    REQUIRE(f.size() == 5);
    size_t lens[] = {7182, 7183, 7184, 7184, 7184};
    for (size_t i = 0; i < 5; ++i) {
        CHECK_EQ(f[i].first.size(), size_t(40));
        CHECK_EQ(f[i].second.size(), lens[i]);
        // An IDR slice NAL after the start code (4-byte on slice 0, 3-byte on the others)
        size_t sc = i == 0 ? 4 : 3;
        CHECK_EQ(f[i].second[sc - 1], uint8_t(1));
        CHECK_EQ(f[i].second[sc] & 0x1F, 5);
    }
}

TEST(capi_transport_errors) {
    ql_transport t{};
    t.write = [](void*, const uint8_t*, size_t, int) { return 0; };
    t.read = [](void*, uint8_t*, size_t, int) { return -1; };
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(nullptr, &t, &h) == QL_OK);
    CHECK_EQ(ql_host_poll(h, 0), QL_ERR_TRANSPORT);
    CHECK_EQ(std::string(ql_host_get_error(h)), std::string("USB read failed"));
    CHECK_EQ(std::string(ql_state_name(QL_STATE_WAIT_LINK_REQUEST)), std::string("wait_link_request"));
    ql_host_close(h);
    CHECK_EQ(ql_host_open_transport(nullptr, nullptr, &h), QL_ERR_ARGUMENT);
}

TEST(capi_hands) {
    FakeUsb usb;
    const auto& fx = qltest::fixture("hands", "messages");
    for (const auto& r : fx["topic9_first_chunks"].items()) usb.queue_message(r.as_hex());
    for (const auto& r : fx["topic8"].items()) usb.queue_message(r.as_hex());
    ql_config cfg;
    ql_config_init(&cfg);
    CHECK_EQ(cfg.hands, 1);
    cfg.attach = 1;
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    ql_hands hands;
    CHECK_EQ(ql_host_get_hands(h, &hands), QL_ERR_NO_DATA);
    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    REQUIRE(ql_host_get_hands(h, &hands) == QL_OK);
    CHECK_EQ(hands.count, uint64_t(2));
    const ql_hand& l = hands.hands[0];
    const ql_hand& r = hands.hands[1];
    CHECK_EQ(l.flags, uint32_t(QL_HAND_TRACKED | QL_HAND_AIM_VALID | QL_HAND_HEADSET_SKELETON));
    CHECK_EQ(r.flags, uint32_t(QL_HAND_TRACKED | QL_HAND_AIM_VALID | QL_HAND_DOMINANT | QL_HAND_HEADSET_SKELETON));
    CHECK_EQ(l.root.time_ns, uint64_t(351543261520000));
    CHECK(std::abs(l.pinch[0] - 0.7f) < 1e-4f);
    // Thumb tip (5) meets index tip (10) at pinch 0.7
    float d = std::hypot(l.joints[5].px - l.joints[10].px, l.joints[5].py - l.joints[10].py, l.joints[5].pz - l.joints[10].pz);
    CHECK(d < 0.02f);
    CHECK(std::abs(r.joints[25].px - 0.303384f) < 2e-5f);  // LITTLE_TIP, decode_hands.py

    usb.written.clear();
    REQUIRE(ql_host_set_hands(h, 0) == QL_OK);
    REQUIRE(usb.written.size() == 1);
    CHECK_EQ(usb.written[0][1], uint8_t(28));
    ql_host_close(h);
}

namespace {

// Queue a labelled sensors message (segment table + segment) as Meta's two XRSP messages
void queue_capnp(FakeUsb& usb, uint8_t topic, const char* label) {
    for (const auto& m : qltest::fixture("sensors", "controllers").items())
        if (m["label"].as_string() == label) {
            Bytes b = m["capnp"].as_hex();
            usb.queue_message(encode_message(0x10, topic, 0, std::span(b).first(8)));
            usb.queue_message(encode_message(0x10, topic, 1, std::span(b).subspan(8)));
        }
}

ql_controller controller_after(FakeUsb& usb, ql_host* h, uint8_t topic, const char* label, int hand) {
    queue_capnp(usb, topic, label);
    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    ql_controller c;
    CHECK_EQ(ql_host_get_controller(h, hand, &c), QL_OK);
    return c;
}

}  // namespace

TEST(capi_controllers) {
    FakeUsb usb;
    ql_config cfg;
    ql_config_init(&cfg);
    CHECK_EQ(cfg.haptics, 1);
    cfg.attach = 1;
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    ql_controller c;
    CHECK_EQ(ql_host_get_controller(h, 0, &c), QL_ERR_NO_DATA);
    CHECK_EQ(ql_host_get_controller(h, 2, &c), QL_ERR_ARGUMENT);

    c = controller_after(usb, h, 34, "pose_both_tracked", 1);
    CHECK_EQ(c.flags, uint32_t(0));  // pose but no input yet
    c = controller_after(usb, h, 35, "a", 1);
    CHECK_EQ(c.flags, uint32_t(QL_CONTROLLER_CONNECTED | QL_CONTROLLER_POSITION_TRACKED | QL_CONTROLLER_ROTATION_TRACKED));
    CHECK_EQ(c.buttons, uint32_t(QL_BUTTON_PRIMARY));
    CHECK(c.touches & QL_BUTTON_PRIMARY);
    CHECK(std::abs(c.pose.pz - 0.645326f) < 1e-5f);
    CHECK_EQ(c.pose.time_ns, uint64_t(0x0001441716a04368));
    // Left X/Y/stick sit 8 bits up in the raw word; menu is left only
    CHECK_EQ(controller_after(usb, h, 35, "x", 0).buttons, uint32_t(QL_BUTTON_PRIMARY));
    CHECK_EQ(controller_after(usb, h, 35, "y", 0).buttons, uint32_t(QL_BUTTON_SECONDARY));
    CHECK_EQ(controller_after(usb, h, 35, "l_stick_click", 0).buttons, uint32_t(QL_BUTTON_THUMBSTICK));
    CHECK_EQ(controller_after(usb, h, 35, "r_stick_click", 1).buttons, uint32_t(QL_BUTTON_THUMBSTICK));
    CHECK_EQ(controller_after(usb, h, 35, "menu", 0).buttons, uint32_t(QL_BUTTON_MENU));
    c = controller_after(usb, h, 35, "l_trigger", 0);
    CHECK(c.trigger > 0.99f);
    CHECK(controller_after(usb, h, 35, "r_stick_y_min", 1).stick_y < -0.9f);
    // Controllers put down: not connected
    controller_after(usb, h, 34, "pose_controllers_lost", 0);
    c = controller_after(usb, h, 35, "hands_mode", 0);
    CHECK_EQ(c.flags, uint32_t(0));

    usb.written.clear();
    CHECK_EQ(ql_host_haptic(h, 0, -1.0f, 0.1f), QL_ERR_ARGUMENT);
    REQUIRE(ql_host_haptic(h, 0, 1.0f, 1.0f) == QL_OK);
    REQUIRE(usb.written.size() == 2);  // attached: lookup + connect first, then the call
    CHECK_EQ(usb.written[0][1], uint8_t(31));
    CHECK_EQ(usb.written[1][1], uint8_t(31));
    ql_host_close(h);
}

TEST(capi_reopen_resumes_stream) {
    // Attached stream with a clock offset and a pose, one frame sent
    FakeUsb usb;
    Bytes ping(8 + 32);
    ping[0] = 0x06;
    ping[1] = 0x80;
    wr16(&ping[2], 2);
    wr64(&ping[8 + 16], uint64_t(ql_now_ns() + 71'400'000'000));
    usb.queue_message(encode_message(0x10, 1, 0, ping));
    std::vector<Bytes> pose;  // segment table, segment
    for (const auto& g : qltest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == 34)
            for (const auto& m : g["samples"].items()) pose.push_back(m["raw"].as_hex());
    for (const auto& m : pose) usb.queue_message(m);
    ql_config cfg;
    ql_config_init(&cfg);
    cfg.attach = 1;
    cfg.attach_video_seq = 7;
    cfg.first_frame_index = 1000;
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68};
    uint8_t nal[] = {0, 0, 0, 1, 0x65, 1, 2, 3, 4, 5};
    ql_slice slices[5];
    for (auto& s : slices) s = {csd, sizeof csd, nal, sizeof nal};
    REQUIRE(ql_host_submit_frame_slices(h, slices, 5, nullptr) == QL_OK);

    // USB error: everything fails until the host is reopened
    usb.fail = true;
    CHECK_EQ(ql_host_poll(h, 0), QL_ERR_TRANSPORT);
    CHECK_EQ(ql_host_submit_frame_slices(h, slices, 5, nullptr), QL_ERR_TRANSPORT);

    FakeUsb usb2;
    ql_transport t2 = usb2.transport();
    REQUIRE(ql_host_reopen(h, &t2) == QL_OK);
    CHECK_EQ(usb.closed, 1);
    CHECK(usb2.written.empty());  // no BYE: the headset may still be streaming
    ql_info info;
    ql_host_get_info(h, &info);
    CHECK_EQ(info.resuming, 1);
    CHECK_EQ(ql_host_get_state(h), QL_STATE_STREAMING);
    ql_head_pose none;
    CHECK_EQ(ql_host_get_head_pose(h, &none), QL_ERR_NO_DATA);  // the old pose is stale

    for (const auto& m : pose) usb2.queue_message(m);
    while (usb2.rx_pos < usb2.rx.size()) CHECK_EQ(ql_host_poll(h, 0), QL_OK);
    ql_host_get_info(h, &info);
    CHECK_EQ(info.resuming, 0);
    CHECK_EQ(info.resumes, uint32_t(1));
    REQUIRE(ql_host_submit_frame_slices(h, slices, 5, nullptr) == QL_OK);
    REQUIRE(usb2.written.size() == 1);
    const Bytes& x = usb2.written[0];
    CHECK_EQ(rd16(&x[4]), uint16_t(7 + 4));       // topic 10 seq continues
    CHECK_EQ(rd64(&x[16 + 8 + 8]), uint64_t(1001));  // and the frame index
    ql_host_close(h);
    CHECK_EQ(usb2.closed, 1);
}

TEST(capi_reopen_without_stream_answers_with_bye) {
    FakeUsb usb;
    ql_transport t = usb.transport();
    ql_config cfg;
    ql_config_init(&cfg);
    cfg.bye_retry_ms = -1;
    ql_host* h = nullptr;
    REQUIRE(ql_host_open_transport(&cfg, &t, &h) == QL_OK);
    usb.fail = true;
    CHECK_EQ(ql_host_poll(h, 0), QL_ERR_TRANSPORT);
    FakeUsb usb2;
    ql_transport t2 = usb2.transport();
    REQUIRE(ql_host_reopen(h, &t2) == QL_OK);
    ql_info info;
    ql_host_get_info(h, &info);
    CHECK_EQ(info.state, QL_STATE_WAIT_HELLO);
    CHECK_EQ(info.resuming, 0);
    CHECK_EQ(ql_host_reopen(nullptr, &t2), QL_ERR_ARGUMENT);
    ql_host_close(h);
}
