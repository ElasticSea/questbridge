// Recorded messages from Meta's host that ours is built from (templates_data.cpp is generated
// by tools/gen_native_templates.py and checked against the fixtures by the tests).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ql::templates {

struct Control {
    uint8_t type;
    uint16_t version;      // root data word 0, low 16 bits
    uint8_t inner_flags;   // inner header flags byte
    std::span<const uint8_t> capnp;  // segment table + segment, as Meta sent it
};

struct SetupMessage {
    int64_t t_ns;  // time in the recorded session (gaps between messages matter, capped at 0.5 s)
    uint8_t topic;
    uint8_t flags;
    std::span<const uint8_t> body;
};

struct SliceTemplate {
    uint8_t topic;                    // 10 + slice index
    std::span<const uint8_t> table;   // 8-byte capnp segment table, sent as its own message
    std::span<const uint8_t> header;  // EncodedSlice segment
};

const std::vector<Control>& controls();
const std::vector<SetupMessage>& stream_setup();
const std::vector<SliceTemplate>& slice_headers();

}  // namespace ql::templates
