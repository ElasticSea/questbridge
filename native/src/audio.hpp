// Link audio on topic 6 (PROTOCOL_NOTES "Audio", port of tools/decode_audio.py).
//
// One 10 ms block = one single-segment Cap'n Proto message {u64 ts (sender clock), Data pcm}
// sent as two XRSP messages: the 8-byte segment table, then the segment padded so the whole
// message is a multiple of 1 KiB. PC->Quest: 480 frames 48 kHz stereo float32 interleaved
// (3840 B), every 10 ms. Quest->PC (mic): 480 frames 48 kHz mono int16 (960 B).
// Meta's topic-26 AudioControl setup is part of the recorded stream setup (templates).
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "xrsp.hpp"

namespace ql::audio {

constexpr uint8_t kTopic = 6;
constexpr uint32_t kRate = 48000;
constexpr size_t kBlockFrames = 480;  // 10 ms
constexpr size_t kChannels = 2;       // to the Quest
constexpr int64_t kBlockNs = 10'000'000;
constexpr size_t kMessageAlign = 1024;

// Segment table + padded segment for one block, byte-identical to Meta's encode_audio().
std::vector<StreamMessage> encode_block(uint64_t ts_ns, std::span<const uint8_t> pcm);
// One PC->Quest block from kBlockFrames * kChannels floats.
std::vector<StreamMessage> encode_playback(uint64_t ts_ns, std::span<const float> stereo);

// Parse an audio segment (unpadded body of the second message). False for anything else,
// including the 8-byte segment table.
bool parse_block(std::span<const uint8_t> segment, uint64_t& ts_ns, std::span<const uint8_t>& pcm);

// Game audio waiting to be sent: stereo float at 48 kHz, written from the engine's audio
// thread (resampled linearly when its rate differs), read by the 10 ms sender. Thread-safe.
// Holds at most max_frames (oldest dropped, bounds latency). After running dry it waits for
// prime_frames before playing again, so bursty writers (Unity: ~21 ms chunks) don't crackle.
class PlaybackBuffer {
public:
    explicit PlaybackBuffer(size_t max_frames = kRate / 10, size_t prime_frames = kRate * 3 / 100);

    void write(const float* interleaved_stereo, size_t frames, uint32_t sample_rate);
    // Fills out with exactly frames stereo frames; silence where nothing is buffered.
    // Returns how many frames were real audio.
    size_t read(float* out, size_t frames);
    size_t buffered() const;
    uint64_t dropped_frames() const;
    void clear();

private:
    void push(float l, float r);

    mutable std::mutex mu_;
    std::vector<float> ring_;
    size_t max_frames_, prime_frames_;
    size_t head_ = 0, count_ = 0;  // frames
    bool primed_ = false;
    uint64_t dropped_ = 0;
    // Resampler state
    uint32_t rate_ = 0;
    double pos_ = 0;  // output position between prev_ and the next input frame, [0, 1)
    float prev_[2] = {};
    bool have_prev_ = false;
};

// Quest microphone: 48 kHz mono int16, newest max_frames kept. Not thread-safe.
class MicBuffer {
public:
    explicit MicBuffer(size_t max_frames = kRate) : ring_(max_frames) {}
    void write(std::span<const int16_t> samples);
    void write_le(std::span<const uint8_t> pcm);  // int16 little-endian bytes
    size_t read(int16_t* out, size_t max_frames);
    size_t buffered() const { return count_; }

private:
    std::vector<int16_t> ring_;
    size_t head_ = 0, count_ = 0;
};

// 10 ms cadence on a monotonic clock. due() says how many blocks to send now; after a stall
// longer than max_lag it skips the missed blocks instead of bursting them.
class Pacer {
public:
    explicit Pacer(int64_t period_ns = kBlockNs, int64_t max_lag_ns = 50'000'000)
        : period_(period_ns), max_lag_(max_lag_ns) {}
    int due(int64_t now_ns);
    int64_t next_ns() const { return next_; }  // valid after the first due()
    uint64_t skipped() const { return skipped_; }
    void reset() { started_ = false; }

private:
    int64_t period_, max_lag_;
    int64_t next_ = 0;
    bool started_ = false;
    uint64_t skipped_ = 0;
};

}  // namespace ql::audio
