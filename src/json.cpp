#include "z80/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace z80 {

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : members_) {
        if (k == key) return &v;
    }
    return nullptr;
}

Json& Json::set(std::string key, Json value) {
    if (type_ == Type::Null) type_ = Type::Object;
    for (auto& [k, v] : members_) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (type_ == Type::Null) type_ = Type::Array;
    items_.push_back(std::move(value));
    return items_.back();
}

namespace {

void escapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Json parseDocument() {
        Json v = parseValue();
        skip();
        if (i_ != s_.size()) fail("unexpected trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw JsonError("JSON error at position " + std::to_string(i_) + ": " + msg);
    }
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    char peek() {
        skip();
        if (i_ >= s_.size()) fail("unexpected end of input");
        return s_[i_];
    }
    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++i_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) == word) {
            i_ += word.size();
            return true;
        }
        return false;
    }

    Json parseValue() {
        if (++depth_ > 64) fail("nesting too deep");
        Json result;
        const char c = peek();
        if (c == '{') {
            ++i_;
            result = Json::object();
            if (peek() == '}') {
                ++i_;
            } else {
                for (;;) {
                    if (peek() != '"') fail("expected string key");
                    std::string key = parseString();
                    expect(':');
                    result.set(std::move(key), parseValue());
                    if (peek() == ',') { ++i_; continue; }
                    expect('}');
                    break;
                }
            }
        } else if (c == '[') {
            ++i_;
            result = Json::array();
            if (peek() == ']') {
                ++i_;
            } else {
                for (;;) {
                    result.push(parseValue());
                    if (peek() == ',') { ++i_; continue; }
                    expect(']');
                    break;
                }
            }
        } else if (c == '"') {
            result = Json(parseString());
        } else if (literal("true")) {
            result = Json(true);
        } else if (literal("false")) {
            result = Json(false);
        } else if (literal("null")) {
            result = Json();
        } else {
            result = parseNumber();
        }
        --depth_;
        return result;
    }

    Json parseNumber() {
        const std::size_t begin = i_;
        bool isInt = true;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
            if (!std::isdigit(static_cast<unsigned char>(s_[i_]))) isInt = false;
            ++i_;
        }
        const std::string token(s_.substr(begin, i_ - begin));
        if (token.empty() || token == "-" || token == "+") fail("invalid value");
        char* end = nullptr;
        if (isInt) {
            const long long v = std::strtoll(token.c_str(), &end, 10);
            if (*end != '\0') fail("invalid number");
            return Json(static_cast<std::int64_t>(v));
        }
        const double d = std::strtod(token.c_str(), &end);
        if (*end != '\0') fail("invalid number");
        return Json(d);
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("bad escape");
            c = s_[i_++];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '/': case '\\': case '"': out += c; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) fail("bad \\u escape");
                    const unsigned code = static_cast<unsigned>(std::strtoul(std::string(s_.substr(i_, 4)).c_str(), nullptr, 16));
                    i_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
            }
        }
        if (i_ >= s_.size()) fail("unterminated string");
        ++i_;
        return out;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    int depth_ = 0;
};

}  // namespace

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number:
            if (isInt_) {
                out += std::to_string(int_);
            } else if (std::isfinite(num_)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            } else {
                out += "null";
            }
            break;
        case Type::String: escapeTo(out, str_); break;
        case Type::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].dumpTo(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i) out += ',';
                escapeTo(out, members_[i].first);
                out += ':';
                members_[i].second.dumpTo(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

Json Json::parse(std::string_view text) { return Parser(text).parseDocument(); }

}  // namespace z80
