// The shared library through its C API, over a fake transport that plays the Quest's side
// of Meta's recorded session. Our USB transfers must equal Meta's.
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

    void queue_message(const Bytes& raw) {
        Bytes x = pack_transfer(std::vector<Bytes>{raw});
        rx.insert(rx.end(), x.begin(), x.end());
    }

    ql_transport transport() {
        ql_transport t{};
        t.user = this;
        t.write = [](void* u, const uint8_t* d, size_t n, int) {
            static_cast<FakeUsb*>(u)->written.emplace_back(d, d + n);
            return 0;
        };
        t.read = [](void* u, uint8_t* buf, size_t cap, int) {
            auto* f = static_cast<FakeUsb*>(u);
            size_t n = std::min(cap, f->rx.size() - f->rx_pos);
            std::copy(f->rx.begin() + f->rx_pos, f->rx.begin() + f->rx_pos + n, buf);
            f->rx_pos += n;
            return int(n);
        };
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
    ql_get_head_pose(h, &p);
    return p;
}

}  // namespace

TEST(capi_handshake_transfers_match_meta) {
    FakeUsb usb;
    for (const auto& e : qltest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == "quest_to_pc") usb.queue_message(e["raw"].as_hex());
    ql_config cfg;
    ql_config_init(&cfg);
    ql_transport t = usb.transport();
    ql_host* h = nullptr;
    REQUIRE(ql_open_transport(&cfg, &t, &h) == QL_OK);
    CHECK_EQ(ql_get_state(h), QL_STATE_WAIT_HELLO);
    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_poll(h, 0), QL_OK);
    CHECK_EQ(ql_get_state(h), QL_STATE_SETUP);

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

    ql_info info;
    REQUIRE(ql_get_info(h, &info) == QL_OK);
    CHECK_EQ(info.session_id, uint64_t(6936518381556450372ull));
    CHECK(info.has_clock_offset);
    CHECK(info.rx_counts[1] > 15);
    ql_close(h);
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
    REQUIRE(ql_open_transport(&cfg, &t, &h) == QL_OK);
    CHECK_EQ(ql_get_state(h), QL_STATE_STREAMING);

    uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68};
    uint8_t nal[] = {0, 0, 0, 1, 0x65, 1, 2, 3, 4, 5};
    ql_slice slices[5];
    for (auto& s : slices) s = {csd, sizeof csd, nal, sizeof nal};
    CHECK_EQ(ql_submit_frame_slices(h, slices, 5, nullptr), QL_ERR_NO_CLOCK);
    CHECK_EQ(ql_get_head_pose(h, nullptr), 0);

    while (usb.rx_pos < usb.rx.size()) CHECK_EQ(ql_poll(h, 0), QL_OK);
    ql_head_pose p = pose_after(h);
    CHECK_EQ(p.count, uint64_t(1));
    CHECK_EQ(p.sample_time_ns, uint64_t(344814707924000));

    usb.written.clear();
    ql_frame_info fi{};
    fi.is_intra = 1;
    fi.has_render_pose = 1;
    fi.render_pose = p.pose;
    REQUIRE(ql_submit_frame_slices(h, slices, 5, &fi) == QL_OK);
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
    CHECK_EQ(q3, p.pose.orientation[3]);

    ql_info info;
    ql_get_info(h, &info);
    CHECK_EQ(info.next_frame_index, uint64_t(1001));
    CHECK_EQ(info.video_seq, uint16_t(7 + 4));
    CHECK(info.quest_clock_offset_ns > 71'000'000'000);
    ql_close(h);
}

TEST(clip_splits_back_into_slices) {
    auto clip = qlcli::load_clip(qltest::repo("host/replay/meta-idr.json"));
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
    REQUIRE(ql_open_transport(nullptr, &t, &h) == QL_OK);
    CHECK_EQ(ql_poll(h, 0), QL_ERR_TRANSPORT);
    CHECK_EQ(std::string(ql_get_error(h)), std::string("USB read failed"));
    CHECK_EQ(std::string(ql_state_name(QL_STATE_WAIT_LINK_REQUEST)), std::string("wait_link_request"));
    ql_close(h);
    CHECK_EQ(ql_open_transport(nullptr, nullptr, &h), QL_ERR_ARGUMENT);
}
