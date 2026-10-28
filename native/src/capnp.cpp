// SPDX-FileCopyrightText: 2026 ElasticSea
// SPDX-License-Identifier: Apache-2.0
#include "capnp.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>

namespace qb::capnp {

Ptr::Ptr(Node n) : p_(std::make_unique<Node>(std::move(n))) {}
Ptr::Ptr(const Ptr& o) : p_(o.p_ ? std::make_unique<Node>(*o.p_) : nullptr) {}
Ptr& Ptr::operator=(const Ptr& o) {
    if (this != &o) p_ = o.p_ ? std::make_unique<Node>(*o.p_) : nullptr;
    return *this;
}
Ptr::~Ptr() = default;

namespace {

constexpr uint32_t kBits[6] = {0, 1, 8, 16, 32, 64};

int64_t s30(uint64_t v) {
    v &= 0x3FFFFFFF;
    return (v & (1u << 29)) ? int64_t(v) - (int64_t(1) << 30) : int64_t(v);
}

void fail(const char* what) { throw std::runtime_error(std::string("capnp: ") + what); }

class Reader {
public:
    explicit Reader(std::span<const uint8_t> seg) : seg_(seg) {}

    uint64_t word(int64_t i) const {
        if (i < 0 || size_t(i + 1) * 8 > seg_.size()) fail("pointer out of bounds");
        return rd64(&seg_[size_t(i) * 8]);
    }

    Ptr ptr(int64_t at, int depth = 0) const {
        if (depth > 64) fail("nesting too deep");
        uint64_t w = word(at);
        if (w == 0) return nullptr;
        uint32_t kind = w & 3;
        if (kind == 2) fail("far pointers unsupported (multi-segment)");
        if (kind == 3) fail("capability pointers unsupported");
        int64_t target = at + 1 + s30((w & 0xFFFFFFFF) >> 2);
        Node n;
        if (kind == 0) {
            n = read_struct(target, uint16_t(w >> 32), uint16_t(w >> 48), depth);
        } else {
            uint32_t esz = (w >> 32) & 7;
            uint32_t count = uint32_t(w >> 35);
            if (esz == 7) {
                uint64_t tag = word(target);
                int64_t elems = s30((tag & 0xFFFFFFFF) >> 2);
                n.kind = Kind::Composite;
                n.dsz = uint16_t(tag >> 32);
                n.psz = uint16_t(tag >> 48);
                n.list_words = count;
                int64_t step = n.dsz + n.psz;
                if (elems < 0 || elems * step > count) fail("bad composite list");
                for (int64_t i = 0; i < elems; ++i)
                    n.elems.push_back(read_struct(target + 1 + i * step, n.dsz, n.psz, depth));
            } else if (esz == 6) {
                n.kind = Kind::PtrList;
                word(target + int64_t(count) - 1);  // bounds check
                for (uint32_t i = 0; i < count; ++i) n.ptrs.push_back(ptr(target + i, depth + 1));
            } else {
                n.kind = Kind::Prim;
                n.esz = uint8_t(esz);
                n.count = count;
                size_t nbytes = (size_t(count) * kBits[esz] + 7) / 8;
                if (target < 0 || size_t(target) * 8 + nbytes > seg_.size()) fail("list out of bounds");
                n.bytes.assign(seg_.begin() + size_t(target) * 8, seg_.begin() + size_t(target) * 8 + nbytes);
            }
        }
        n.offset = target;
        return Ptr(std::move(n));
    }

private:
    std::span<const uint8_t> seg_;

    Node read_struct(int64_t at, uint16_t dsz, uint16_t psz, int depth) const {
        Node n;
        n.kind = Kind::Struct;
        for (int64_t i = 0; i < dsz; ++i) n.data.push_back(word(at + i));
        for (int64_t i = 0; i < psz; ++i) n.ptrs.push_back(ptr(at + dsz + i, depth + 1));
        return n;
    }
};

class Writer {
public:
    explicit Writer(uint32_t words) : seg_(size_t(words) * 8) {}

    void put(int64_t i, uint64_t w) {
        if (i < 0 || size_t(i + 1) * 8 > seg_.size()) fail("segment too small");
        wr64(&seg_[size_t(i) * 8], w);
    }

    int64_t alloc(const Node& n) {
        int64_t at = n.offset >= 0 ? n.offset : next_;
        next_ = std::max(next_, at + int64_t(n.size_words()));
        return at;
    }

