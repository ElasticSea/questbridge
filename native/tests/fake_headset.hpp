// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
// Reactive fake Quest behind open_usb() for the recovery tests: answers the host like xrspd
// with the recorded Quest messages (BYE -> HELLO v1, CONFIG/CONFIRM/START -> ACK/CONFIRM_ACK/
// READY, Launch -> BYE + HELLO v3), streams clock pings and head poses once a stream runs, and
// can be disturbed: USB glitch, USB function drop (Link quit, cable, reboot), headset BYE, new
// HELLO, silence. Defines qb::open_usb, so include it in exactly one file per test binary.
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "harness.hpp"
#include "session.hpp"
#include "transport.hpp"

namespace {

using namespace qb;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr int64_t kMs = 1'000'000;

template <class F>
bool wait_for(F cond, int ms = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

Bytes recorded_quest(uint8_t type, int version) {
    for (const auto& e : qbtest::fixture("handshake", "control").items()) {
        if (e["dir"].as_string() != "quest_to_pc") continue;
        Bytes raw = e["raw"].as_hex();
        if (raw.size() > 0x22 && raw[8] == type && raw[9] != control::kClockFlags && rd16(&raw[0x20]) == version)
            return raw;
    }
    throw std::runtime_error("no recorded Quest message");
}

// A message the host wrote, with the headset stream (HELLO v3 count) it went to
struct Written {
    Bytes raw;
    uint32_t stream;
    uint64_t conn;
};

struct FakeHeadset {
    enum class Phase { Idle, Phase1, WaitUser, Phase2, Streaming };

    std::mutex mu;
    bool present = false;      // USB function enabled
    int64_t back_at = 0;       // function comes back (after a drop)
    uint64_t conn = 0;         // transports of older connections fail
    Phase phase = Phase::Idle;
    int64_t silent_until = 0;  // asleep: nothing sent before this
    bool auto_launch = true;
    std::deque<std::pair<int64_t, Bytes>> pending;  // (due, message)
    Bytes rx;
    size_t rx_pos = 0;
    std::vector<Written> written;
    uint32_t streams = 0;  // HELLO v3s sent
    uint32_t hellos = 0;   // HELLO v1s sent
    uint32_t host_byes = 0;
    int64_t next_pose = 0, next_ping = 0;

    void send_at(int64_t due, Bytes raw) { pending.emplace_back(due, std::move(raw)); }

    // --- host -> headset ----------------------------------------------------
    void on_write(const uint8_t* d, size_t n) {
        int64_t now = now_ns();
        for (size_t i = 0; i + 8 <= n;) {
            size_t size = 4 + size_t(rd16(&d[i + 2])) * 4;
            if (rd16(&d[i + 2]) == 0 || i + size > n) break;  // transfer padding
            Bytes m(d + i, d + i + size);
            i += size;
            written.push_back({m, streams, conn});
            if (m[1] != topic::kControl || m.size() < 16 || m[9] == control::kClockFlags) continue;
            uint8_t type = m[8];
            uint16_t v = m.size() > 0x22 ? rd16(&m[0x20]) : 0;
            switch (type) {
                case control::kBye:
                    // Preluding BYE: xrspd ends whatever it has and pairs again (~1.6 s, shortened)
                    ++host_byes;
                    phase = Phase::Idle;
                    pending.clear();
                    invite(now + 30 * kMs);
                    break;
                case control::kConfig: send_at(now + kMs, recorded_quest(control::kAck, v)); break;
                case control::kConfirm: send_at(now + kMs, recorded_quest(control::kConfirmAck, v)); break;
                case control::kStart:
                    send_at(now + kMs, recorded_quest(control::kReady, v));
                    if (v == 1) {
                        phase = Phase::WaitUser;
                        if (auto_launch) launch(now + 50 * kMs);
                    } else {
                        phase = Phase::Streaming;
                        next_pose = next_ping = now + 2 * kMs;
                    }
                    break;
            }
        }
    }

    void invite(int64_t at) {
        send_at(at, recorded_quest(control::kHello, 1));
        ++hellos;
        phase = Phase::Phase1;
    }

    // The user presses Launch: the phase-1 session ends with BYE, xrstreamingclient says HELLO v3
    void launch(int64_t at) {
        send_at(at, recorded_quest(control::kBye, 1));
        send_at(at + kMs, recorded_quest(control::kHello, 3));
        ++streams;
        phase = Phase::Phase2;
    }

    // --- headset -> host ----------------------------------------------------
    void pump(int64_t now) {
        if (now < silent_until) return;
        while (!pending.empty() && pending.front().first <= now) {
            add(pending.front().second);
            pending.pop_front();
        }
        if (phase != Phase::Streaming) return;
        for (; next_pose <= now; next_pose += 10 * kMs)
            for (const auto& g : qbtest::fixture("handshake", "streams").items())
                if (g["topic"].as_u64() == topic::kHeadPose)
                    for (const auto& s : g["samples"].items()) add(s["raw"].as_hex());
        for (; next_ping <= now; next_ping += 250 * kMs) {
            Bytes ping(8 + 32);
            ping[0] = control::kClockPing;
            ping[1] = control::kClockFlags;
            wr16(&ping[2], 2);
            wr64(&ping[8 + 16], uint64_t(now + 71'400'000'000));
            add(encode_message(0x10, topic::kControl, 0, ping));
        }
    }

    void add(const Bytes& raw) {
        Bytes x = pack_transfer(std::vector<Bytes>{raw});
        rx.insert(rx.end(), x.begin(), x.end());
    }

    // --- disturbances ---------------------------------------------------------
    // The open transport fails once; the headset keeps its stream (resume path)
    void usb_glitch() { ++conn; }
    // The USB function goes away for gone_ms (Link quit, cable, reboot): the stream is gone and
    // the INVITE xrspd writes on re-enable reaches nobody, so only a BYE brings a HELLO
    void usb_drop(int gone_ms) {
        ++conn;
        present = false;
        back_at = now_ns() + gone_ms * kMs;
        phase = Phase::Idle;
        pending.clear();
    }
    // The headset ends the stream (protocol error, Link quit without a USB drop) and pairs again
    void headset_bye() {
        int64_t now = now_ns();
        pending.clear();
        send_at(now, recorded_quest(control::kBye, 1));
        invite(now + 30 * kMs);
    }
    void new_hello() {
        pending.clear();
        invite(now_ns());
    }
    // Asleep: sends nothing for ms; the stream is gone when it wakes
    void sleep_for(int ms) {
        silent_until = now_ns() + ms * kMs;
        phase = Phase::Idle;
        pending.clear();
    }
};
FakeHeadset g_hs;

class HeadsetTransport final : public Transport {
public:
    explicit HeadsetTransport(uint64_t conn) : conn_(conn) {}
    bool write(const uint8_t* data, size_t len, int) override {
        std::lock_guard<std::mutex> l(g_hs.mu);
        if (conn_ != g_hs.conn) return false;
        g_hs.on_write(data, len);
        return true;
    }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        {
            std::lock_guard<std::mutex> l(g_hs.mu);
            if (conn_ != g_hs.conn) return -1;
            g_hs.pump(now_ns());
            size_t n = std::min(cap, g_hs.rx.size() - g_hs.rx_pos);
            if (n) {
                std::copy(g_hs.rx.begin() + g_hs.rx_pos, g_hs.rx.begin() + g_hs.rx_pos + n, buf);
                g_hs.rx_pos += n;
                return long(n);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 2)));
        return 0;
    }

private:
    uint64_t conn_;
};

}  // namespace

namespace qb {
std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    std::lock_guard<std::mutex> l(g_hs.mu);
    if (!g_hs.present && g_hs.back_at && now_ns() >= g_hs.back_at) g_hs.present = true;
    if (!g_hs.present) {
        err = OpenError::NotFound;
        detail = "fake: no Quest";
        return nullptr;
    }
    // A new connection drops what was in flight on the old one
    ++g_hs.conn;
    g_hs.rx.clear();
    g_hs.rx_pos = 0;
    err = OpenError::None;
    detail.clear();
    return std::make_unique<HeadsetTransport>(g_hs.conn);
}
}  // namespace qb
