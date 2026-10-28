// Cap'n Proto bodies of Meta's control messages, byte-exact by allocation order
// (mirrors tests/test_roundtrip.py ControlCapnp).
#include "capnp.hpp"
#include "harness.hpp"

using namespace qb;
using namespace qb::capnp;

namespace {

// Fixture tree JSON (tools/capnp_codec.py node format) -> Node
Ptr from_json(const json::Value& v);

Node struct_from_json(const json::Value& v) {
    Node n;
    for (const auto& w : v["s"].items()) n.data.push_back(w.as_u64());
    for (const auto& c : v["p"].items()) n.ptrs.push_back(from_json(c));
    return n;
}

Ptr from_json(const json::Value& v) {
    if (v.is_null()) return nullptr;
    Node n;
    if (v.has("s")) {
        n = struct_from_json(v);
    } else if (v.has("lc")) {
        n.kind = Kind::Composite;
        n.dsz = uint16_t(v["d"].as_u64());
        n.psz = uint16_t(v["q"].as_u64());
        for (const auto& e : v["lc"].items()) n.elems.push_back(struct_from_json(e));
        n.list_words = v.has("w") ? uint32_t(v["w"].as_u64()) : 0;
    } else if (v.has("lp")) {
        n.kind = Kind::PtrList;
        for (const auto& c : v["lp"].items()) n.ptrs.push_back(from_json(c));
    } else {
        n.kind = Kind::Prim;
        n.esz = uint8_t(v["l"].as_u64());
        n.count = uint32_t(v["n"].as_u64());
        n.bytes = v["b"].as_hex();
    }
    if (v.has("o")) n.offset = v["o"].as_i64();
    return Ptr(std::move(n));
}

std::vector<std::string> order_of(const json::Value& c) {
    std::vector<std::string> out;
    for (const auto& p : c["order"].items()) out.push_back(p.as_string());
    return out;
}

template <class F>
size_t each_capnp(F f) {
    size_t n = 0;
    for (const auto& sc : qbtest::scenarios())
        for (const auto& e : qbtest::fixture(sc, "control").items())
            if (e.has("capnp")) {
                f(e, e["raw"].as_hex());
                ++n;
            }
    return n;
}

}  // namespace

TEST(capnp_bodies_reencode_by_allocation_order) {
    size_t n = each_capnp([](const json::Value& e, const Bytes& raw) {
        const auto& c = e["capnp"];
        Bytes capnp = encode(from_json(c["tree"]), uint32_t(c["words"].as_u64()), order_of(c));
        REQUIRE(raw.size() >= 16 + capnp.size());
        CHECK_EQ(capnp, Bytes(raw.begin() + 16, raw.begin() + 16 + capnp.size()));
        // Remaining bytes after the Cap'n Proto message are zero padding
        CHECK(std::all_of(raw.begin() + 16 + capnp.size(), raw.end(), [](uint8_t b) { return b == 0; }));
    });
    CHECK(n > 0);
}

TEST(capnp_decode_matches_fixture_tree) {
    size_t n = each_capnp([](const json::Value& e, const Bytes& raw) {
        const auto& c = e["capnp"];
        uint32_t words = 0;
        Ptr tree = decode(std::span(raw).subspan(16), &words);
        CHECK_EQ(words, uint32_t(c["words"].as_u64()));
        CHECK(same(tree, from_json(c["tree"])));
        CHECK(allocation_order(tree) == order_of(c));
        // Recorded offsets alone reproduce Meta's bytes
        Bytes again = encode(tree, words);
        CHECK_EQ(again, Bytes(raw.begin() + 16, raw.begin() + 16 + again.size()));
    });
    CHECK(n > 0);
}

TEST(capnp_own_order_roundtrips_semantically) {
    each_capnp([](const json::Value& e, const Bytes&) {
        Ptr tree = from_json(e["capnp"]["tree"]);
        Bytes ours = encode(strip_offsets(tree));
        CHECK(same(decode(ours), tree));
    });
}

TEST(capnp_text_and_struct_builders) {
    std::vector<Ptr> ptrs;
    ptrs.push_back(Ptr(Node::make_text("USB3")));
    ptrs.push_back(nullptr);
    Ptr root(Node::make_struct({0x1234, 5}, std::move(ptrs)));
    Ptr back = decode(encode(root));
    REQUIRE(back);
    CHECK_EQ(back->word(0), uint64_t(0x1234));
    CHECK_EQ(back->child(0)->text(), std::string("USB3"));
    CHECK(back->child(1) == nullptr);
}

TEST(inner_header_size_field) {
    // inner.a is the Cap'n Proto size in 16-byte units, rounded up
    each_capnp([](const json::Value& e, const Bytes&) {
        uint64_t bytes = 8 + e["capnp"]["words"].as_u64() * 8;
        CHECK_EQ(e["inner"]["a"].as_u64(), (bytes + 15) / 16);
    });
}

TEST(inner_header_reencode) {
    for (const auto& sc : qbtest::scenarios()) {
        for (const auto& e : qbtest::fixture(sc, "control").items()) {
            Bytes raw = e["raw"].as_hex();
            const auto& i = e["inner"];
            Bytes inner(8);
            inner[0] = uint8_t(i["type"].as_u64());
            inner[1] = uint8_t(i["flags"].as_u64());
            wr16(&inner[2], uint16_t(i["a"].as_u64()));
            wr32(&inner[4], uint32_t(i["b"].as_u64()));
            CHECK_EQ(inner, Bytes(raw.begin() + 8, raw.begin() + 16));
        }
    }
}
