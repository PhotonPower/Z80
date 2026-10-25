#include "z80/expression.hpp"

#include <string>
#include <cctype>

namespace z80 {
namespace {

struct ExprError {
    std::string message;
};

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '@' ||
           c == '?';
}
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

std::int64_t digitValue(char c) {
    if (isDigit(c)) return c - '0';
    return std::toupper(static_cast<unsigned char>(c)) - 'A' + 10;
}

std::int64_t parseDigits(std::string_view digits, int base, std::string_view whole) {
    if (digits.empty()) {
        throw ExprError{"invalid number '" + std::string(whole) + "'"};
    }
    std::int64_t value = 0;
    for (char c : digits) {
        if (!isHex(c) || digitValue(c) >= base) {
            throw ExprError{"invalid number '" + std::string(whole) + "'"};
        }
        value = value * base + digitValue(c);
        if (value > 0xFFFFFFFFLL) {
            throw ExprError{"number too large '" + std::string(whole) + "'"};
        }
    }
    return value;
}

bool allHexDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!isHex(c)) return false;
    }
    return true;
}

class Parser {
public:
    Parser(std::string_view text, const ExpressionEvaluator::Lookup& lookup, std::int64_t pc,
           ExprResult& result, bool hexDefault)
        : s_(text), lookup_(lookup), pc_(pc), result_(result), hexDefault_(hexDefault) {}

