// Fake Quest behind open_usb() for the Session API tests: replays the Quest's side of the
// recorded handshake and records what the runtime writes. Defines qb::open_usb, so include it
// in exactly one file per test binary.
#pragma once

#include <chrono>
#include <mutex>
#include <thread>

#include "harness.hpp"
#include "transport.hpp"

namespace {

using namespace qb;

// Plays the Quest's side of the recorded session, records what we send
struct FakeQuest {
    std::mutex mu;
    Bytes rx;
    size_t rx_pos = 0;
    std::vector<Bytes> written;
};
FakeQuest g_quest;
bool g_quest_plugged = false;

class FakeTransport final : public Transport {
public:
    bool write(const uint8_t* data, size_t len, int) override {
        std::lock_guard<std::mutex> l(g_quest.mu);
        g_quest.written.emplace_back(data, data + len);
        return true;
    }
    long read(uint8_t* buf, size_t cap, int timeout_ms) override {
        {
            std::lock_guard<std::mutex> l(g_quest.mu);
            size_t n = std::min(cap, g_quest.rx.size() - g_quest.rx_pos);
            if (n) {
                std::copy(g_quest.rx.begin() + g_quest.rx_pos, g_quest.rx.begin() + g_quest.rx_pos + n, buf);
                g_quest.rx_pos += n;
                return long(n);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
        return 0;
    }
};

template <class F>
bool wait_for(F cond, int ms = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Queue the recorded Quest side (handshake, head poses, mic) and plug it in once
void plug_recorded_quest() {
    std::lock_guard<std::mutex> l(g_quest.mu);
    auto add = [](const Bytes& raw) {
        Bytes x = pack_transfer(std::vector<Bytes>{raw});
        g_quest.rx.insert(g_quest.rx.end(), x.begin(), x.end());
    };
    for (const auto& e : qbtest::fixture("handshake", "control").items())
        if (e["dir"].as_string() == "quest_to_pc") add(e["raw"].as_hex());
    for (const auto& g : qbtest::fixture("handshake", "streams").items())
        if (g["topic"].as_u64() == 34 || (g["topic"].as_u64() == 6 && g["dir"].as_string() == "quest_to_pc"))
            for (const auto& s : g["samples"].items()) add(s["raw"].as_hex());
    g_quest_plugged = true;
}

}  // namespace

namespace qb {
std::unique_ptr<Transport> open_usb(OpenError& err, std::string& detail) {
    std::lock_guard<std::mutex> l(g_quest.mu);
    if (!g_quest_plugged) {
        err = OpenError::NotFound;
        detail = "fake: not plugged";
        return nullptr;
    }
    g_quest_plugged = false;  // one connection
    err = OpenError::None;
    return std::make_unique<FakeTransport>();
}
}  // namespace qb
