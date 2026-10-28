#include "xrsp.hpp"

#include <stdexcept>

namespace qb {

Bytes encode_message(uint8_t flags, uint8_t topic, uint16_t seq, std::span<const uint8_t> body, uint16_t reserved) {
    if (body.size() % 4) throw std::invalid_argument("XRSP body must be a multiple of 4 bytes");
    if ((body.size() + 4) / 4 > 0xFFFF) throw std::invalid_argument("XRSP body too large");
    Bytes out(kHeaderSize + body.size());
    out[0] = flags;
    out[1] = topic;
    wr16(&out[2], uint16_t((body.size() + 4) / 4));
    wr16(&out[4], seq);
    wr16(&out[6], reserved);
    std::copy(body.begin(), body.end(), out.begin() + kHeaderSize);
    return out;
}

Bytes encode_message(const Message& m) { return encode_message(m.flags, m.topic, m.seq, m.body, m.reserved); }

bool decode_message(std::span<const uint8_t> raw, Message& out) {
    if (raw.size() < kHeaderSize) return false;
    size_t size = 4 + size_t(rd16(&raw[2])) * 4;
    if (size < kHeaderSize || size != raw.size()) return false;
    out.flags = raw[0];
    out.topic = raw[1];
    out.seq = rd16(&raw[4]);
    out.reserved = rd16(&raw[6]);
    out.body.assign(raw.begin() + kHeaderSize, raw.end());
    return true;
}

uint8_t pad_body(Bytes& body, size_t message_align) {
    size_t n = (message_align - (kHeaderSize + body.size()) % message_align) % message_align;
    if (!n) return kFlagsNormal;
    if (n > 255) throw std::invalid_argument("XRSP pad count exceeds one byte");
    body.insert(body.end(), n - 1, 0xDE);
    body.push_back(uint8_t(n));
    return kFlagsNormal | kFlagPadded;
}

std::span<const uint8_t> unpad_body(uint8_t flags, std::span<const uint8_t> body) {
    if (!(flags & kFlagPadded) || body.empty()) return body;
    size_t n = body.back();
    return n <= body.size() ? body.first(body.size() - n) : body.first(0);
}

Bytes pack_transfer(std::span<const Bytes> raw_messages, size_t block) {
    Bytes data;
    for (const auto& m : raw_messages) data.insert(data.end(), m.begin(), m.end());
    size_t pad = (block - data.size() % block) % block;
    if (pad) {
        if (pad < kHeaderSize) pad += block;
        uint16_t last_seq = raw_messages.empty() ? 0 : rd16(&raw_messages.back()[4]);
        Bytes zeros(pad - kHeaderSize);
        Bytes filler = encode_message(kFlagsNormal, 0, last_seq, zeros);
        data.insert(data.end(), filler.begin(), filler.end());
    }
    return data;
}

void Reassembler::feed(std::span<const uint8_t> data) {
    if (pos_) {
        buf_.erase(buf_.begin(), buf_.begin() + pos_);
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

bool Reassembler::next(Bytes& raw) {
    while (buf_.size() - pos_ >= kHeaderSize) {
        const uint8_t* p = buf_.data() + pos_;
        size_t size = 4 + size_t(rd16(p + 2)) * 4;
        if (size < kHeaderSize) {
            // Corrupt header: drop what we have and resync on the next transfer
            buf_.clear();
            pos_ = 0;
            ++resyncs_;
            return false;
        }
        if (buf_.size() - pos_ < size) return false;
        uint8_t topic = p[1];
        pos_ += size;
        if (topic != 0) {
            raw.assign(p, p + size);
            return true;
        }
    }
    return false;
}

}  // namespace qb
