#include "json.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ql::json {

namespace {
const Value kNull;

void expect(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string("json: ") + what);
}
}  // namespace

bool Value::as_bool() const {
    expect(type_ == Type::Bool, "not a bool");
    return bool_;
}

uint64_t Value::as_u64() const {
    expect(type_ == Type::Number, "not a number");
    uint64_t v = 0;
    auto r = std::from_chars(text_.data(), text_.data() + text_.size(), v);
    expect(r.ec == std::errc() && r.ptr == text_.data() + text_.size(), "not an unsigned integer");
    return v;
}

int64_t Value::as_i64() const {
    expect(type_ == Type::Number, "not a number");
    int64_t v = 0;
    auto r = std::from_chars(text_.data(), text_.data() + text_.size(), v);
    expect(r.ec == std::errc() && r.ptr == text_.data() + text_.size(), "not an integer");
    return v;
}

double Value::as_double() const {
    expect(type_ == Type::Number, "not a number");
    return std::strtod(text_.c_str(), nullptr);
}

const std::string& Value::as_string() const {
    expect(type_ == Type::String, "not a string");
    return text_;
}

Bytes Value::as_hex() const { return from_hex(as_string()); }

size_t Value::size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

const Value& Value::operator[](size_t i) const {
    expect(type_ == Type::Array && i < items_.size(), "bad array index");
    return items_[i];
}

const std::vector<Value>& Value::items() const {
    expect(type_ == Type::Array, "not an array");
    return items_;
}

bool Value::has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const auto& [k, v] : members_)
        if (k == key) return true;
    return false;
}

const Value& Value::operator[](std::string_view key) const {
    expect(type_ == Type::Object, "not an object");
    for (const auto& [k, v] : members_)
        if (k == key) return v;
    return kNull;
}

const std::vector<std::pair<std::string, Value>>& Value::members() const {
    expect(type_ == Type::Object, "not an object");
    return members_;
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value document() {
        Value v = value();
        ws();
        expect(i_ == s_.size(), "trailing data");
        return v;
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }

    char peek() {
        ws();
        expect(i_ < s_.size(), "unexpected end");
        return s_[i_];
    }

    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    Value value() {
        Value v;
        char c = peek();
        if (c == '{') {
            v.type_ = Value::Type::Object;
            ++i_;
            if (peek() == '}') { ++i_; return v; }
            for (;;) {
                expect(peek() == '"', "expected key");
                std::string key = string();
                expect(peek() == ':', "expected ':'");
                ++i_;
                v.members_.emplace_back(std::move(key), value());
                c = peek();
                ++i_;
                if (c == '}') break;
                expect(c == ',', "expected ',' in object");
            }
        } else if (c == '[') {
            v.type_ = Value::Type::Array;
            ++i_;
            if (peek() == ']') { ++i_; return v; }
            for (;;) {
                v.items_.push_back(value());
                c = peek();
                ++i_;
                if (c == ']') break;
                expect(c == ',', "expected ',' in array");
            }
        } else if (c == '"') {
            v.type_ = Value::Type::String;
            v.text_ = string();
        } else if (literal("null")) {
        } else if (literal("true")) {
            v.type_ = Value::Type::Bool;
            v.bool_ = true;
        } else if (literal("false")) {
            v.type_ = Value::Type::Bool;
        } else {
            size_t start = i_;
            while (i_ < s_.size() && (std::isdigit(uint8_t(s_[i_])) || std::string_view("+-.eE").find(s_[i_]) != std::string_view::npos))
                ++i_;
            expect(i_ > start, "unexpected character");
            v.type_ = Value::Type::Number;
            v.text_ = std::string(s_.substr(start, i_ - start));
        }
        return v;
    }

    std::string string() {
        ++i_;  // opening quote
        std::string out;
        for (;;) {
            expect(i_ < s_.size(), "unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            expect(i_ < s_.size(), "bad escape");
            char e = s_[i_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    expect(i_ + 4 <= s_.size(), "bad \\u escape");
                    unsigned cp = 0;
                    std::from_chars(s_.data() + i_, s_.data() + i_ + 4, cp, 16);
                    i_ += 4;
                    // UTF-8 encode (BMP only; surrogates are not expected in our files)
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
    }
};

Value parse(std::string_view text) { return Parser(text).document(); }

Value parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

Bytes from_hex(std::string_view hex) {
    expect(hex.size() % 2 == 0, "odd hex length");
    Bytes out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto r = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, out[i], 16);
        expect(r.ec == std::errc(), "bad hex digit");
    }
    return out;
}

}  // namespace ql::json
