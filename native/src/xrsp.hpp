// XRSP framing: messages packed back to back in USB bulk transfers.
//
//   u8  flags      0x10 normal; bit 0x08 = padded body (last byte = pad count, filler 0xDE)
//   u8  topic      0 = padding, 1 = control, others = streams
//   u16 len_words  message size is 4 + len_words * 4 bytes
//   u16 seq        per topic, per direction
//   u16 reserved
//   ... body
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qb {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kFlagsNormal = 0x10;
constexpr uint8_t kFlagPadded = 0x08;
constexpr size_t kHeaderSize = 8;
constexpr size_t kTransferBlock = 1024;

struct Message {
    uint8_t flags = kFlagsNormal;
    uint8_t topic = 0;
    uint16_t seq = 0;
    uint16_t reserved = 0;
    Bytes body;
};

// A message to send on a stream topic; seq is assigned per topic when sent
struct StreamMessage {
    uint8_t topic;
    uint8_t flags;
    Bytes body;
};

// Body must be a multiple of 4 bytes (use pad_body first).
Bytes encode_message(uint8_t flags, uint8_t topic, uint16_t seq, std::span<const uint8_t> body,
                     uint16_t reserved = 0);
Bytes encode_message(const Message& m);

// Parse one raw message (header + body). Returns false if malformed.
bool decode_message(std::span<const uint8_t> raw, Message& out);

// Meta's body padding: flag 0x08, filler 0xDE, last byte = pad count. Pads until the whole
// message (header + body) is a multiple of message_align: 4 for video and topic 31, 1024 for
// audio (6) and telemetry (33). Returns the flags to send (0x10 or 0x18).
uint8_t pad_body(Bytes& body, size_t message_align = 4);
// Inverse of pad_body for a message whose flags carry 0x08.
std::span<const uint8_t> unpad_body(uint8_t flags, std::span<const uint8_t> body);

// Concatenate raw messages and pad to a 1 KiB multiple with a zeroed topic-0 message that
// carries the last message's seq (as Meta's host does).
Bytes pack_transfer(std::span<const Bytes> raw_messages, size_t block = kTransferBlock);

// Splits incoming transfers into messages; messages may span transfers.
class Reassembler {
public:
    void feed(std::span<const uint8_t> data);
    // Next complete non-padding message as raw bytes; false if none buffered.
    bool next(Bytes& raw);
    size_t resyncs() const { return resyncs_; }
    void clear() { buf_.clear(); pos_ = 0; }

private:
    Bytes buf_;
    size_t pos_ = 0;
    size_t resyncs_ = 0;
};

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(rd16(p)) | (uint32_t(rd16(p + 2)) << 16); }
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { wr16(p, uint16_t(v)); wr16(p + 2, uint16_t(v >> 16)); }
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, uint32_t(v)); wr32(p + 4, uint32_t(v >> 32)); }

}  // namespace qb
