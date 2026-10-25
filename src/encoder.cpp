#include "z80/encoder.hpp"

#include <string>
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>

namespace z80 {
namespace {

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

constexpr std::array<const char*, 8> kConditions{"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};

int conditionIndex(const std::string& text) {
    const std::string u = upper(text);
    for (int i = 0; i < 8; ++i) {
        if (u == kConditions[i]) return i;
    }
    return -1;
}

const std::map<std::string, std::vector<Byte>>& simpleInstructions() {
    static const std::map<std::string, std::vector<Byte>> table{
        {"NOP", {0x00}},        {"HALT", {0x76}},       {"DI", {0xF3}},         {"EI", {0xFB}},
        {"EXX", {0xD9}},        {"CCF", {0x3F}},        {"SCF", {0x37}},        {"CPL", {0x2F}},
        {"DAA", {0x27}},        {"RLCA", {0x07}},       {"RRCA", {0x0F}},       {"RLA", {0x17}},
        {"RRA", {0x1F}},        {"NEG", {0xED, 0x44}},  {"RETN", {0xED, 0x45}}, {"RETI", {0xED, 0x4D}},
        {"RRD", {0xED, 0x67}},  {"RLD", {0xED, 0x6F}},  {"LDI", {0xED, 0xA0}},  {"LDD", {0xED, 0xA8}},
        {"LDIR", {0xED, 0xB0}}, {"LDDR", {0xED, 0xB8}}, {"CPI", {0xED, 0xA1}},  {"CPD", {0xED, 0xA9}},
        {"CPIR", {0xED, 0xB1}}, {"CPDR", {0xED, 0xB9}}, {"INI", {0xED, 0xA2}},  {"IND", {0xED, 0xAA}},
        {"INIR", {0xED, 0xB2}}, {"INDR", {0xED, 0xBA}}, {"OUTI", {0xED, 0xA3}}, {"OUTD", {0xED, 0xAB}},
        {"OTIR", {0xED, 0xB3}}, {"OTDR", {0xED, 0xBB}},
    };
    return table;
}

const std::map<std::string, int>& aluOps() {
    static const std::map<std::string, int> table{{"ADD", 0}, {"ADC", 1}, {"SUB", 2}, {"SBC", 3},
                                                  {"AND", 4}, {"XOR", 5}, {"OR", 6},  {"CP", 7}};
    return table;
}

const std::map<std::string, int>& rotateOps() {
    static const std::map<std::string, int> table{{"RLC", 0}, {"RRC", 1}, {"RL", 2}, {"RR", 3},
                                                  {"SLA", 4}, {"SRA", 5}, {"SRL", 7}};
    return table;
}

const std::set<std::string>& otherMnemonics() {
    static const std::set<std::string> names{"LD",  "PUSH", "POP", "EX", "INC", "DEC", "BIT", "RES",
                                             "SET", "JP",   "JR",  "DJNZ", "CALL", "RET", "RST",
                                             "IN",  "OUT",  "IM"};
    return names;
}

/// Parsed operand.
struct Op {
    enum class Kind {
        None,
        Reg8,     // B C D E H L A           (r = 0..7, never 6)
        Reg16,    // BC DE HL SP AF IX IY    (r = 0..6 in that order)
        AfShadow,  // AF'
        RegI,
        RegR,
        IndHL,   // (HL)
        IndBC,   // (BC)
        IndDE,   // (DE)
        IndSP,   // (SP)
        IndC,    // (C)
        Indexed,  // (IX+d) / (IY+d)
        Memory,   // (expression)
        Immediate
    };
    Kind kind = Kind::None;
    int r = 0;
    bool iy = false;
    std::string text;  // trimmed original text
    std::string expr;  // Immediate/Memory: expression; Indexed: displacement expression
};

constexpr int kBC = 0, kDE = 1, kHL = 2, kSP = 3, kAF = 4, kIX = 5, kIY = 6;

std::size_t matchingParen(const std::string& t) {
    int depth = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '(') ++depth;
        if (t[i] == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

Op parseOperand(const std::string& raw) {
    Op op;
    op.text = trim(raw);
    if (op.text.empty()) return op;
    const std::string u = upper(op.text);

    static const std::map<std::string, int> reg8{{"B", 0}, {"C", 1}, {"D", 2}, {"E", 3},
                                                 {"H", 4}, {"L", 5}, {"A", 7}};
    static const std::map<std::string, int> reg16{{"BC", kBC}, {"DE", kDE}, {"HL", kHL}, {"SP", kSP},
                                                  {"AF", kAF}, {"IX", kIX}, {"IY", kIY}};
    if (auto it = reg8.find(u); it != reg8.end()) {
        op.kind = Op::Kind::Reg8;
        op.r = it->second;
        return op;
    }
    if (auto it = reg16.find(u); it != reg16.end()) {
        op.kind = Op::Kind::Reg16;
        op.r = it->second;
        return op;
    }
    if (u == "AF'") { op.kind = Op::Kind::AfShadow; return op; }
    if (u == "I") { op.kind = Op::Kind::RegI; return op; }
    if (u == "R") { op.kind = Op::Kind::RegR; return op; }

    if (op.text.front() == '(' && matchingParen(op.text) == op.text.size() - 1) {
        const std::string inner = trim(op.text.substr(1, op.text.size() - 2));
        const std::string iu = upper(inner);
        if (iu == "HL") { op.kind = Op::Kind::IndHL; return op; }
        if (iu == "BC") { op.kind = Op::Kind::IndBC; return op; }
        if (iu == "DE") { op.kind = Op::Kind::IndDE; return op; }
        if (iu == "SP") { op.kind = Op::Kind::IndSP; return op; }
        if (iu == "C") { op.kind = Op::Kind::IndC; return op; }
        if (iu.size() >= 2 && (iu.compare(0, 2, "IX") == 0 || iu.compare(0, 2, "IY") == 0)) {
            const std::string rest = trim(inner.substr(2));
            if (rest.empty() || rest[0] == '+' || rest[0] == '-') {
                op.kind = Op::Kind::Indexed;
                op.iy = iu[1] == 'Y';
                op.expr = rest.empty() ? "0" : rest;
                return op;
            }
        }
        op.kind = Op::Kind::Memory;
        op.expr = inner;
        return op;
    }
    op.kind = Op::Kind::Immediate;
    op.expr = op.text;
    return op;
}

class Encoder {
public:
    Encoder(Word pc, const ExpressionCallback& eval) : pc_(pc), eval_(eval) {}

    EncodeResult run(const std::string& mn, const std::vector<std::string>& rawOps) {
        ops_.clear();
        for (const auto& o : rawOps) ops_.push_back(parseOperand(o));
        for (const auto& o : ops_) {
            if (o.kind == Op::Kind::None) return fail("missing operand");
        }
        if (!encode(mn) && error_.empty()) {
            error_ = "invalid operands for " + mn;
        }
        if (!error_.empty() && fatal_) out_.clear();
        return {out_, error_};
    }

private:
    using K = Op::Kind;

    EncodeResult fail(const std::string& message) {
        return {{}, message};
    }

    // ---- emit helpers -----------------------------------------------------------
    void b(unsigned v) { out_.push_back(static_cast<Byte>(v & 0xFF)); }
    void w(unsigned v) {
        b(v);
        b(v >> 8);
    }
    void prefix(const Op& o) { b(o.iy ? 0xFD : 0xDD); }
    void prefixReg(int reg16) { b(reg16 == kIY ? 0xFD : 0xDD); }

    std::int64_t value(const std::string& expr) {
        const ExprResult r = eval_(expr);
        if (!r.ok) softError(r.error);
        return r.value;
    }
    void softError(const std::string& message) {
        if (error_.empty()) error_ = message;
    }
    void imm8(const Op& o) {
        const std::int64_t v = value(o.expr);
        if (v < -128 || v > 255) softError("value out of 8-bit range: " + std::to_string(v));
        b(static_cast<unsigned>(v));
    }
    void imm16(const std::string& expr) {
        const std::int64_t v = value(expr);
        if (v < -32768 || v > 65535) softError("value out of 16-bit range: " + std::to_string(v));
        w(static_cast<unsigned>(v));
    }
    void displacement(const Op& o) {
        const std::int64_t v = value(o.expr);
        if (v < -128 || v > 127) softError("index displacement out of range: " + std::to_string(v));
        b(static_cast<unsigned>(v));
    }
    void relative(const Op& o) {
        const std::int64_t target = value(o.expr);
        const std::int64_t offset = target - (static_cast<std::int64_t>(pc_) + 2);
        if (offset < -128 || offset > 127) {
            softError("relative jump out of range (" + std::to_string(offset) + ")");
        }
        b(static_cast<unsigned>(offset));
    }

    static bool is16(const Op& o, int reg) { return o.kind == K::Reg16 && o.r == reg; }
    static bool isIndexReg(const Op& o) { return o.kind == K::Reg16 && (o.r == kIX || o.r == kIY); }
    static bool isA(const Op& o) { return o.kind == K::Reg8 && o.r == 7; }

    /// Register number 0..7 of an 8-bit operand (6 = (HL)); -1 if not a plain register operand.
    static int rm(const Op& o) {
        if (o.kind == K::Reg8) return o.r;
        if (o.kind == K::IndHL) return 6;
        return -1;
    }

    // ---- instruction groups -----------------------------------------------------
    bool encode(const std::string& mn) {
        if (auto it = simpleInstructions().find(mn); it != simpleInstructions().end()) {
            if (!ops_.empty()) {
                error_ = mn + " takes no operands";
                fatal_ = true;
                return true;
            }
            out_ = it->second;
            return true;
        }
        if (mn == "LD") return encodeLd();
        if (mn == "PUSH" || mn == "POP") return encodeStack(mn == "PUSH");
        if (mn == "EX") return encodeEx();
        if (auto it = aluOps().find(mn); it != aluOps().end()) return encodeAlu(mn, it->second);
        if (mn == "INC" || mn == "DEC") return encodeIncDec(mn == "DEC");
        if (auto it = rotateOps().find(mn); it != rotateOps().end()) return encodeRotate(it->second);
        if (mn == "BIT" || mn == "RES" || mn == "SET") return encodeBit(mn);
        if (mn == "JP") return encodeJp();
        if (mn == "JR") return encodeJr();
        if (mn == "DJNZ") return encodeDjnz();
        if (mn == "CALL") return encodeCall();
        if (mn == "RET") return encodeRet();
        if (mn == "RST") return encodeRst();
        if (mn == "IN") return encodeIn();
        if (mn == "OUT") return encodeOut();
        if (mn == "IM") return encodeIm();
        error_ = "unknown mnemonic " + mn;
        fatal_ = true;
        return true;
    }

    bool encodeLd() {
        if (ops_.size() != 2) return false;
        const Op& d = ops_[0];
        const Op& s = ops_[1];

        // 8-bit register destination
        if (d.kind == K::Reg8) {
            if (s.kind == K::Reg8 || s.kind == K::IndHL) {
                b(0x40 | (d.r << 3) | rm(s));
                return true;
            }
            if (s.kind == K::Indexed) {
                prefix(s);
                b(0x46 | (d.r << 3));
                displacement(s);
                return true;
            }
            if (isA(d)) {
                if (s.kind == K::IndBC) { b(0x0A); return true; }
                if (s.kind == K::IndDE) { b(0x1A); return true; }
                if (s.kind == K::Memory) { b(0x3A); imm16(s.expr); return true; }
                if (s.kind == K::RegI) { b(0xED); b(0x57); return true; }
                if (s.kind == K::RegR) { b(0xED); b(0x5F); return true; }
            }
            if (s.kind == K::Immediate) {
                b(0x06 | (d.r << 3));
                imm8(s);
                return true;
            }
            return false;
        }
        if (d.kind == K::IndHL) {
            if (s.kind == K::Reg8) { b(0x70 | s.r); return true; }
            if (s.kind == K::Immediate) { b(0x36); imm8(s); return true; }
            return false;
        }
        if (d.kind == K::Indexed) {
            if (s.kind == K::Reg8) {
                prefix(d);
                b(0x70 | s.r);
                displacement(d);
                return true;
            }
            if (s.kind == K::Immediate) {
                prefix(d);
                b(0x36);
                displacement(d);
                imm8(s);
                return true;
            }
            return false;
        }
        if (d.kind == K::IndBC && isA(s)) { b(0x02); return true; }
        if (d.kind == K::IndDE && isA(s)) { b(0x12); return true; }
        if (d.kind == K::RegI && isA(s)) { b(0xED); b(0x47); return true; }
        if (d.kind == K::RegR && isA(s)) { b(0xED); b(0x4F); return true; }
        if (d.kind == K::Memory) {
            if (isA(s)) { b(0x32); imm16(d.expr); return true; }
            if (is16(s, kHL)) { b(0x22); imm16(d.expr); return true; }
            if (isIndexReg(s)) { prefixReg(s.r); b(0x22); imm16(d.expr); return true; }
            if (s.kind == K::Reg16 && (s.r == kBC || s.r == kDE || s.r == kSP)) {
                b(0xED);
                b(0x43 | (s.r << 4));
                imm16(d.expr);
                return true;
            }
            return false;
        }
        if (d.kind == K::Reg16) {
            if (d.r == kSP && is16(s, kHL)) { b(0xF9); return true; }
            if (d.r == kSP && isIndexReg(s)) { prefixReg(s.r); b(0xF9); return true; }
            if (d.r == kAF) return false;
            const bool index = isIndexReg(d);
            if (s.kind == K::Immediate) {
                if (index) {
                    prefixReg(d.r);
                    b(0x21);
                } else {
                    b(0x01 | (d.r << 4));
                }
                imm16(s.expr);
                return true;
            }
            if (s.kind == K::Memory) {
                if (index) {
                    prefixReg(d.r);
                    b(0x2A);
                } else if (d.r == kHL) {
                    b(0x2A);
                } else {
                    b(0xED);
                    b(0x4B | (d.r << 4));
                }
                imm16(s.expr);
                return true;
            }
        }
        return false;
    }

    bool encodeStack(bool push) {
        if (ops_.size() != 1 || ops_[0].kind != K::Reg16) return false;
        const int r = ops_[0].r;
        if (r == kSP) return false;
        if (r == kIX || r == kIY) {
            prefixReg(r);
            b(push ? 0xE5 : 0xE1);
            return true;
        }
        b((push ? 0xC5 : 0xC1) | ((r == kAF ? 3 : r) << 4));
        return true;
    }

    bool encodeEx() {
        if (ops_.size() != 2) return false;
        const Op& a = ops_[0];
        const Op& c = ops_[1];
        if (is16(a, kAF) && c.kind == K::AfShadow) { b(0x08); return true; }
        if (is16(a, kDE) && is16(c, kHL)) { b(0xEB); return true; }
        if (a.kind == K::IndSP && is16(c, kHL)) { b(0xE3); return true; }
        if (a.kind == K::IndSP && isIndexReg(c)) { prefixReg(c.r); b(0xE3); return true; }
        return false;
    }

    bool encodeAlu(const std::string& mn, int op) {
        if (ops_.empty() || ops_.size() > 2) return false;

        // 16-bit arithmetic: ADD HL/IX/IY,rr  ADC HL,rr  SBC HL,rr
        if (ops_.size() == 2 && ops_[0].kind == K::Reg16) {
            const Op& d = ops_[0];
            const Op& s = ops_[1];
            if (s.kind != K::Reg16) return false;
            if (mn == "ADD") {
                if (d.r == kHL) {
                    if (s.r > kSP) return false;
                    b(0x09 | (s.r << 4));
                    return true;
                }
                if (isIndexReg(d)) {
                    int code;
                    if (s.r == kBC || s.r == kDE || s.r == kSP) code = s.r;
                    else if (s.r == d.r) code = kHL;
                    else return false;
                    prefixReg(d.r);
                    b(0x09 | (code << 4));
                    return true;
                }
                return false;
            }
            if (d.r != kHL || s.r > kSP) return false;
            if (mn == "ADC") { b(0xED); b(0x4A | (s.r << 4)); return true; }
            if (mn == "SBC") { b(0xED); b(0x42 | (s.r << 4)); return true; }
            return false;
        }

        // 8-bit: "OP A,src" or "OP src"
        if (ops_.size() == 2 && !isA(ops_[0])) return false;
        const Op& s = ops_.back();
        if (s.kind == K::Reg8 || s.kind == K::IndHL) {
            b(0x80 | (op << 3) | rm(s));
            return true;
        }
        if (s.kind == K::Indexed) {
            prefix(s);
            b(0x86 | (op << 3));
            displacement(s);
            return true;
        }
        if (s.kind == K::Immediate) {
            b(0xC6 | (op << 3));
            imm8(s);
            return true;
        }
        return false;
    }

    bool encodeIncDec(bool dec) {
        if (ops_.size() != 1) return false;
        const Op& o = ops_[0];
        if (o.kind == K::Reg8 || o.kind == K::IndHL) {
            b((dec ? 0x05 : 0x04) | (rm(o) << 3));
            return true;
        }
        if (o.kind == K::Indexed) {
            prefix(o);
            b(dec ? 0x35 : 0x34);
            displacement(o);
            return true;
        }
        if (o.kind == K::Reg16) {
            if (o.r <= kSP) { b((dec ? 0x0B : 0x03) | (o.r << 4)); return true; }
            if (isIndexReg(o)) { prefixReg(o.r); b(dec ? 0x2B : 0x23); return true; }
        }
        return false;
    }

    bool encodeRotate(int op) {
        if (ops_.size() != 1) return false;
        const Op& o = ops_[0];
        if (o.kind == K::Reg8 || o.kind == K::IndHL) {
            b(0xCB);
            b((op << 3) | rm(o));
            return true;
        }
        if (o.kind == K::Indexed) {
            prefix(o);
            b(0xCB);
            displacement(o);
            b((op << 3) | 6);
            return true;
        }
        return false;
    }

    bool encodeBit(const std::string& mn) {
        if (ops_.size() != 2) return false;
        const Op& bitOp = ops_[0];
        const Op& o = ops_[1];
        if (bitOp.kind != K::Immediate) return false;
        const std::int64_t bit = value(bitOp.expr);
        if (bit < 0 || bit > 7) {
            softError("bit number out of range (0-7)");
        }
        const int base = mn == "BIT" ? 0x40 : (mn == "RES" ? 0x80 : 0xC0);
        const int code = base | ((static_cast<int>(bit) & 7) << 3);
        if (o.kind == K::Reg8 || o.kind == K::IndHL) {
            b(0xCB);
            b(code | rm(o));
            return true;
        }
        if (o.kind == K::Indexed) {
            prefix(o);
            b(0xCB);
            displacement(o);
            b(code | 6);
            return true;
        }
        return false;
    }

    bool encodeJp() {
        if (ops_.size() == 1) {
            const Op& t = ops_[0];
            if (t.kind == K::IndHL) { b(0xE9); return true; }
            if (t.kind == K::Indexed) {
                if (value(t.expr) != 0) {
                    softError("JP (IX) takes no displacement");
                }
                prefix(t);
                b(0xE9);
                return true;
            }
            if (t.kind == K::Immediate) { b(0xC3); imm16(t.expr); return true; }
            return false;
        }
        if (ops_.size() == 2) {
            const int cc = conditionIndex(ops_[0].text);
            if (cc < 0 || ops_[1].kind != K::Immediate) return false;
            b(0xC2 | (cc << 3));
            imm16(ops_[1].expr);
            return true;
        }
        return false;
    }

    bool encodeJr() {
        if (ops_.size() == 1 && ops_[0].kind == K::Immediate) {
            b(0x18);
            relative(ops_[0]);
            return true;
        }
        if (ops_.size() == 2 && ops_[1].kind == K::Immediate) {
            const int cc = conditionIndex(ops_[0].text);
            if (cc < 0 || cc > 3) return false;
            b(0x20 | (cc << 3));
            relative(ops_[1]);
            return true;
        }
        return false;
    }

    bool encodeDjnz() {
        if (ops_.size() != 1 || ops_[0].kind != K::Immediate) return false;
        b(0x10);
        relative(ops_[0]);
        return true;
    }

    bool encodeCall() {
        if (ops_.size() == 1 && ops_[0].kind == K::Immediate) {
            b(0xCD);
            imm16(ops_[0].expr);
            return true;
        }
        if (ops_.size() == 2 && ops_[1].kind == K::Immediate) {
            const int cc = conditionIndex(ops_[0].text);
            if (cc < 0) return false;
            b(0xC4 | (cc << 3));
            imm16(ops_[1].expr);
            return true;
        }
        return false;
    }

    bool encodeRet() {
        if (ops_.empty()) { b(0xC9); return true; }
        if (ops_.size() == 1) {
            const int cc = conditionIndex(ops_[0].text);
            if (cc < 0) return false;
            b(0xC0 | (cc << 3));
            return true;
        }
        return false;
    }

    bool encodeRst() {
        if (ops_.size() != 1 || ops_[0].kind != K::Immediate) return false;
        const std::int64_t v = value(ops_[0].expr);
        if (v < 0 || v > 0x38 || (v & 7) != 0) {
            softError("RST address must be 0, 8, 16, ..., 56 (&38)");
        }
        b(0xC7 | (static_cast<int>(v) & 0x38));
        return true;
    }

    bool encodeIn() {
        if (ops_.size() != 2) return false;
        const Op& d = ops_[0];
        const Op& s = ops_[1];
        if (isA(d) && s.kind == K::Memory) {
            b(0xDB);
            Op n;
            n.expr = s.expr;
            imm8(n);
            return true;
        }
        if (d.kind == K::Reg8 && s.kind == K::IndC) {
            b(0xED);
            b(0x40 | (d.r << 3));
            return true;
        }
        return false;
    }

    bool encodeOut() {
        if (ops_.size() != 2) return false;
        const Op& d = ops_[0];
        const Op& s = ops_[1];
        if (d.kind == K::Memory && isA(s)) {
            b(0xD3);
            Op n;
            n.expr = d.expr;
            imm8(n);
            return true;
        }
        if (d.kind == K::IndC && s.kind == K::Reg8) {
            b(0xED);
            b(0x41 | (s.r << 3));
            return true;
        }
        return false;
    }

    bool encodeIm() {
        if (ops_.size() != 1 || ops_[0].kind != K::Immediate) return false;
        const std::int64_t v = value(ops_[0].expr);
        if (v < 0 || v > 2) {
            softError("interrupt mode must be 0, 1 or 2");
        }
        static constexpr std::array<Byte, 3> kCodes{0x46, 0x56, 0x5E};
        b(0xED);
        b(kCodes[static_cast<std::size_t>(v < 0 || v > 2 ? 0 : v)]);
        return true;
    }

    Word pc_;
    const ExpressionCallback& eval_;
    std::vector<Op> ops_;
    std::vector<Byte> out_;
    std::string error_;
    bool fatal_ = false;
};

}  // namespace

bool isMnemonic(std::string_view name) {
    const std::string u = upper(name);
    return simpleInstructions().count(u) != 0 || aluOps().count(u) != 0 || rotateOps().count(u) != 0 ||
           otherMnemonics().count(u) != 0;
}

bool isReservedName(std::string_view name) {
    static const std::set<std::string> reserved{"A",  "B",  "C",  "D",  "E",  "H",  "L",  "I",
                                                "R",  "AF", "BC", "DE", "HL", "SP", "IX", "IY",
                                                "NZ", "Z",  "NC", "PO", "PE", "P",  "M"};
    const std::string u = upper(name);
    return reserved.count(u) != 0 || isMnemonic(u);
}

std::vector<std::string> splitOperands(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    char prev = 0;
    for (char c : text) {
        if (quote != 0) {
            current += c;
            if (c == quote) quote = 0;
        } else if (c == '"' || (c == '\'' && !std::isalnum(static_cast<unsigned char>(prev)))) {
            quote = c;
            current += c;
        } else if (c == '(') {
            ++depth;
            current += c;
        } else if (c == ')') {
            --depth;
            current += c;
        } else if (c == ',' && depth == 0) {
            parts.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
        prev = c;
    }
    if (!trim(current).empty() || !parts.empty()) parts.push_back(trim(current));
    return parts;
}

EncodeResult encodeInstruction(std::string_view mnemonic, const std::vector<std::string>& operands,
                               Word pc, const ExpressionCallback& eval) {
    return Encoder(pc, eval).run(upper(mnemonic), operands);
}

}  // namespace z80
