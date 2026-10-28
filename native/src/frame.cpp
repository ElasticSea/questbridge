#include "frame.hpp"

#include <cstring>
#include <stdexcept>

#include "templates.hpp"

namespace ql {

namespace {

// Byte offset of the root struct's data section inside a segment
size_t data_offset(const Bytes& seg) {
    if (seg.size() < 8) throw std::invalid_argument("EncodedSlice header too short");
    return 8 + 8 * size_t((rd64(seg.data()) & 0xFFFFFFFF) >> 2);
}

void put_f32(uint8_t* p, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    wr32(p, u);
}

// Struct a pointer at byte offset ptr of the segment points to (capnp struct pointer)
size_t struct_target(const Bytes& seg, size_t ptr) {
    if (ptr + 8 > seg.size()) throw std::invalid_argument("EncodedSlice header too short");
    uint64_t v = rd64(seg.data() + ptr);
    if ((v & 3) != 0 || v == 0) throw std::invalid_argument("EncodedSlice FovPort is not a struct pointer");
    int32_t off = int32_t(uint32_t(v)) >> 2;
    return size_t(int64_t(ptr) + 8 + 8 * int64_t(off));
}

}  // namespace

bool fits_message_cap(std::span<const SliceData> slices) {
    // Header plus at most 4 bytes of padding (video bodies pad to 4)
    for (const auto& s : slices)
        if (s.csd.size() + kHeaderSize + 4 > kMaxStreamMessage || s.video.size() + kHeaderSize + 4 > kMaxStreamMessage)
            return false;
    return true;
}

void patch_slice_header(Bytes& header, size_t slice, size_t count, const SliceData& s, const FrameParams& p) {
    namespace es = encoded_slice;
    size_t d = data_offset(header);
    if (header.size() < d + 8 * (es::kTimingWords[5] + 1)) throw std::invalid_argument("EncodedSlice header too short");
    uint8_t* h = header.data() + d;
    wr64(h + es::kIndex, p.index);
    if (p.render_pose) {
        for (int i = 0; i < 4; ++i) put_f32(h + es::kPose + 4 * i, p.render_pose->orientation[i]);
        for (int i = 0; i < 3; ++i) put_f32(h + es::kPose + 16 + 4 * i, p.render_pose->position[i]);
    }
    if (p.rectify_mesh_id) wr32(h + es::kRectifyMeshId, *p.rectify_mesh_id);
    if (p.fov) {
        for (size_t eye = 0; eye < 2; ++eye) {
            size_t t = struct_target(header, d + 8 * (es::kFovWord + eye));
            if (t + 16 > header.size()) throw std::invalid_argument("EncodedSlice FovPort out of range");
            const FovPort& f = (*p.fov)[eye];
            put_f32(header.data() + t, f.left);
            put_f32(header.data() + t + 4, f.right);
            put_f32(header.data() + t + 8, f.up);
            put_f32(header.data() + t + 12, f.down);
        }
    }
    h[es::kSliceIndex] = uint8_t(slice);
    h[es::kSliceFlags] = uint8_t((h[es::kSliceFlags] & ~3) | (p.intra ? 1 : 0) | (slice + 1 == count ? 2 : 0));
    wr32(h + es::kCsdLength, uint32_t(s.csd.size()));
    wr32(h + es::kVideoLength, uint32_t(s.video.size()));
    // Shift all timestamps together so the frame targets the requested vsync
    int64_t shift = p.target_vsync_quest_ns - int64_t(rd64(h + 8 * es::kTargetVsyncWord));
    for (size_t w : es::kTimingWords) {
        uint64_t v = rd64(h + 8 * w);
        if (v) wr64(h + 8 * w, uint64_t(int64_t(v) + shift));
    }
}

std::vector<StreamMessage> build_frame(std::span<const SliceData> slices, const FrameParams& p) {
    const auto& tmpl = templates::slice_headers();
    if (slices.empty() || slices.size() > tmpl.size()) throw std::invalid_argument("unsupported slice count");
    std::vector<StreamMessage> out;
    out.reserve(slices.size() * 4);
    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& t = tmpl[i];
        const auto& s = slices[i];
        Bytes header(t.header.begin(), t.header.end());
        patch_slice_header(header, i, slices.size(), s, p);
        out.push_back({t.topic, kFlagsNormal, Bytes(t.table.begin(), t.table.end())});
        out.push_back({t.topic, kFlagsNormal, std::move(header)});
        for (auto payload : {s.csd, s.video}) {
            if (payload.empty()) continue;
            Bytes body(payload.begin(), payload.end());
            uint8_t flags = pad_body(body);
            out.push_back({t.topic, flags, std::move(body)});
        }
    }
    return out;
}

}  // namespace ql