    void ptr(int64_t at, const Ptr& p) {
        if (!p) return;
        const Node& n = *p;
        int64_t target = alloc(n);
        uint64_t off = uint64_t(target - at - 1) & 0x3FFFFFFF;
        switch (n.kind) {
            case Kind::Struct:
                put(at, (off << 2) | (uint64_t(n.data.size()) << 32) | (uint64_t(n.ptrs.size()) << 48));
                write_struct(target, n);
                break;
            case Kind::Composite: {
                uint32_t body = uint32_t(n.elems.size()) * (n.dsz + n.psz);
                uint32_t w = n.list_words ? n.list_words : body;
                put(at, (off << 2) | 1 | (uint64_t(7) << 32) | (uint64_t(w) << 35));
                put(target, (uint64_t(n.elems.size()) << 2) | (uint64_t(n.dsz) << 32) | (uint64_t(n.psz) << 48));
                for (size_t i = 0; i < n.elems.size(); ++i) write_struct(target + 1 + int64_t(i) * (n.dsz + n.psz), n.elems[i]);
                break;
            }
            case Kind::PtrList:
                put(at, (off << 2) | 1 | (uint64_t(6) << 32) | (uint64_t(n.ptrs.size()) << 35));
                for (size_t i = 0; i < n.ptrs.size(); ++i) ptr(target + int64_t(i), n.ptrs[i]);
                break;
            case Kind::Prim:
                put(at, (off << 2) | 1 | (uint64_t(n.esz) << 32) | (uint64_t(n.count) << 35));
                if (size_t(target) * 8 + n.bytes.size() > seg_.size()) fail("segment too small");
                std::copy(n.bytes.begin(), n.bytes.end(), seg_.begin() + size_t(target) * 8);
                break;
        }
    }

    void write_struct(int64_t at, const Node& n) {
        for (size_t i = 0; i < n.data.size(); ++i) put(at + int64_t(i), n.data[i]);
        int64_t base = at + int64_t(n.data.size());
        for (size_t i = 0; i < n.ptrs.size(); ++i) ptr(base + int64_t(i), n.ptrs[i]);
    }

    Bytes take() { return std::move(seg_); }

private:
    Bytes seg_;
    int64_t next_ = 1;
};

void walk_into(const Ptr& p, const std::string& path, std::vector<std::pair<std::string, Node*>>& out) {
    if (!p) return;
    out.emplace_back(path, p.get());
    const char* key = p->kind == Kind::PtrList ? ".lp" : ".p";
    for (size_t i = 0; i < p->ptrs.size(); ++i) walk_into(p->ptrs[i], path + key + std::to_string(i), out);
    for (size_t j = 0; j < p->elems.size(); ++j)
        for (size_t i = 0; i < p->elems[j].ptrs.size(); ++i)
            walk_into(p->elems[j].ptrs[i], path + ".lc" + std::to_string(j) + ".p" + std::to_string(i), out);
}

void strip(Node& n) {
    n.offset = -1;
    for (auto& c : n.ptrs)
        if (c) strip(*c);
    for (auto& e : n.elems) strip(e);
}

bool same_node(const Node& a, const Node& b);

bool same_ptrs(const std::vector<Ptr>& a, const std::vector<Ptr>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (bool(a[i]) != bool(b[i])) return false;
        if (a[i] && !same_node(*a[i], *b[i])) return false;
    }
    return true;
}

bool same_node(const Node& a, const Node& b) {
    if (a.kind != b.kind || a.data != b.data || !same_ptrs(a.ptrs, b.ptrs)) return false;
    switch (a.kind) {
        case Kind::Prim: return a.esz == b.esz && a.count == b.count && a.bytes == b.bytes;
        case Kind::Composite:
            if (a.dsz != b.dsz || a.psz != b.psz || a.list_words != b.list_words || a.elems.size() != b.elems.size())
                return false;
            for (size_t i = 0; i < a.elems.size(); ++i)
                if (!same_node(a.elems[i], b.elems[i])) return false;
            return true;
        default: return true;
    }
}

}  // namespace

Node Node::make_struct(std::vector<uint64_t> data, std::vector<Ptr> ptrs) {
    Node n;
    n.kind = Kind::Struct;
    n.data = std::move(data);
    n.ptrs = std::move(ptrs);
    return n;
}

Node Node::make_text(const std::string& s) {
    Node n;
    n.kind = Kind::Prim;
    n.esz = 2;
    n.bytes.assign(s.begin(), s.end());
    n.bytes.push_back(0);
    n.count = uint32_t(n.bytes.size());
    return n;
}

uint32_t Node::size_words() const {
    switch (kind) {
        case Kind::Struct: return uint32_t(data.size() + ptrs.size());
        case Kind::Composite: return 1 + uint32_t(elems.size()) * (dsz + psz);
        case Kind::PtrList: return uint32_t(ptrs.size());
        case Kind::Prim: return uint32_t((uint64_t(count) * kBits[esz] + 63) / 64);
    }
    return 0;
}

