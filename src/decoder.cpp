#include "z80/decoder.hpp"

#include <array>
#include <cstdlib>

namespace z80 {
namespace {

constexpr std::array<const char*, 8> kReg8{"B", "C", "D", "E", "H", "L", "(HL)", "A"};
constexpr std::array<const char*, 4> kReg16{"BC", "DE", "HL", "SP"};
constexpr std::array<const char*, 4> kReg16Stack{"BC", "DE", "HL", "AF"};
constexpr std::array<const char*, 8> kCondition{"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
constexpr std::array<const char*, 8> kAlu{"ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"};
constexpr std::array<const char*, 8> kRotate{"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
constexpr std::array<const char*, 8> kAccumulatorOps{"RLCA", "RRCA", "RLA", "RRA",
                                                      "DAA",  "CPL",  "SCF", "CCF"};
constexpr std::array<std::array<const char*, 4>, 4> kBlockOps{{
    {"LDI", "CPI", "INI", "OUTI"},
    {"LDD", "CPD", "IND", "OUTD"},
    {"LDIR", "CPIR", "INIR", "OTIR"},
    {"LDDR", "CPDR", "INDR", "OTDR"},
}};

/// Opcodes (after a DD/FD prefix) that are affected by the prefix in documented Z80.
bool isIndexable(Byte op, Byte opAfterDisplacement) {
    switch (op) {
        case 0x09: case 0x19: case 0x29: case 0x39:  // ADD IX,rr
        case 0x21: case 0x22: case 0x2A:             // LD IX,nn / LD (nn),IX / LD IX,(nn)
        case 0x23: case 0x2B:                        // INC/DEC IX
        case 0x34: case 0x35: case 0x36:             // INC/DEC (IX+d), LD (IX+d),n
        case 0xE1: case 0xE3: case 0xE5:             // POP IX, EX (SP),IX, PUSH IX
        case 0xE9: case 0xF9:                        // JP (IX), LD SP,IX
            return true;
        case 0xCB: {
            const int z = opAfterDisplacement & 7;
            const int x = opAfterDisplacement >> 6;
            const int y = (opAfterDisplacement >> 3) & 7;
            return z == 6 && !(x == 0 && y == 6);  // only the (IX+d) forms, no SLL
        }
        default:
            break;
    }
    if ((op & 0xC0) == 0x40) {  // LD r,(IX+d) / LD (IX+d),r  (but not HALT)
        return (((op >> 3) & 7) == 6) != ((op & 7) == 6);
    }
    return (op & 0xC7) == 0x86;  // ALU A,(IX+d)
}

class Decoder {
public:
    Decoder(const Memory& memory, Word address, const DecodeOptions& options)
        : memory_(memory), start_(address), pc_(address), options_(options) {}

    Instruction run() {
        ins_.address = start_;
        Byte op = next();

        if (op == 0xDD || op == 0xFD) {
            const Byte following = memory_.read(pc_);
            const Byte afterDisp = memory_.read(static_cast<Word>(pc_ + 2));
            if (!isIndexable(following, afterDisp)) {
                // DD CB d op with an undocumented op still consumes four bytes.
                return invalid(following == 0xCB ? 4 : 1);
            }
            index_ = (op == 0xDD) ? "IX" : "IY";
            op = next();
            if (op == 0xCB) {
                return decodeIndexedCb();
            }
        } else if (op == 0xCB) {
            return decodeCb();
        } else if (op == 0xED) {
            return decodeEd();
        }
        decodeMain(op);
        return ins_;
    }

private:
    // ---- byte stream ------------------------------------------------------------
    Byte next() {
        const Byte b = memory_.read(pc_);
        if (ins_.length < ins_.bytes.size()) {
            ins_.bytes[ins_.length] = b;
        }
        ++ins_.length;
        ++pc_;
        return b;
    }

    // ---- formatting helpers -----------------------------------------------------
    std::string hex8(unsigned v) const { return formatHex(v & 0xFF, 2, options_.style); }
    std::string hex16(unsigned v) const { return formatHex(v & 0xFFFF, 4, options_.style); }
    std::string imm8() { return hex8(next()); }
    std::string imm16() {
        const unsigned lo = next();
        const unsigned hi = next();
        return hex16(lo | (hi << 8));
    }

    std::string targetName(Word target) const {
        if (options_.symbolizer) {
            if (auto name = options_.symbolizer(target)) {
                return *name;
            }
        }
        return hex16(target);
    }

    /// "(IX+&05)" — reads the displacement byte from the stream.
    std::string indexedOperand() {
        const auto d = static_cast<std::int8_t>(next());
        const int magnitude = d < 0 ? -static_cast<int>(d) : d;
        return std::string("(") + index_ + (d < 0 ? "-" : "+") + hex8(static_cast<unsigned>(magnitude)) +
               ")";
    }

    std::string reg8(int index) {
        if (index == 6) {
            return index_ ? indexedOperand() : "(HL)";
        }
        return kReg8[index];
    }
    std::string hlName() const { return index_ ? index_ : "HL"; }
    std::string reg16(int index) const { return index == 2 ? hlName() : kReg16[index]; }
    std::string reg16Stack(int index) const { return index == 2 ? hlName() : kReg16Stack[index]; }

    // ---- result helpers ---------------------------------------------------------
    void set(std::string mnemonic, std::string operands = {}, FlowKind flow = FlowKind::Sequential) {
        ins_.mnemonic = std::move(mnemonic);
        ins_.operands = std::move(operands);
        ins_.flow = flow;
    }

    void setBranch(const std::string& mnemonic, const std::string& condition, Word target,
                   FlowKind flow) {
        ins_.target = target;
        std::string operands = condition.empty() ? "" : condition + ",";
        operands += targetName(target);
        set(mnemonic, operands, flow);
    }

    /// Re-reads `count` bytes from the start and emits them as DB.
    Instruction invalid(int count) {
        ins_ = Instruction{};
        ins_.address = start_;
        pc_ = start_;
        std::string operands;
        for (int i = 0; i < count; ++i) {
            if (i != 0) operands += ",";
            operands += hex8(next());
        }
        set("DB", operands);
        ins_.valid = false;
        return ins_;
    }

    // ---- decoding ---------------------------------------------------------------
    void decodeMain(Byte op) {
        const int x = op >> 6;
        const int y = (op >> 3) & 7;
        const int z = op & 7;
        const int p = y >> 1;
        const int q = y & 1;

        switch (x) {
            case 0:
                switch (z) {
                    case 0:
                        if (y == 0) {
                            set("NOP");
                        } else if (y == 1) {
                            set("EX", "AF,AF'");
                        } else {
                            const auto d = static_cast<std::int8_t>(next());
                            const auto target = static_cast<Word>(pc_ + d);
                            if (y == 2) {
                                setBranch("DJNZ", "", target, FlowKind::ConditionalJump);
                            } else if (y == 3) {
                                setBranch("JR", "", target, FlowKind::Jump);
                            } else {
                                setBranch("JR", kCondition[y - 4], target, FlowKind::ConditionalJump);
                            }
                        }
                        break;
                    case 1:
                        if (q == 0) {
                            set("LD", reg16(p) + "," + imm16());
                        } else {
                            set("ADD", hlName() + "," + reg16(p));
                        }
                        break;
                    case 2: {
                        static constexpr std::array<const char*, 2> kIndirect{"(BC)", "(DE)"};
                        if (p < 2) {
                            if (q == 0) {
                                set("LD", std::string(kIndirect[p]) + ",A");
                            } else {
                                set("LD", std::string("A,") + kIndirect[p]);
                            }
                        } else {
                            const std::string nn = "(" + imm16() + ")";
                            const std::string reg = (p == 2) ? hlName() : "A";
                            set("LD", q == 0 ? nn + "," + reg : reg + "," + nn);
                        }
                        break;
                    }
                    case 3:
                        set(q == 0 ? "INC" : "DEC", reg16(p));
                        break;
                    case 4:
                        set("INC", reg8(y));
                        break;
                    case 5:
                        set("DEC", reg8(y));
                        break;
                    case 6: {
                        const std::string dst = reg8(y);  // displacement precedes the immediate
                        set("LD", dst + "," + imm8());
                        break;
                    }
                    default:
                        set(kAccumulatorOps[y]);
                        break;
                }
                break;

            case 1:
                if (y == 6 && z == 6) {
                    set("HALT");
                } else {
                    const std::string dst = reg8(y);
                    const std::string src = reg8(z);
                    set("LD", dst + "," + src);
                }
                break;

            case 2:
                setAlu(y, reg8(z));
                break;

            default:
                decodeX3(y, z, p, q);
                break;
        }
    }

    void setAlu(int op, const std::string& operand) {
        const bool needsAccumulator = op == 0 || op == 1 || op == 3;  // ADD, ADC, SBC
        set(kAlu[op], needsAccumulator ? "A," + operand : operand);
    }

    void decodeX3(int y, int z, int p, int q) {
        switch (z) {
            case 0:
                set("RET", kCondition[y], FlowKind::ConditionalReturn);
                break;
            case 1:
                if (q == 0) {
                    set("POP", reg16Stack(p));
                } else if (p == 0) {
                    set("RET", "", FlowKind::Return);
                } else if (p == 1) {
                    set("EXX");
                } else if (p == 2) {
                    set("JP", "(" + hlName() + ")", FlowKind::JumpIndirect);
                } else {
                    set("LD", "SP," + hlName());
                }
                break;
            case 2: {
                const auto target = static_cast<Word>(imm16Raw());
                setBranch("JP", kCondition[y], target, FlowKind::ConditionalJump);
                break;
            }
            case 3:
                switch (y) {
                    case 0: {
                        const auto target = static_cast<Word>(imm16Raw());
                        setBranch("JP", "", target, FlowKind::Jump);
                        break;
                    }
                    case 2:
                        set("OUT", "(" + imm8() + "),A");
                        break;
                    case 3:
                        set("IN", "A,(" + imm8() + ")");
                        break;
                    case 4:
                        set("EX", "(SP)," + hlName());
                        break;
                    case 5:
                        set("EX", "DE,HL");
                        break;
                    case 6:
                        set("DI");
                        break;
                    default:
                        set("EI");
                        break;
                }
                break;
            case 4: {
                const auto target = static_cast<Word>(imm16Raw());
                setBranch("CALL", kCondition[y], target, FlowKind::ConditionalCall);
                break;
            }
            case 5:
                if (q == 0) {
                    set("PUSH", reg16Stack(p));
                } else {  // p == 0: CALL nn (DD/ED/FD are handled before)
                    const auto target = static_cast<Word>(imm16Raw());
                    setBranch("CALL", "", target, FlowKind::Call);
                }
                break;
            case 6:
                setAlu(y, imm8());
                break;
            default:
                ins_.target = static_cast<Word>(y * 8);
                set("RST", hex8(static_cast<unsigned>(y * 8)), FlowKind::Restart);
                break;
        }
    }

    unsigned imm16Raw() {
        const unsigned lo = next();
        const unsigned hi = next();
        return lo | (hi << 8);
    }

    Instruction decodeCb() {
        const Byte op = next();
        const int x = op >> 6;
        const int y = (op >> 3) & 7;
        const int z = op & 7;
        if (x == 0) {
            if (y == 6) {
                return invalid(2);  // SLL is undocumented
            }
            set(kRotate[y], kReg8[z]);
        } else {
            static constexpr std::array<const char*, 3> kBitOps{"BIT", "RES", "SET"};
            set(kBitOps[x - 1], std::to_string(y) + "," + kReg8[z]);
        }
        return ins_;
    }

    Instruction decodeIndexedCb() {
        const std::string operand = indexedOperand();
        const Byte op = next();
        const int x = op >> 6;
        const int y = (op >> 3) & 7;
        if (x == 0) {
            set(kRotate[y], operand);
        } else {
            static constexpr std::array<const char*, 3> kBitOps{"BIT", "RES", "SET"};
            set(kBitOps[x - 1], std::to_string(y) + "," + operand);
        }
        return ins_;
    }

    Instruction decodeEd() {
        const Byte op = next();
        const int x = op >> 6;
        const int y = (op >> 3) & 7;
        const int z = op & 7;
        const int p = y >> 1;
        const int q = y & 1;

        if (x == 2 && z < 4 && y >= 4) {
            set(kBlockOps[y - 4][z]);
            return ins_;
        }
        if (x != 1) {
            return invalid(2);
        }
        switch (z) {
            case 0:
                if (y == 6) return invalid(2);  // IN F,(C) is undocumented
                set("IN", std::string(kReg8[y]) + ",(C)");
                break;
            case 1:
                if (y == 6) return invalid(2);  // OUT (C),0 is undocumented
                set("OUT", std::string("(C),") + kReg8[y]);
                break;
            case 2:
                set(q == 0 ? "SBC" : "ADC", std::string("HL,") + kReg16[p]);
                break;
            case 3: {
                if (p == 2) return invalid(2);  // ED 63 / ED 6B: alias of the short forms
                const std::string nn = "(" + imm16() + ")";
                set("LD", q == 0 ? nn + "," + kReg16[p] : std::string(kReg16[p]) + "," + nn);
                break;
            }
            case 4:
                if (y != 0) return invalid(2);
                set("NEG");
                break;
            case 5:
                if (y > 1) return invalid(2);
                set(y == 0 ? "RETN" : "RETI", "", FlowKind::Return);
                break;
            case 6:
                if (y != 0 && y != 2 && y != 3) return invalid(2);
                set("IM", y == 0 ? "0" : (y == 2 ? "1" : "2"));
                break;
            default: {
                static constexpr std::array<const char*, 6> kNames{"LD", "LD", "LD", "LD", "RRD", "RLD"};
                static constexpr std::array<const char*, 4> kOperands{"I,A", "R,A", "A,I", "A,R"};
                if (y > 5) return invalid(2);
                set(kNames[y], y < 4 ? kOperands[y] : "");
                break;
            }
        }
        return ins_;
    }

    const Memory& memory_;
    Word start_;
    Word pc_;
    const DecodeOptions& options_;
    Instruction ins_;
    const char* index_ = nullptr;  // "IX"/"IY" while decoding a prefixed instruction
};

}  // namespace

Instruction decode(const Memory& memory, Word address, const DecodeOptions& options) {
    return Decoder(memory, address, options).run();
}

}  // namespace z80
