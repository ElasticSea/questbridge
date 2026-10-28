#include "audio.hpp"

#include <algorithm>
#include <cstring>

namespace ql::audio {

namespace {
constexpr uint8_t kRootPointer[8] = {0, 0, 0, 0, 1, 0, 1, 0};  // struct: 1 data word, 1 pointer
}

std::vector<StreamMessage> encode_block(uint64_t ts_ns, std::span<const uint8_t> pcm) {
    Bytes seg(kRootPointer, kRootPointer + 8);
    seg.resize(24);
    wr64(&seg[8], ts_ns);
    wr32(&seg[16], 1);                               // list pointer, offset 0
    wr32(&seg[20], uint32_t(pcm.size() << 3 | 2));  // element size 1 B, n elements
    seg.insert(seg.end(), pcm.begin(), pcm.end());
    seg.resize((seg.size() + 7) / 8 * 8);
    Bytes table(8);
    wr32(&table[4], uint32_t(seg.size() / 8));
    uint8_t flags = pad_body(seg, kMessageAlign);
    return {{kTopic, kFlagsNormal, std::move(table)}, {kTopic, flags, std::move(seg)}};
}

std::vector<StreamMessage> encode_playback(uint64_t ts_ns, std::span<const float> stereo) {
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(stereo.data()), stereo.size_bytes());
    return encode_block(ts_ns, bytes);  // float32 LE: every target is little-endian
}

bool parse_block(std::span<const uint8_t> seg, uint64_t& ts_ns, std::span<const uint8_t>& pcm) {
    if (seg.size() < 24 || std::memcmp(seg.data(), kRootPointer, 8) != 0) return false;
    uint32_t lo = rd32(&seg[16]), hi = rd32(&seg[20]);
    if (lo != 1 || (hi & 7) != 2) return false;
    size_t n = hi >> 3;
    if (24 + n > seg.size()) return false;
    ts_ns = rd64(&seg[8]);
    pcm = seg.subspan(24, n);
    return true;
}

// --- PlaybackBuffer --------------------------------------------------------

PlaybackBuffer::PlaybackBuffer(size_t max_frames, size_t prime_frames)
    : ring_(max_frames * kChannels), max_frames_(max_frames), prime_frames_(std::min(prime_frames, max_frames)) {}

void PlaybackBuffer::push(float l, float r) {
    if (count_ == max_frames_) {  // full: drop the oldest frame
        head_ = (head_ + 1) % max_frames_;
        --count_;
        ++dropped_;
    }
    size_t i = (head_ + count_) % max_frames_ * kChannels;
    ring_[i] = l;
    ring_[i + 1] = r;
    ++count_;
}

void PlaybackBuffer::write(const float* in, size_t frames, uint32_t rate) {
    if (!in || !rate) return;
    std::lock_guard<std::mutex> l(mu_);
    if (rate != rate_) {
        rate_ = rate;
        have_prev_ = false;
    }
    if (rate == kRate) {
        for (size_t i = 0; i < frames; ++i) push(in[2 * i], in[2 * i + 1]);
        return;
    }
    // Linear interpolation; pos_ = next output position in input frames after prev_
    const double step = double(rate) / kRate;
    for (size_t i = 0; i < frames; ++i) {
        const float* cur = in + 2 * i;
        if (!have_prev_) {
            push(cur[0], cur[1]);
            prev_[0] = cur[0];
            prev_[1] = cur[1];
            have_prev_ = true;
            pos_ = step;
            continue;
        }
        while (pos_ < 1.0) {
            float t = float(pos_);
            push(prev_[0] + (cur[0] - prev_[0]) * t, prev_[1] + (cur[1] - prev_[1]) * t);
            pos_ += step;
        }
        pos_ -= 1.0;
        prev_[0] = cur[0];
        prev_[1] = cur[1];
    }
}

size_t PlaybackBuffer::read(float* out, size_t frames) {
    std::lock_guard<std::mutex> l(mu_);
    if (!primed_ && count_ >= prime_frames_ && count_ > 0) primed_ = true;
    size_t n = primed_ ? std::min(frames, count_) : 0;
    for (size_t i = 0; i < n; ++i) {
        size_t j = (head_ + i) % max_frames_ * kChannels;
        out[2 * i] = ring_[j];
        out[2 * i + 1] = ring_[j + 1];
    }
    std::fill(out + 2 * n, out + 2 * frames, 0.0f);
    head_ = (head_ + n) % max_frames_;
    count_ -= n;
    if (n < frames) primed_ = false;  // ran dry: wait for a cushion again
    return n;
}

size_t PlaybackBuffer::buffered() const {
    std::lock_guard<std::mutex> l(mu_);
    return count_;
}

uint64_t PlaybackBuffer::dropped_frames() const {
    std::lock_guard<std::mutex> l(mu_);
    return dropped_;
}

void PlaybackBuffer::clear() {
    std::lock_guard<std::mutex> l(mu_);
    head_ = count_ = 0;
    primed_ = false;
    have_prev_ = false;
}

// --- MicBuffer -------------------------------------------------------------

void MicBuffer::write(std::span<const int16_t> samples) {
    const size_t cap = ring_.size();
    for (int16_t s : samples) {
        if (count_ == cap) {
            head_ = (head_ + 1) % cap;
            --count_;
        }
        ring_[(head_ + count_) % cap] = s;
        ++count_;
    }
}

void MicBuffer::write_le(std::span<const uint8_t> pcm) {
    std::vector<int16_t> s(pcm.size() / 2);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(rd16(&pcm[2 * i]));
    write(s);
}

size_t MicBuffer::read(int16_t* out, size_t max_frames) {
    size_t n = std::min(max_frames, count_);
    for (size_t i = 0; i < n; ++i) out[i] = ring_[(head_ + i) % ring_.size()];
    head_ = (head_ + n) % ring_.size();
    count_ -= n;
    return n;
}

// --- Pacer -----------------------------------------------------------------

int Pacer::due(int64_t now) {
    if (!started_) {
        started_ = true;
        next_ = now;
    }
    if (now - next_ > max_lag_) {
        skipped_ += uint64_t((now - next_) / period_);
        next_ = now;
    }
    int n = 0;
    while (now >= next_) {
        ++n;
        next_ += period_;
    }
    return n;
}

}  // namespace ql::audio