std::string Node::text() const {
    std::string s(bytes.begin(), bytes.end());
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

Ptr decode(std::span<const uint8_t> msg, uint32_t* seg_words) {
    if (msg.size() < 8) fail("truncated segment table");
    if (rd32(&msg[0]) != 0) fail("multi-segment messages unsupported");
    uint32_t words = rd32(&msg[4]);
    if (msg.size() < 8 + size_t(words) * 8) fail("truncated segment");
    if (seg_words) *seg_words = words;
    if (words == 0) return nullptr;
    return Reader(msg.subspan(8, size_t(words) * 8)).ptr(0);
}

uint32_t total_words(const Ptr& root) {
    uint32_t n = 0;
    for (auto& [path, node] : walk(root)) n += node->size_words();
    return n;
}

Bytes encode(const Ptr& root, uint32_t seg_words) {
    if (!seg_words) seg_words = 1 + total_words(root);
    Writer w(seg_words);
    w.ptr(0, root);
    Bytes seg = w.take();
    Bytes out(8);
    wr32(&out[0], 0);
    wr32(&out[4], seg_words);
    out.insert(out.end(), seg.begin(), seg.end());
    return out;
}

Bytes encode(const Ptr& root, uint32_t seg_words, const std::vector<std::string>& order) {
    return encode(layout(root, order), seg_words);
}

std::vector<std::pair<std::string, Node*>> walk(const Ptr& root) {
    std::vector<std::pair<std::string, Node*>> out;
    walk_into(root, "r", out);
    return out;
}

std::vector<std::string> allocation_order(const Ptr& root) {
    auto nodes = walk(root);
    std::stable_sort(nodes.begin(), nodes.end(), [](auto& a, auto& b) { return a.second->offset < b.second->offset; });
    std::vector<std::string> order;
    int64_t at = 1;
    for (auto& [path, n] : nodes) {
        if (n->offset > at) order.push_back("gap:" + std::to_string(n->offset - at));
        order.push_back(path);
        at = std::max(at, n->offset + int64_t(n->size_words()));
    }
    return order;
}

Ptr strip_offsets(const Ptr& root) {
    Ptr copy = root;
    if (copy) strip(*copy);
    return copy;
}

Ptr layout(const Ptr& root, const std::vector<std::string>& order) {
    Ptr tree = strip_offsets(root);
    auto nodes = walk(tree);
    std::map<std::string, Node*> by_path(nodes.begin(), nodes.end());
    int64_t at = 1;
    auto place = [&](Node* n) {
        n->offset = at;
        at += n->size_words();
    };
    for (const auto& p : order) {
        if (p.rfind("gap:", 0) == 0) {
            at += std::stoll(p.substr(4));
        } else if (auto it = by_path.find(p); it != by_path.end() && it->second->offset < 0) {
            place(it->second);
        }
    }
    for (auto& [path, n] : nodes)
        if (n->offset < 0) place(n);
    return tree;
}

bool same(const Ptr& a, const Ptr& b) {
    if (bool(a) != bool(b)) return false;
    return !a || same_node(*a, *b);
}

View View::root(std::span<const uint8_t> segment) { return at_pointer(segment, 0); }

View View::at_pointer(std::span<const uint8_t> seg, size_t word) {
    size_t words = seg.size() / 8;
    if (word >= words) return {};
    uint64_t w = rd64(&seg[word * 8]);
    if (w == 0 || (w & 3) != 0) return {};  // null, or not a struct pointer
    int64_t target = int64_t(word) + 1 + s30(w >> 2);
    uint16_t dw = uint16_t(w >> 32), pw = uint16_t(w >> 48);
    if (target < 0 || size_t(target) + dw + pw > words) return {};
    return View(seg, size_t(target) * 8, dw, pw);
}

View View::ptr(size_t i) const {
    if (seg_.empty() || i >= pwords_) return {};
    return at_pointer(seg_, at_ / 8 + dwords_ + i);
}

size_t View::list(size_t i, View* out, size_t max_out) const {
    if (seg_.empty() || i >= pwords_) return 0;
    size_t words = seg_.size() / 8, word = at_ / 8 + dwords_ + i;
    uint64_t w = rd64(&seg_[word * 8]);
    if ((w & 3) != 1 || ((w >> 32) & 7) != 7) return 0;  // not a composite list
    int64_t tag = int64_t(word) + 1 + s30(w >> 2);
    uint64_t list_words = w >> 35;
    if (tag < 0 || size_t(tag) + 1 + list_words > words) return 0;
    uint64_t t = rd64(&seg_[size_t(tag) * 8]);
    size_t count = size_t((t >> 2) & 0x3FFFFFFF);
    uint16_t dw = uint16_t(t >> 32), pw = uint16_t(t >> 48);
    if (uint64_t(count) * (dw + pw) > list_words) return 0;
    for (size_t e = 0; e < count && e < max_out; ++e)
        out[e] = View(seg_, (size_t(tag) + 1 + e * (dw + pw)) * 8, dw, pw);
    return count;
}

}  // namespace qb::capnp
