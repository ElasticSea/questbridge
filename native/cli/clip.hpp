// Replay clips (host/replay/*.json, written by tools/extract_replay.py and
// tools/encode_pattern.py) for qlhost.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace qlcli {

// A recorded clip, split back into slices: per topic, segment table, EncodedSlice header,
// csd (if csdLength > 0), video; payload lengths come from the header.
struct Clip {
    uint64_t frame_index = 1;
    uint32_t height = 0;  // picture height: the first frame's strips (heightInMacroBlocks x 16)
    std::vector<std::vector<std::pair<ql::Bytes, ql::Bytes>>> frames;  // per frame: (csd, video) per slice
};

inline uint32_t u32_at(const ql::Bytes& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

inline Clip load_clip(const std::string& path) {
    auto data = ql::json::parse_file(path);
    Clip clip;
    if (data.has("frame_index")) clip.frame_index = data["frame_index"].as_u64();
    std::vector<const ql::json::Value*> frames;
    if (data.has("frames"))
        for (const auto& f : data["frames"].items()) frames.push_back(&f);
    else
        frames.push_back(&data["frame"]);
    for (const auto* f : frames) {
        const auto& msgs = f->items();
        std::vector<std::pair<ql::Bytes, ql::Bytes>> slices;
        for (size_t i = 0; i + 1 < msgs.size();) {
            ql::Bytes table = msgs[i]["body"].as_hex();
            if (table.size() != 8 || msgs[i + 1]["topic"].as_u64() != msgs[i]["topic"].as_u64()) {
                ++i;
                continue;
            }
            ql::Bytes hdr = msgs[i + 1]["body"].as_hex();
            size_t d = 8 + 8 * ((u32_at(hdr, 0)) >> 2);
            uint32_t csd_len = u32_at(hdr, d + 60), video_len = u32_at(hdr, d + 64);
            if (clip.frames.empty()) clip.height += u32_at(hdr, d + 56) * 16;
            i += 2;
            ql::Bytes csd, video;
            if (csd_len) {
                csd = msgs[i++]["body"].as_hex();
                csd.resize(csd_len);
            }
            video = msgs[i++]["body"].as_hex();
            video.resize(video_len);
            slices.emplace_back(std::move(csd), std::move(video));
        }
        clip.frames.push_back(std::move(slices));
    }
    return clip;
}

}  // namespace qlcli