    std::int64_t parseAll() {
        const std::int64_t v = parseOr();
        skipSpaces();
        if (i_ != s_.size()) {
            throw ExprError{"unexpected '" + std::string(1, s_[i_]) + "' in expression"};
        }
        return v;
    }

private:
    void skipSpaces() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool peekIs(char c) {
        skipSpaces();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool peekIs2(char a, char b) {
        skipSpaces();
        return i_ + 1 < s_.size() && s_[i_] == a && s_[i_ + 1] == b;
    }

    std::int64_t parseOr() {
        std::int64_t v = parseXor();
        while (peekIs('|')) {
            ++i_;
            v |= parseXor();
        }
        return v;
    }
    std::int64_t parseXor() {
        std::int64_t v = parseAnd();
        while (peekIs('^')) {
            ++i_;
            v ^= parseAnd();
        }
        return v;
    }
    std::int64_t parseAnd() {
        std::int64_t v = parseShift();
        while (peekIs('&')) {
            ++i_;
            v &= parseShift();
        }
        return v;
    }
    std::int64_t parseShift() {
        std::int64_t v = parseAdd();
        for (;;) {
            if (peekIs2('<', '<')) {
                i_ += 2;
                v <<= (parseAdd() & 63);
            } else if (peekIs2('>', '>')) {
                i_ += 2;
                v >>= (parseAdd() & 63);
            } else {
                return v;
            }
        }
    }
    std::int64_t parseAdd() {
        std::int64_t v = parseMul();
        for (;;) {
            if (peekIs('+')) {
                ++i_;
                v += parseMul();
            } else if (peekIs('-')) {
                ++i_;
                v -= parseMul();
            } else {
                return v;
            }
        }
    }
    std::int64_t parseMul() {
        std::int64_t v = parseUnary();
        for (;;) {
            if (peekIs('*')) {
                ++i_;
                v *= parseUnary();
            } else if (peekIs('/')) {
                ++i_;
                const std::int64_t d = parseUnary();
                if (d == 0) throw ExprError{"division by zero"};
                v /= d;
            } else if (peekIs('%')) {
                ++i_;
                const std::int64_t d = parseUnary();
                if (d == 0) throw ExprError{"division by zero"};
                v %= d;
            } else {
                return v;
            }
        }
    }
    std::int64_t parseUnary() {
        skipSpaces();
        if (i_ < s_.size()) {
            if (s_[i_] == '-') {
                ++i_;
                return -parseUnary();
            }
            if (s_[i_] == '+') {
                ++i_;
                return parseUnary();
            }
            if (s_[i_] == '~') {
                ++i_;
                return ~parseUnary();
            }
        }
        return parsePrimary();
    }

    std::int64_t parsePrimary() {
        skipSpaces();
        if (i_ >= s_.size()) throw ExprError{"unexpected end of expression"};
        const char c = s_[i_];

        if (c == '(') {
            ++i_;
            const std::int64_t v = parseOr();
            if (!peekIs(')')) throw ExprError{"missing ')'"};
            ++i_;
            return v;
        }
        if (c == '\'') {
            if (i_ + 2 < s_.size() && s_[i_ + 2] == '\'') {
                const auto v = static_cast<unsigned char>(s_[i_ + 1]);
                i_ += 3;
                return v;
            }
            throw ExprError{"invalid character literal"};
        }
        if (c == '$') {
            if (i_ + 1 < s_.size() && isHex(s_[i_ + 1])) {
                ++i_;
                return readRadix(16);
            }
            ++i_;
            return pc_;
        }
        if (c == '&') {
            ++i_;
            if (i_ + 1 < s_.size() && (s_[i_] == 'h' || s_[i_] == 'H') && isHex(s_[i_ + 1])) {
                ++i_;
            }
            if (i_ >= s_.size() || !isHex(s_[i_])) throw ExprError{"invalid hexadecimal number"};
            return readRadix(16);
        }
        if (c == '#') {
            ++i_;
            return readRadix(16);
        }
        if (c == '%') {
            ++i_;
            return readRadix(2);
        }
        if (isDigit(c)) {
            return readNumberToken();
        }
        if (isIdentStart(c)) {
            return readSymbol();
        }
        throw ExprError{"unexpected '" + std::string(1, c) + "' in expression"};
    }

    std::int64_t readRadix(int base) {
        const std::size_t begin = i_;
        while (i_ < s_.size() && isHex(s_[i_])) ++i_;
        return parseDigits(s_.substr(begin, i_ - begin), base, s_.substr(begin - 1, i_ - begin + 1));
    }

    std::int64_t readNumberToken() {
        const std::size_t begin = i_;
        while (i_ < s_.size() && isIdentChar(s_[i_])) ++i_;
        const std::string_view token = s_.substr(begin, i_ - begin);
        std::string u;
        for (char ch : token) u += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        auto allBinary = [](std::string_view v) {
            if (v.empty()) return false;
            for (char ch : v) {
                if (ch != '0' && ch != '1') return false;
            }
            return true;
        };

        if (hexDefault_ && allHexDigits(u)) return parseDigits(u, 16, token);
        if (u.size() > 2 && u[0] == '0' && u[1] == 'X') return parseDigits(u.substr(2), 16, token);
        if (u.back() == 'H') return parseDigits(std::string_view(u).substr(0, u.size() - 1), 16, token);
        if (u.size() > 2 && u[0] == '0' && u[1] == 'B' && allBinary(std::string_view(u).substr(2))) {
            return parseDigits(std::string_view(u).substr(2), 2, token);
        }
        if (u.back() == 'B' && allBinary(std::string_view(u).substr(0, u.size() - 1))) {
            return parseDigits(std::string_view(u).substr(0, u.size() - 1), 2, token);
        }
        return parseDigits(u, 10, token);
    }

    std::int64_t readSymbol() {
        const std::size_t begin = i_;
        while (i_ < s_.size() && isIdentChar(s_[i_])) ++i_;
        std::string name;
        for (char ch : s_.substr(begin, i_ - begin)) {
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if ((name == "HIGH" || name == "LOW") && peekIs('(')) {
            ++i_;
            const std::int64_t v = parseOr();
            if (!peekIs(')')) throw ExprError{"missing ')'"};
            ++i_;
            return name == "HIGH" ? ((v >> 8) & 0xFF) : (v & 0xFF);
        }
        if (lookup_) {
            if (auto v = lookup_(name)) return *v;
        }
        if (hexDefault_ && allHexDigits(name)) return parseDigits(name, 16, name);
        result_.undefinedNames.push_back(name);
        return 0;
    }

    std::string_view s_;
    const ExpressionEvaluator::Lookup& lookup_;
    std::int64_t pc_;
    ExprResult& result_;
    bool hexDefault_;
    std::size_t i_ = 0;
};

}  // namespace

ExprResult ExpressionEvaluator::evaluate(std::string_view text) const {
    ExprResult result;
    try {
        Parser parser(text, lookup_, pc_, result, hexDefault_);
        result.value = parser.parseAll();
    } catch (const ExprError& e) {
        result.ok = false;
        result.error = e.message;
        result.value = 0;
    }
    return result;
}

std::optional<std::int64_t> parseConstant(std::string_view text) {
    const ExprResult r = ExpressionEvaluator{}.evaluate(text);
    if (!r.ok || !r.undefinedNames.empty()) return std::nullopt;
    return r.value;
}

}  // namespace z80
