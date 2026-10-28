// Touch controllers: topic 35 DeviceInput (buttons, touches, trigger/grip/stick per controller)
// and haptics. Controller poses ride topic 34 with the head pose (pose.hpp). Layouts in
// docs/protocol/spec.md and tools/decode_controllers.py.
//
// Haptics are not a Link topic: Meta's PC opens a RuntimeIPC (topic 31) connection to the Quest's
// HapticsServer (lookup + connect, with a client id the PC picks) and sends each pulse as a call
// on it. Every RuntimeIPC frame is 4 topic-31 messages: {0, 2}, {0, 1, 32, payload size}, a
// packed 32-byte header, the payload.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xrsp.hpp"

namespace qb {

namespace raw_button {  // topic 35 entry buttons word
constexpr uint32_t kA = 0x1, kB = 0x2, kRightStick = 0x4, kX = 0x100, kY = 0x200, kLeftStick = 0x400,
                   kMenu = 0x40000000, kAnyStick = 0x80000000;
}
namespace raw_touch {  // topic 35 entry touches word (same bits on both hands)
constexpr uint32_t kPrimary = 0x1, kSecondary = 0x2, kStick = 0x4, kThumbRest = 0x8, kTrigger = 0x10;
}

struct ControllerInput {
    bool present = false;  // listed in the message (index 0 left, 1 right)
    uint8_t battery = 0;   // %
    uint8_t type = 0;      // 4 with Touch Plus
    uint32_t buttons = 0;  // raw_button bits
    uint32_t touches = 0;  // raw_touch bits
    float trigger = 0, grip = 0;
    float stick[2] = {};   // x, y (-1..1, y up)
};

struct DeviceInput {
    uint8_t flags = 0;             // 0x3 controllers active, bit 0x4 hand tracking active
    uint8_t headset_battery = 0;   // %
    uint16_t controller_mask = 0;  // 3 both, 0 none (controllers put down)
    float ipd = 0;                 // metres
    std::array<ControllerInput, 2> controllers;  // by index; empty list while hands are tracked
};

// nullopt unless segment is a topic 35 DeviceInput segment
std::optional<DeviceInput> parse_device_input(std::span<const uint8_t> segment);

namespace ripc {

constexpr uint8_t kTopic = 31;
constexpr uint32_t kLookup = 0x43E, kConnect = 0x13E, kCall = 0x23E;  // header kinds
constexpr uint32_t kHapticCall = 0x2ADC6769;      // type hash of the haptic call (bytes 6967dc2a)
constexpr uint64_t kHapticsClient = 40998640360;  // the client id Meta's PC picked

// The 4 messages of one PC->Q frame
std::vector<StreamMessage> frame(uint32_t kind, uint64_t client, uint32_t seq, uint32_t tag,
                                 std::span<const uint8_t> payload);
// Lookup of the VrRuntimeService (seq field 0, like Meta's PC)
std::vector<StreamMessage> lookup(uint64_t lookup_id);
std::vector<StreamMessage> connect_haptics(uint64_t client, uint32_t seq);
// controller: 1 left, 2 right
std::vector<StreamMessage> haptic(uint64_t client, uint32_t seq, uint8_t controller, uint64_t duration_ns,
                                  float amplitude);

// What the replayed stream setup (templates) already used: its lookup id and the next seq
struct SetupState {
    uint64_t lookup_id = 0;
    uint32_t next_seq = 1;
};
SetupState from_setup();

}  // namespace ripc

}  // namespace qb
