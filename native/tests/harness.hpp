// Tiny test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "xrsp.hpp"

namespace qltest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline std::string hex(const ql::Bytes& b, size_t max = 64) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < b.size() && i < max; ++i) {
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    if (b.size() > max) s += "...";
    return s;
}

template <class T>
std::string show(const T& v) {
    if constexpr (std::is_same_v<T, ql::Bytes>) {
        return hex(v) + " (" + std::to_string(v.size()) + " B)";
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        return std::to_string(+v);
    } else {
        std::ostringstream ss;
        ss << v;
        return ss.str();
    }
}

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::cerr << file << ":" << line << ": FAIL " << msg << "\n";
}

// Repo root and fixtures, set by CMake
inline std::string repo(const std::string& rel) { return std::string(QL_REPO_ROOT) + "/" + rel; }
// Parsed once and cached, so range-for over fixture(...).items() is safe
inline const ql::json::Value& fixture(const std::string& scenario, const std::string& name) {
    static std::map<std::string, ql::json::Value> cache;
    std::string path = repo("tests/fixtures/" + scenario + "/" + name + ".json");
    auto it = cache.find(path);
    if (it == cache.end()) it = cache.emplace(path, ql::json::parse_file(path)).first;
    return it->second;
}

// Same list as SCENARIOS in tests/test_roundtrip.py
inline const std::vector<std::string>& scenarios() {
    static const std::vector<std::string> s = {"handshake"};
    return s;
}

}  // namespace qltest

#define QL_CAT2(a, b) a##b
#define QL_CAT(a, b) QL_CAT2(a, b)
#define TEST(name)                                                             \
    static void name();                                                        \
    static qltest::Register QL_CAT(reg_, name)(#name, name);                   \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) qltest::fail(__FILE__, __LINE__, #cond);                  \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto&& qa_ = (a);                                                      \
        auto&& qb_ = (b);                                                      \
        if (!(qa_ == qb_))                                                     \
            qltest::fail(__FILE__, __LINE__,                                   \
                         std::string(#a " == " #b "\n  left:  ") + qltest::show(qa_) + \
                             "\n  right: " + qltest::show(qb_));               \
    } while (0)

// Stop the current test after a failed precondition
#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            qltest::fail(__FILE__, __LINE__, "required: " #cond);              \
            return;                                                            \
        }                                                                      \
    } while (0)
