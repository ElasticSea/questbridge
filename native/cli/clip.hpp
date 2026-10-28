// Replay clips (out/clips/*.json, written by tools/extract_replay.py and
// tools/encode_pattern.py) for qbhost.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace qbcli {

// A recorded clip, split back into slices: per topic, segment table, EncodedSlice header,
// csd (if csdLength > 0), video; payload lengths come from the header.
struct Clip {
    uint64_t frame_index = 1;
    uint32_t height = 0;  // picture height: the first frame's strips (heightInMacroBlocks x 16)
    bool hevc = false;    // first csd starts with an HEVC VPS (else H.264 SPS)
    std::vector<std::vector<std::pair<qb::Bytes, qb::Bytes>>> frames;  // per frame: (csd, video) per slice
    std::vector<bool> intra;  // per frame: slice 0's isIntraFrame flag
};

// Type of the first NAL after an Annex-B start code: true for an HEVC VPS / SPS / PPS (32-34)
inline bool is_hevc_csd(const qb::Bytes& csd) {
    for (size_t i = 0; i + 3 < csd.size(); ++i)
        if (csd[i] == 0 && csd[i + 1] == 0 && csd[i + 2] == 1) {
            int t = (csd[i + 3] >> 1) & 0x3F;
            return (csd[i + 3] & 0x80) == 0 && t >= 32 && t <= 34;
        }
    return false;
}

inline uint32_t u32_at(const qb::Bytes& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

inline Clip load_clip(const std::string& path) {
    auto data = qb::json::parse_file(path);
    Clip clip;
    if (data.has("frame_index")) clip.frame_index = data["frame_index"].as_u64();
    std::vector<const qb::json::Value*> frames;
    if (data.has("frames"))
        for (const auto& f : data["frames"].items()) frames.push_back(&f);
    else
        frames.push_back(&data["frame"]);
    for (const auto* f : frames) {
        const auto& msgs = f->items();
        std::vector<std::pair<qb::Bytes, qb::Bytes>> slices;
        bool intra = true;
        for (size_t i = 0; i + 1 < msgs.size();) {
            qb::Bytes table = msgs[i]["body"].as_hex();
            if (table.size() != 8 || msgs[i + 1]["topic"].as_u64() != msgs[i]["topic"].as_u64()) {
                ++i;
                continue;
            }
            qb::Bytes hdr = msgs[i + 1]["body"].as_hex();
            size_t d = 8 + 8 * ((u32_at(hdr, 0)) >> 2);
            uint32_t csd_len = u32_at(hdr, d + 60), video_len = u32_at(hdr, d + 64);
            if (clip.frames.empty()) clip.height += u32_at(hdr, d + 56) * 16;
            if (slices.empty()) intra = hdr[d + 49] & 1;
            i += 2;
            qb::Bytes csd, video;
            if (csd_len) {
                csd = msgs[i++]["body"].as_hex();
                csd.resize(csd_len);
            }
            video = msgs[i++]["body"].as_hex();
            video.resize(video_len);
            if (clip.frames.empty() && slices.empty()) clip.hevc = is_hevc_csd(csd);
            slices.emplace_back(std::move(csd), std::move(video));
        }
        clip.frames.push_back(std::move(slices));
        clip.intra.push_back(intra);
    }
    return clip;
}

}  // namespace qbcli
