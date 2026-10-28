// Schema-less Cap'n Proto codec for single-segment messages (port of tools/capnp_codec.py).
//
// A message decodes to a tree of Nodes. Every pointer target keeps its word offset in the
// segment, so re-encoding places objects exactly where the original builder did. Meta's
// builder allocates in field-init order (not pointer-slot order) and leaves zeroed holes,
// so byte-exact output needs either those offsets or an explicit allocation order.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xrsp.hpp"

namespace ql::capnp {

struct Node;

// Nullable owning pointer with deep copy (a null pointer is a Cap'n Proto null pointer).
class Ptr {
public:
    Ptr() = default;
    Ptr(std::nullptr_t) {}
    explicit Ptr(Node n);
    Ptr(const Ptr& o);
    Ptr(Ptr&&) noexcept = default;
    Ptr& operator=(const Ptr& o);
    Ptr& operator=(Ptr&&) noexcept = default;
    ~Ptr();

    explicit operator bool() const { return bool(p_); }
    Node& operator*() const { return *p_; }
    Node* operator->() const { return p_.get(); }
    Node* get() const { return p_.get(); }

private:
    std::unique_ptr<Node> p_;
};

enum class Kind : uint8_t {
    Struct,     // data words + pointers
    Prim,       // primitive list, element size code 0..5 (2 = bytes / text)
    PtrList,    // list of pointers
    Composite,  // list of structs
};

struct Node {
    Kind kind = Kind::Struct;
    std::vector<uint64_t> data;  // Struct: data section words
    std::vector<Ptr> ptrs;       // Struct: pointer section; PtrList: elements
    std::vector<Node> elems;     // Composite: element structs
    uint16_t dsz = 0, psz = 0;   // Composite: element data / pointer words
    uint32_t list_words = 0;     // Composite: word count stored in the list pointer
    uint8_t esz = 0;             // Prim: element size code
    uint32_t count = 0;          // Prim: element count
    Bytes bytes;                 // Prim: raw element bytes
    int64_t offset = -1;         // word offset in the segment, -1 = unplaced

    static Node make_struct(std::vector<uint64_t> data, std::vector<Ptr> ptrs);
    static Node make_text(const std::string& s);  // NUL-terminated byte list
    uint32_t size_words() const;

    // Struct helpers (bounds-checked; missing fields read as zero / null)
    uint64_t word(size_t i) const { return i < data.size() ? data[i] : 0; }
    uint32_t u32(size_t word, int half) const { return uint32_t(this->word(word) >> (32 * half)); }
    const Node* child(size_t i) const { return i < ptrs.size() ? ptrs[i].get() : nullptr; }
    std::string text() const;  // Prim byte list without trailing NULs
};

// Decode "segment table + segment". seg_words receives the segment size.
Ptr decode(std::span<const uint8_t> msg, uint32_t* seg_words = nullptr);

// Encode with each node's recorded offset (nodes without one are placed depth-first after
// the furthest object so far). seg_words 0 = exactly what the tree needs.
Bytes encode(const Ptr& root, uint32_t seg_words = 0);
// Encode with objects allocated back to back in `order` (paths from walk(), "gap:N" skips
// N zero words); unlisted objects follow depth-first.
Bytes encode(const Ptr& root, uint32_t seg_words, const std::vector<std::string>& order);

// (path, node) for every pointer target, depth-first: "r", "r.p3", "r.lp0", "r.lc1.p0".
std::vector<std::pair<std::string, Node*>> walk(const Ptr& root);
// Paths sorted by recorded offsets: the order the original builder allocated them.
std::vector<std::string> allocation_order(const Ptr& root);
// Copy of tree with offsets assigned by sequential allocation in `order`.
Ptr layout(const Ptr& root, const std::vector<std::string>& order);
Ptr strip_offsets(const Ptr& root);
uint32_t total_words(const Ptr& root);

// Structural equality ignoring offsets
bool same(const Ptr& a, const Ptr& b);

}  // namespace ql::capnp
