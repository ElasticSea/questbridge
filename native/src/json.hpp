// Minimal JSON reader for fixtures and replay clips. Numbers keep their source text so
// 64-bit integers (Cap'n Proto data words) round-trip exactly.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xrsp.hpp"

namespace qb::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    bool as_bool() const;
    uint64_t as_u64() const;
    int64_t as_i64() const;
    double as_double() const;
    const std::string& as_string() const;
    Bytes as_hex() const;  // string of hex digits -> bytes

    // Arrays
    size_t size() const;
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[size_t(i)]; }
    const std::vector<Value>& items() const;

    // Objects
    bool has(std::string_view key) const;
    const Value& operator[](std::string_view key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    const std::vector<std::pair<std::string, Value>>& members() const;

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string text_;  // number text or string value
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
};

Value parse(std::string_view text);
Value parse_file(const std::string& path);

Bytes from_hex(std::string_view hex);

}  // namespace qb::json
