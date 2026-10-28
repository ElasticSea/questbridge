// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Byte-exact XRSP framing against Meta's traffic (mirrors tests/test_roundtrip.py XrspFraming).
#include "harness.hpp"

using namespace qb;

namespace {

std::vector<Bytes> split_messages(const Bytes& data) {
    std::vector<Bytes> out;
    size_t i = 0;
    while (i + 8 <= data.size()) {
        size_t size = 4 + size_t(rd16(&data[i + 2])) * 4;
        out.emplace_back(data.begin() + i, data.begin() + i + size);
        i += size;
    }
    return out;
}

}  // namespace

TEST(control_messages_reencode) {
    size_t n = 0;
    for (const auto& sc : qbtest::scenarios()) {
        for (const auto& e : qbtest::fixture(sc, "control").items()) {
            Bytes raw = e["raw"].as_hex();
            Message m;
            REQUIRE(decode_message(raw, m));
            CHECK_EQ(encode_message(m), raw);
            ++n;
        }
    }
    CHECK(n > 0);
}

TEST(stream_samples_reencode) {
    size_t n = 0;
    for (const auto& sc : qbtest::scenarios()) {
        for (const auto& group : qbtest::fixture(sc, "streams").items()) {
            for (const auto& s : group["samples"].items()) {
                Bytes raw = s["raw"].as_hex();
                Message m;
                REQUIRE(decode_message(raw, m));
                CHECK_EQ(m.topic, uint8_t(group["topic"].as_u64()));
                CHECK_EQ(encode_message(m.flags, m.topic, m.seq, m.body, m.reserved), raw);
                ++n;
            }
        }
    }
    CHECK(n > 0);
}

TEST(transfers_repack) {
    size_t n = 0;
    for (const auto& sc : qbtest::scenarios()) {
        for (const auto& x : qbtest::fixture(sc, "transfers").items()) {
            Bytes raw = x["raw"].as_hex();
            std::vector<Bytes> msgs;
            for (auto& m : split_messages(raw))
                if (m[1] != 0) msgs.push_back(std::move(m));
            CHECK_EQ(pack_transfer(msgs), raw);
            ++n;
        }
    }
    CHECK(n > 0);
}

TEST(padding_rule) {
    // Meta pads bodies to 4 bytes with 0xDE filler; last byte = pad count, flag 0x08
    for (size_t len = 0; len < 9; ++len) {
        Bytes body(len, 0x11);
        Bytes padded = body;
        uint8_t flags = pad_body(padded);
        CHECK_EQ(padded.size() % 4, size_t(0));
        CHECK_EQ(flags, uint8_t(len % 4 ? 0x18 : 0x10));
        if (len % 4) {
            CHECK_EQ(size_t(padded.back()), padded.size() - len);
            for (size_t i = len; i + 1 < padded.size(); ++i) CHECK_EQ(padded[i], uint8_t(0xDE));
        }
        auto back = unpad_body(flags, padded);
        CHECK_EQ(Bytes(back.begin(), back.end()), body);
    }
}

TEST(padded_stream_bodies_unpad) {
    // Every 0x18 message Meta sent ends with a pad count byte preceded by 0xDE filler. Video and
    // topic 31 pad to 4 bytes; audio (6) and telemetry (33) pad the message to 1 KiB.
    size_t n = 0;
    for (const auto& sc : qbtest::scenarios()) {
        for (const auto& group : qbtest::fixture(sc, "streams").items()) {
            for (const auto& s : group["samples"].items()) {
                Message m;
                REQUIRE(decode_message(s["raw"].as_hex(), m));
                if (!(m.flags & kFlagPadded)) continue;
                uint8_t pad = m.body.back();
                size_t align = pad < 4 ? 4 : 1024;
                CHECK(pad >= 1);
                CHECK_EQ((m.body.size() + 8) % align, size_t(0));
                for (size_t i = m.body.size() - pad; i + 1 < m.body.size(); ++i) CHECK_EQ(m.body[i], uint8_t(0xDE));
                Bytes core(m.body.begin(), m.body.end() - pad);
                Bytes again = core;
                CHECK_EQ(pad_body(again, align), m.flags);
                CHECK_EQ(again, m.body);
                ++n;
            }
        }
    }
    CHECK(n > 0);
}

TEST(reassembly_across_transfers) {
    // Feed every recorded transfer byte-split at awkward points; messages must come out whole
    for (const auto& sc : qbtest::scenarios()) {
        for (const char* dir : {"pc_to_quest", "quest_to_pc"}) {
            std::vector<Bytes> expected;
            Bytes stream;
            for (const auto& x : qbtest::fixture(sc, "transfers").items()) {
                if (x["dir"].as_string() != dir) continue;
                Bytes raw = x["raw"].as_hex();
                for (auto& m : split_messages(raw))
                    if (m[1] != 0) expected.push_back(m);
                stream.insert(stream.end(), raw.begin(), raw.end());
            }
            for (size_t chunk : {size_t(1), size_t(7), size_t(1024), size_t(5000)}) {
                Reassembler r;
                std::vector<Bytes> got;
                for (size_t i = 0; i < stream.size(); i += chunk) {
                    size_t end = std::min(stream.size(), i + chunk);
                    r.feed(std::span(stream).subspan(i, end - i));
                    Bytes m;
                    while (r.next(m)) got.push_back(m);
                }
                CHECK_EQ(got.size(), expected.size());
                CHECK(got == expected);
            }
        }
    }
}

TEST(reassembly_resyncs_on_garbage) {
    Reassembler r;
    Bytes bad = {0x10, 0x01, 0x00, 0x00, 0, 0, 0, 0};  // len 0 -> size 4 < header
    r.feed(bad);
    Bytes m;
    CHECK(!r.next(m));
    CHECK_EQ(r.resyncs(), size_t(1));
    Bytes body = {1, 2, 3, 4};
    Bytes good = encode_message(0x10, 5, 9, body);
    r.feed(good);
    REQUIRE(r.next(m));
    CHECK_EQ(m, good);
}
