#include "z80/cpu.hpp"

#include <utility>

namespace z80 {
namespace {

constexpr Byte B8(unsigned v) { return static_cast<Byte>(v & 0xFF); }
constexpr Word W16(unsigned v) { return static_cast<Word>(v & 0xFFFF); }

constexpr std::array<Byte, 256> makeParityTable() {
    std::array<Byte, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned bits = 0;
        for (unsigned b = i; b != 0; b >>= 1) bits += b & 1;
        table[i] = (bits % 2 == 0) ? flag::PV : Byte{0};
    }
    return table;
}
constexpr auto kParity = makeParityTable();

/// S, Z, and the undocumented bits 3 and 5 derived from a result byte.
constexpr Byte szxy(Byte v) {
    return B8((v & (flag::S | flag::X | flag::Y)) | (v == 0 ? flag::Z : 0));
}

}  // namespace

const char* toString(StopReason reason) {
    switch (reason) {
        case StopReason::Stepped: return "stepped";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Address: return "address";
        case StopReason::Returned: return "returned";
        case StopReason::Halted: return "halted";
        case StopReason::InstructionLimit: return "instruction_limit";
        case StopReason::CycleLimit: return "cycle_limit";
        case StopReason::Watchpoint: return "watchpoint";
    }
    return "unknown";
}

void Cpu::reset() {
    state_ = CpuState{};
    state_.regs.main[7] = 0xFF;  // A
    state_.regs.main[6] = 0xFF;  // F
    state_.regs.sp = 0xFFFF;
    lastCall_ = lastReturn_ = false;
    watchHit_.reset();
}

// ---- bus ------------------------------------------------------------------------------

Byte Cpu::rd(Word address) {
    if (!watch_.empty()) {
        if (auto it = watch_.find(address); it != watch_.end() && it->second != WatchKind::Write) {
            watchHit_ = address;
            watchHitWrite_ = false;
        }
    }
    return memory_->read(address);
}

void Cpu::wr(Word address, Byte value) {
    if (!watch_.empty()) {
        if (auto it = watch_.find(address); it != watch_.end() && it->second != WatchKind::Read) {
            watchHit_ = address;
            watchHitWrite_ = true;
        }
    }
    memory_->write(address, value);
}

Word Cpu::rd16(Word address) {
    const Byte lo = rd(address);
    const Byte hi = rd(W16(address + 1u));
    return W16(lo | (hi << 8));
}

void Cpu::wr16(Word address, Word value) {
    wr(address, B8(value));
    wr(W16(address + 1u), B8(value >> 8));
}

Word Cpu::fetch16() {
    const Byte lo = fetch8();
    const Byte hi = fetch8();
    return W16(lo | (hi << 8));
}

Byte Cpu::fetchOpcode() {
    Registers& r = state_.regs;
    r.r = B8((r.r & 0x80) | ((r.r + 1) & 0x7F));
    return fetch8();
}

void Cpu::push(Word value) {
    Registers& r = state_.regs;
    r.sp = W16(r.sp - 1u);
    wr(r.sp, B8(value >> 8));
    r.sp = W16(r.sp - 1u);
    wr(r.sp, B8(value));
}

Word Cpu::pop() {
    Registers& r = state_.regs;
    const Byte lo = rd(r.sp);
    r.sp = W16(r.sp + 1u);
    const Byte hi = rd(r.sp);
    r.sp = W16(r.sp + 1u);
    return W16(lo | (hi << 8));
}

// ---- register helpers -----------------------------------------------------------------

Byte Cpu::reg(int index, int mode) const {
    const Registers& r = state_.regs;
    const auto i = static_cast<std::size_t>(index);
    if (mode != 0 && (index == 4 || index == 5)) {
        const Word v = mode == 1 ? r.ix : r.iy;
        return index == 4 ? B8(v >> 8) : B8(v);
    }
    return r.main[i];
}

void Cpu::setReg(int index, Byte value, int mode) {
    Registers& r = state_.regs;
    if (mode != 0 && (index == 4 || index == 5)) {
        Word& v = mode == 1 ? r.ix : r.iy;
        v = index == 4 ? W16((v & 0x00FF) | (value << 8)) : W16((v & 0xFF00) | value);
        return;
    }
    r.main[static_cast<std::size_t>(index)] = value;
}

Word Cpu::pairGet(int p, int mode) const {
    const Registers& r = state_.regs;
    switch (p) {
        case 0: return r.bc();
        case 1: return r.de();
        case 2: return mode == 0 ? r.hl() : (mode == 1 ? r.ix : r.iy);
        default: return r.sp;
    }
}

void Cpu::pairSet(int p, int mode, Word value) {
    Registers& r = state_.regs;
    switch (p) {
        case 0: r.setBc(value); break;
        case 1: r.setDe(value); break;
        case 2:
            if (mode == 0) r.setHl(value);
            else if (mode == 1) r.ix = value;
            else r.iy = value;
            break;
        default: r.sp = value; break;
    }
}

Word& Cpu::indexReg(int mode) { return mode == 1 ? state_.regs.ix : state_.regs.iy; }

Word Cpu::indexAddress(int mode) {
    const auto d = static_cast<std::int8_t>(fetch8());
    const Word address = W16(static_cast<unsigned>(indexReg(mode)) + static_cast<unsigned>(static_cast<int>(d)));
    state_.regs.wz = address;
    return address;
}

bool Cpu::condition(int cc) const {
    const Byte f = state_.regs.f();
    switch (cc) {
        case 0: return !(f & flag::Z);
        case 1: return (f & flag::Z) != 0;
        case 2: return !(f & flag::C);
        case 3: return (f & flag::C) != 0;
        case 4: return !(f & flag::PV);
        case 5: return (f & flag::PV) != 0;
        case 6: return !(f & flag::S);
        default: return (f & flag::S) != 0;
    }
}

// ---- arithmetic -----------------------------------------------------------------------

void Cpu::alu(int op, Byte v) {
    Byte& a = state_.regs.main[7];
    const Byte carryIn = (flags() & flag::C) ? 1 : 0;
    switch (op) {
        case 0:
        case 1: {  // ADD, ADC
            const unsigned c = (op == 1) ? carryIn : 0u;
            const unsigned res = static_cast<unsigned>(a) + v + c;
            const Byte r8 = B8(res);
            flags() = B8(szxy(r8) | (((a ^ v ^ res) & 0x10) ? flag::H : 0) |
                         (((a ^ res) & (v ^ res) & 0x80) ? flag::PV : 0) | (res > 0xFF ? flag::C : 0));
            a = r8;
            break;
        }
        case 2:
        case 3:
        case 7: {  // SUB, SBC, CP
            const int c = (op == 3) ? carryIn : 0;
            const int res = static_cast<int>(a) - v - c;
            const Byte r8 = B8(static_cast<unsigned>(res));
            Byte f = B8(szxy(r8) | flag::N | (((a ^ v ^ res) & 0x10) ? flag::H : 0) |
                        (((a ^ v) & (a ^ res) & 0x80) ? flag::PV : 0) | ((res & 0x100) ? flag::C : 0));
            if (op == 7) {
                f = B8((f & ~(flag::X | flag::Y)) | (v & (flag::X | flag::Y)));
            } else {
                a = r8;
            }
            flags() = f;
            break;
        }
        case 4:
            a = B8(a & v);
            flags() = B8(szxy(a) | flag::H | kParity[a]);
            break;
        case 5:
            a = B8(a ^ v);
            flags() = B8(szxy(a) | kParity[a]);
            break;
        default:
            a = B8(a | v);
            flags() = B8(szxy(a) | kParity[a]);
            break;
    }
}

Byte Cpu::inc8(Byte v) {
    const Byte res = B8(v + 1u);
    flags() = B8((flags() & flag::C) | szxy(res) | ((res & 0x0F) == 0 ? flag::H : 0) |
                 (res == 0x80 ? flag::PV : 0));
    return res;
}

Byte Cpu::dec8(Byte v) {
    const Byte res = B8(v - 1u);
    flags() = B8((flags() & flag::C) | szxy(res) | flag::N | ((v & 0x0F) == 0 ? flag::H : 0) |
                 (v == 0x80 ? flag::PV : 0));
    return res;
}

Byte Cpu::rotate(int y, Byte v) {
    const bool oldCarry = (flags() & flag::C) != 0;
    unsigned res = 0;
    unsigned carry = 0;
    switch (y) {
        case 0: carry = v >> 7; res = (v << 1) | carry; break;                      // RLC
        case 1: carry = v & 1; res = (v >> 1) | (carry << 7); break;                // RRC
        case 2: carry = v >> 7; res = (v << 1) | (oldCarry ? 1u : 0u); break;       // RL
        case 3: carry = v & 1; res = (v >> 1) | (oldCarry ? 0x80u : 0u); break;     // RR
        case 4: carry = v >> 7; res = v << 1; break;                                // SLA
        case 5: carry = v & 1; res = (v >> 1) | (v & 0x80); break;                  // SRA
        case 6: carry = v >> 7; res = (v << 1) | 1u; break;                         // SLL
        default: carry = v & 1; res = v >> 1; break;                                // SRL
    }
    const Byte r8 = B8(res);
    flags() = B8(szxy(r8) | kParity[r8] | carry);
    return r8;
}

void Cpu::daa() {
    Byte& a = state_.regs.main[7];
    const Byte f = flags();
    const Byte original = a;
    unsigned correction = 0;
    bool carry = (f & flag::C) != 0;
    if ((f & flag::H) || (original & 0x0F) > 9) correction |= 0x06;
    if (carry || original > 0x99) {
        correction |= 0x60;
        carry = true;
    }
    bool half;
    if (f & flag::N) {
        a = B8(original - correction);
        half = (f & flag::H) && (original & 0x0F) < 6;
    } else {
        a = B8(original + correction);
        half = (original & 0x0F) > 9;
    }
    flags() = B8(szxy(a) | kParity[a] | (f & flag::N) | (half ? flag::H : 0) | (carry ? flag::C : 0));
}

Word Cpu::add16(Word a, Word b) {
    const unsigned res = static_cast<unsigned>(a) + b;
    flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) |
                 (((a & 0xFFF) + (b & 0xFFF)) > 0xFFF ? flag::H : 0) | (res > 0xFFFF ? flag::C : 0) |
                 ((res >> 8) & (flag::X | flag::Y)));
    state_.regs.wz = W16(a + 1u);
    return W16(res);
}

void Cpu::adc16(Word v) {
    Registers& r = state_.regs;
    const Word hl = r.hl();
    const unsigned c = (flags() & flag::C) ? 1u : 0u;
    const unsigned res = static_cast<unsigned>(hl) + v + c;
    const Word r16 = W16(res);
    flags() = B8(((r16 >> 8) & (flag::S | flag::X | flag::Y)) | (r16 == 0 ? flag::Z : 0) |
                 (((hl ^ v ^ res) & 0x1000) ? flag::H : 0) |
                 (((hl ^ res) & (v ^ res) & 0x8000) ? flag::PV : 0) | (res > 0xFFFF ? flag::C : 0));
    r.wz = W16(hl + 1u);
    r.setHl(r16);
}

void Cpu::sbc16(Word v) {
    Registers& r = state_.regs;
    const Word hl = r.hl();
    const int c = (flags() & flag::C) ? 1 : 0;
    const int res = static_cast<int>(hl) - v - c;
    const Word r16 = W16(static_cast<unsigned>(res));
    flags() = B8(((r16 >> 8) & (flag::S | flag::X | flag::Y)) | (r16 == 0 ? flag::Z : 0) | flag::N |
                 (((hl ^ v ^ res) & 0x1000) ? flag::H : 0) |
                 (((hl ^ v) & (hl ^ res) & 0x8000) ? flag::PV : 0) | ((res & 0x10000) ? flag::C : 0));
    r.wz = W16(hl + 1u);
    r.setHl(r16);
}

void Cpu::ioBlockFlags(Byte value, unsigned k) {
    const Byte b = state_.regs.main[0];
    flags() = B8(szxy(b) | ((value & 0x80) ? flag::N : 0) | (k > 0xFF ? (flag::H | flag::C) : 0) |
                 kParity[B8((k & 7) ^ b)]);
}

// ---- instruction groups ---------------------------------------------------------------

unsigned Cpu::execMain(Byte op, int mode) {
    Registers& r = state_.regs;
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;
    Byte& a = r.main[7];

    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    switch (y) {
                        case 0: return 4;
                        case 1:
                            std::swap(r.main[6], r.alt[6]);
                            std::swap(r.main[7], r.alt[7]);
                            return 4;
                        case 2: {
                            const auto d = static_cast<std::int8_t>(fetch8());
                            r.main[0] = B8(r.main[0] - 1u);
                            if (r.main[0] != 0) {
                                r.pc = W16(r.pc + static_cast<unsigned>(static_cast<int>(d)));
                                r.wz = r.pc;
                                return 13;
                            }
                            return 8;
                        }
                        case 3: {
                            const auto d = static_cast<std::int8_t>(fetch8());
                            r.pc = W16(r.pc + static_cast<unsigned>(static_cast<int>(d)));
                            r.wz = r.pc;
                            return 12;
                        }
                        default: {
                            const auto d = static_cast<std::int8_t>(fetch8());
                            if (condition(y - 4)) {
                                r.pc = W16(r.pc + static_cast<unsigned>(static_cast<int>(d)));
                                r.wz = r.pc;
                                return 12;
                            }
                            return 7;
                        }
                    }
                case 1:
                    if (q == 0) {
                        pairSet(p, mode, fetch16());
                        return 10;
                    }
                    pairSet(2, mode, add16(pairGet(2, mode), pairGet(p, mode)));
                    return 11;
                case 2:
                    if (p < 2) {
                        const Word addr = p == 0 ? r.bc() : r.de();
                        if (q == 0) {
                            wr(addr, a);
                            r.wz = W16((a << 8) | ((addr + 1u) & 0xFF));
                        } else {
                            a = rd(addr);
                            r.wz = W16(addr + 1u);
                        }
                        return 7;
                    } else if (p == 2) {
                        const Word nn = fetch16();
                        if (q == 0) wr16(nn, pairGet(2, mode));
                        else pairSet(2, mode, rd16(nn));
                        r.wz = W16(nn + 1u);
                        return 16;
                    } else {
                        const Word nn = fetch16();
                        if (q == 0) {
                            wr(nn, a);
                            r.wz = W16((a << 8) | ((nn + 1u) & 0xFF));
                        } else {
                            a = rd(nn);
                            r.wz = W16(nn + 1u);
                        }
                        return 13;
                    }
                case 3:
                    pairSet(p, mode, W16(pairGet(p, mode) + (q == 0 ? 1u : 0xFFFFu)));
                    return 6;
                case 4:
                case 5: {
                    const bool dec = z == 5;
                    if (y == 6) {
                        const Word addr = mode ? indexAddress(mode) : r.hl();
                        const Byte v = rd(addr);
                        wr(addr, dec ? dec8(v) : inc8(v));
                        return mode ? 19u : 11u;
                    }
                    const Byte v = reg(y, mode);
                    setReg(y, dec ? dec8(v) : inc8(v), mode);
                    return 4;
                }
                case 6:
                    if (y == 6) {
                        if (mode) {
                            const Word addr = indexAddress(mode);
                            wr(addr, fetch8());
                            return 15;
                        }
                        wr(r.hl(), fetch8());
                        return 10;
                    }
                    setReg(y, fetch8(), mode);
                    return 7;
                default:
                    switch (y) {
                        case 0: {  // RLCA
                            const Byte c = a >> 7;
                            a = B8((a << 1) | c);
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | (a & (flag::X | flag::Y)) | c);
                            return 4;
                        }
                        case 1: {  // RRCA
                            const Byte c = a & 1;
                            a = B8((a >> 1) | (c << 7));
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | (a & (flag::X | flag::Y)) | c);
                            return 4;
                        }
                        case 2: {  // RLA
                            const Byte c = a >> 7;
                            a = B8((a << 1) | (flags() & flag::C));
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | (a & (flag::X | flag::Y)) | c);
                            return 4;
                        }
                        case 3: {  // RRA
                            const Byte c = a & 1;
                            a = B8((a >> 1) | ((flags() & flag::C) << 7));
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | (a & (flag::X | flag::Y)) | c);
                            return 4;
                        }
                        case 4: daa(); return 4;
                        case 5:  // CPL
                            a = B8(~a);
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV | flag::C)) | flag::H | flag::N |
                                         (a & (flag::X | flag::Y)));
                            return 4;
                        case 6:  // SCF
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | flag::C | (a & (flag::X | flag::Y)));
                            return 4;
                        default: {  // CCF
                            const bool oldCarry = (flags() & flag::C) != 0;
                            flags() = B8((flags() & (flag::S | flag::Z | flag::PV)) | (oldCarry ? flag::H : flag::C) |
                                         (a & (flag::X | flag::Y)));
                            return 4;
                        }
                    }
            }
        case 1:
            if (y == 6 && z == 6) {
                r.halted = true;
                return 4;
            }
            if (z == 6) {
                const Word addr = mode ? indexAddress(mode) : r.hl();
                r.main[static_cast<std::size_t>(y)] = rd(addr);
                return mode ? 15u : 7u;
            }
            if (y == 6) {
                const Word addr = mode ? indexAddress(mode) : r.hl();
                wr(addr, r.main[static_cast<std::size_t>(z)]);
                return mode ? 15u : 7u;
            }
            setReg(y, reg(z, mode), mode);
            return 4;
        case 2:
            if (z == 6) {
                const Word addr = mode ? indexAddress(mode) : r.hl();
                alu(y, rd(addr));
                return mode ? 15u : 7u;
            }
            alu(y, reg(z, mode));
            return 4;
        default:
            break;
    }

    // x == 3
    switch (z) {
        case 0:
            if (condition(y)) {
                r.pc = pop();
                r.wz = r.pc;
                lastReturn_ = true;
                return 11;
            }
            return 5;
        case 1:
            if (q == 0) {
                const Word v = pop();
                if (p == 3) r.setAf(v);
                else pairSet(p, mode, v);
                return 10;
            }
            switch (p) {
                case 0:
                    r.pc = pop();
                    r.wz = r.pc;
                    lastReturn_ = true;
                    return 10;
                case 1:
                    for (std::size_t i = 0; i < 6; ++i) std::swap(r.main[i], r.alt[i]);
                    return 4;
                case 2: r.pc = pairGet(2, mode); return 4;
                default: r.sp = pairGet(2, mode); return 6;
            }
        case 2: {
            const Word nn = fetch16();
            r.wz = nn;
            if (condition(y)) r.pc = nn;
            return 10;
        }
        case 3:
            switch (y) {
                case 0: {
                    const Word nn = fetch16();
                    r.pc = nn;
                    r.wz = nn;
                    return 10;
                }
                case 2: {  // OUT (n),A
                    const Byte n = fetch8();
                    const Word port = W16((a << 8) | n);
                    if (portOut_) portOut_(port, a);
                    r.wz = W16((a << 8) | ((n + 1u) & 0xFF));
                    return 11;
                }
                case 3: {  // IN A,(n)
                    const Byte n = fetch8();
                    const Word port = W16((a << 8) | n);
                    r.wz = W16(port + 1u);
                    a = portIn_ ? portIn_(port) : Byte{0xFF};
                    return 11;
                }
                case 4: {  // EX (SP),HL
                    const Word v = rd16(r.sp);
                    wr16(r.sp, pairGet(2, mode));
                    pairSet(2, mode, v);
                    r.wz = v;
                    return 19;
                }
                case 5: {  // EX DE,HL
                    std::swap(r.main[2], r.main[4]);
                    std::swap(r.main[3], r.main[5]);
                    return 4;
                }
                case 6:
                    r.iff1 = r.iff2 = false;
                    return 4;
                default:
                    r.iff1 = r.iff2 = true;
                    state_.eiDelay = true;
                    return 4;
            }
        case 4: {
            const Word nn = fetch16();
            r.wz = nn;
            if (condition(y)) {
                push(r.pc);
                r.pc = nn;
                lastCall_ = true;
                return 17;
            }
            return 10;
        }
        case 5:
            if (q == 0) {
                push(p == 3 ? r.af() : pairGet(p, mode));
                return 11;
            } else {
                const Word nn = fetch16();
                r.wz = nn;
                push(r.pc);
                r.pc = nn;
                lastCall_ = true;
                return 17;
            }
        case 6:
            alu(y, fetch8());
            return 7;
        default:
            push(r.pc);
            r.pc = W16(static_cast<unsigned>(y) * 8u);
            r.wz = r.pc;
            lastCall_ = true;
            return 11;
    }
}

unsigned Cpu::execCb() {
    Registers& r = state_.regs;
    const Byte op = fetchOpcode();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;

    auto bitFlags = [&](Byte v, Byte xy) {
        const Byte res = B8(v & (1u << y));
        Byte f = B8((flags() & flag::C) | flag::H | (xy & (flag::X | flag::Y)));
        if (res == 0) f |= flag::Z | flag::PV;
        if (y == 7 && res) f |= flag::S;
        flags() = f;
    };
    auto apply = [&](Byte v) -> Byte {
        if (x == 0) return rotate(y, v);
        if (x == 2) return B8(v & ~(1u << y));
        return B8(v | (1u << y));
    };

    if (z == 6) {
        const Word addr = r.hl();
        const Byte v = rd(addr);
        if (x == 1) {
            bitFlags(v, B8(r.wz >> 8));
            return 12;
        }
        wr(addr, apply(v));
        return 15;
    }
    const Byte v = r.main[static_cast<std::size_t>(z)];
    if (x == 1) {
        bitFlags(v, v);
        return 8;
    }
    r.main[static_cast<std::size_t>(z)] = apply(v);
    return 8;
}

unsigned Cpu::execIndexedCb(int mode) {
    Registers& r = state_.regs;
    const Word addr = indexAddress(mode);
    const Byte op = fetch8();  // not an M1 cycle: R is not incremented
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const Byte v = rd(addr);

    if (x == 1) {
        const Byte res = B8(v & (1u << y));
        Byte f = B8((flags() & flag::C) | flag::H | ((addr >> 8) & (flag::X | flag::Y)));
        if (res == 0) f |= flag::Z | flag::PV;
        if (y == 7 && res) f |= flag::S;
        flags() = f;
        return 16;
    }
    Byte res;
    if (x == 0) res = rotate(y, v);
    else if (x == 2) res = B8(v & ~(1u << y));
    else res = B8(v | (1u << y));
    wr(addr, res);
    if (z != 6) r.main[static_cast<std::size_t>(z)] = res;  // undocumented: result also goes to a register
    return 19;
}

unsigned Cpu::execBlock(int y, int z) {
    Registers& r = state_.regs;
    const bool increment = (y & 1) == 0;
    const bool repeat = y >= 6;
    const unsigned step = increment ? 1u : 0xFFFFu;
    Byte& a = r.main[7];

    switch (z) {
        case 0: {  // LDI / LDD / LDIR / LDDR
            const Byte val = rd(r.hl());
            wr(r.de(), val);
            r.setHl(W16(r.hl() + step));
            r.setDe(W16(r.de() + step));
            r.setBc(W16(r.bc() - 1u));
            const Byte n = B8(val + a);
            flags() = B8((flags() & (flag::S | flag::Z | flag::C)) | (n & flag::X) | ((n & 2) ? flag::Y : 0) |
                         (r.bc() != 0 ? flag::PV : 0));
            if (repeat && r.bc() != 0) {
                r.pc = W16(r.pc - 2u);
                r.wz = W16(r.pc + 1u);
                return 21;
            }
            return 16;
        }
        case 1: {  // CPI / CPD / CPIR / CPDR
            const Byte val = rd(r.hl());
            const int res = static_cast<int>(a) - val;
            const Byte r8 = B8(static_cast<unsigned>(res));
            const bool half = ((a ^ val ^ r8) & 0x10) != 0;
            r.setHl(W16(r.hl() + step));
            r.setBc(W16(r.bc() - 1u));
            r.wz = W16(r.wz + step);
            const Byte n = B8(r8 - (half ? 1u : 0u));
            flags() = B8((flags() & flag::C) | flag::N | (r8 & flag::S) | (r8 == 0 ? flag::Z : 0) |
                         (half ? flag::H : 0) | (r.bc() != 0 ? flag::PV : 0) | (n & flag::X) |
                         ((n & 2) ? flag::Y : 0));
            if (repeat && r.bc() != 0 && r8 != 0) {
                r.pc = W16(r.pc - 2u);
                r.wz = W16(r.pc + 1u);
                return 21;
            }
            return 16;
        }
        case 2: {  // INI / IND / INIR / INDR
            const Word bc = r.bc();
            const Byte val = portIn_ ? portIn_(bc) : Byte{0xFF};
            wr(r.hl(), val);
            r.wz = W16(bc + step);
            r.main[0] = B8(r.main[0] - 1u);
            r.setHl(W16(r.hl() + step));
            const unsigned k = val + B8(r.main[1] + (increment ? 1u : 0xFFu));
            ioBlockFlags(val, k);
            if (repeat && r.main[0] != 0) {
                r.pc = W16(r.pc - 2u);
                return 21;
            }
            return 16;
        }
        default: {  // OUTI / OUTD / OTIR / OTDR
            const Byte val = rd(r.hl());
            r.main[0] = B8(r.main[0] - 1u);
            const Word bc = r.bc();
            if (portOut_) portOut_(bc, val);
            r.wz = W16(bc + step);
            r.setHl(W16(r.hl() + step));
            const unsigned k = val + r.main[5];
            ioBlockFlags(val, k);
            if (repeat && r.main[0] != 0) {
                r.pc = W16(r.pc - 2u);
                return 21;
            }
            return 16;
        }
    }
}

unsigned Cpu::execEd() {
    Registers& r = state_.regs;
    const Byte op = fetchOpcode();
    const int x = op >> 6;
    const int y = (op >> 3) & 7;
    const int z = op & 7;
    const int p = y >> 1;
    const int q = y & 1;
    Byte& a = r.main[7];

    if (x == 2 && z < 4 && y >= 4) return execBlock(y, z);
    if (x != 1) return 8;  // undefined ED opcodes act as two-byte NOPs

    switch (z) {
        case 0: {  // IN r,(C)
            const Word port = r.bc();
            const Byte v = portIn_ ? portIn_(port) : Byte{0xFF};
            if (y != 6) r.main[static_cast<std::size_t>(y)] = v;
            flags() = B8((flags() & flag::C) | szxy(v) | kParity[v]);
            r.wz = W16(port + 1u);
            return 12;
        }
        case 1: {  // OUT (C),r
            const Word port = r.bc();
            if (portOut_) portOut_(port, y == 6 ? Byte{0} : r.main[static_cast<std::size_t>(y)]);
            r.wz = W16(port + 1u);
            return 12;
        }
        case 2:
            if (q == 0) sbc16(pairGet(p, 0));
            else adc16(pairGet(p, 0));
            return 15;
        case 3: {
            const Word nn = fetch16();
            if (q == 0) wr16(nn, pairGet(p, 0));
            else pairSet(p, 0, rd16(nn));
            r.wz = W16(nn + 1u);
            return 20;
        }
        case 4: {  // NEG
            const Byte v = a;
            a = 0;
            alu(2, v);
            return 8;
        }
        case 5:  // RETN / RETI
            r.pc = pop();
            r.wz = r.pc;
            r.iff1 = r.iff2;
            lastReturn_ = true;
            return 14;
        case 6: {
            static constexpr std::array<Byte, 8> kModes{0, 0, 1, 2, 0, 0, 1, 2};
            r.im = kModes[static_cast<std::size_t>(y)];
            return 8;
        }
        default:
            switch (y) {
                case 0: r.i = a; return 9;
                case 1: r.r = a; return 9;
                case 2:
                    a = r.i;
                    flags() = B8((flags() & flag::C) | szxy(a) | (r.iff2 ? flag::PV : 0));
                    return 9;
                case 3:
                    a = r.r;
                    flags() = B8((flags() & flag::C) | szxy(a) | (r.iff2 ? flag::PV : 0));
                    return 9;
                case 4: {  // RRD
                    const Byte v = rd(r.hl());
                    wr(r.hl(), B8((a << 4) | (v >> 4)));
                    a = B8((a & 0xF0) | (v & 0x0F));
                    flags() = B8((flags() & flag::C) | szxy(a) | kParity[a]);
                    r.wz = W16(r.hl() + 1u);
                    return 18;
                }
                case 5: {  // RLD
                    const Byte v = rd(r.hl());
                    wr(r.hl(), B8((v << 4) | (a & 0x0F)));
                    a = B8((a & 0xF0) | (v >> 4));
                    flags() = B8((flags() & flag::C) | szxy(a) | kParity[a]);
                    r.wz = W16(r.hl() + 1u);
                    return 18;
                }
                default: return 8;
            }
    }
}

unsigned Cpu::executeInstruction() {
    unsigned cycles = 0;
    int mode = 0;
    Byte op;
    for (;;) {
        op = fetchOpcode();
        if (op == 0xDD) mode = 1;
        else if (op == 0xFD) mode = 2;
        else break;
        cycles += 4;
    }
    if (op == 0xED) return cycles + execEd();
    if (op == 0xCB) return cycles + (mode ? execIndexedCb(mode) : execCb());
    return cycles + execMain(op, mode);
}

unsigned Cpu::acceptNmi() {
    Registers& r = state_.regs;
    state_.nmiPending = false;
    r.halted = false;
    r.iff2 = r.iff1;
    r.iff1 = false;
    r.r = B8((r.r & 0x80) | ((r.r + 1) & 0x7F));
    push(r.pc);
    r.pc = 0x0066;
    r.wz = r.pc;
    return 11;
}

unsigned Cpu::acceptInterrupt() {
    Registers& r = state_.regs;
    state_.irqPending = false;
    r.halted = false;
    r.iff1 = r.iff2 = false;
    r.r = B8((r.r & 0x80) | ((r.r + 1) & 0x7F));
    switch (r.im) {
        case 2: {
            push(r.pc);
            r.pc = rd16(W16((r.i << 8) | state_.irqData));
            r.wz = r.pc;
            return 19;
        }
        case 1:
            push(r.pc);
            r.pc = 0x0038;
            r.wz = r.pc;
            return 13;
        default: {  // IM 0: the device supplies an instruction; only RST is supported
            const Byte op = (state_.irqData & 0xC7) == 0xC7 ? state_.irqData : Byte{0xFF};
            push(r.pc);
            r.pc = W16(op & 0x38u);
            r.wz = r.pc;
            return 13;
        }
    }
}

unsigned Cpu::step() {
    lastCall_ = lastReturn_ = false;
    const bool blocked = state_.eiDelay;
    state_.eiDelay = false;

    unsigned cycles;
    if (state_.nmiPending) {
        cycles = acceptNmi();
    } else if (state_.irqPending && state_.regs.iff1 && !blocked) {
        cycles = acceptInterrupt();
    } else if (state_.regs.halted) {
        Registers& r = state_.regs;
        r.r = B8((r.r & 0x80) | ((r.r + 1) & 0x7F));
        cycles = 4;
    } else {
        cycles = executeInstruction();
    }
    state_.cycles += cycles;
    ++state_.instructions;
    return cycles;
}

// ---- running --------------------------------------------------------------------------

RunResult Cpu::execute(const RunSpec& spec) {
    Registers& r = state_.regs;
    const std::uint64_t startInstructions = state_.instructions;
    const std::uint64_t startCycles = state_.cycles;
    const Word startSp = r.sp;
    RunResult result;
    bool first = true;
    bool overActive = false;
    Word overReturn = 0;
    Word overSp = 0;
    watchHit_.reset();

    auto finish = [&](StopReason reason) {
        result.reason = reason;
        result.instructions = state_.instructions - startInstructions;
        result.cycles = state_.cycles - startCycles;
        result.pc = r.pc;
        return result;
    };

    for (;;) {
        if (!first) {
            if (overActive && r.pc == overReturn && r.sp == overSp) return finish(StopReason::Stepped);
            if (breakpoints_.count(r.pc) != 0) return finish(StopReason::Breakpoint);
            if (spec.untilPc && r.pc == *spec.untilPc) return finish(StopReason::Address);
        }
        if (r.halted && !state_.nmiPending && !(state_.irqPending && r.iff1)) {
            return finish(StopReason::Halted);
        }
        if (spec.limits.maxInstructions != 0 &&
            state_.instructions - startInstructions >= spec.limits.maxInstructions) {
            return finish(StopReason::InstructionLimit);
        }
        if (spec.limits.maxCycles != 0 && state_.cycles - startCycles >= spec.limits.maxCycles) {
            return finish(StopReason::CycleLimit);
        }

        if (trace_) trace_(*this);
        step();

        if (watchHit_) {
            result.watchAddress = watchHit_;
            result.watchWrite = watchHitWrite_;
            return finish(StopReason::Watchpoint);
        }
        if (spec.single) return finish(StopReason::Stepped);
        if (spec.untilReturn && lastReturn_ && r.sp > startSp) return finish(StopReason::Returned);
        if (spec.stepOver && first) {
            if (!lastCall_) return finish(StopReason::Stepped);
            overActive = true;
            overReturn = memory_->read16(r.sp);
            overSp = W16(r.sp + 2u);
        }
        first = false;
    }
}

RunResult Cpu::stepInstruction() {
    RunSpec spec;
    spec.single = true;
    return execute(spec);
}

RunResult Cpu::run(const RunLimits& limits) {
    RunSpec spec;
    spec.limits = limits;
    return execute(spec);
}

RunResult Cpu::runUntil(Word address, const RunLimits& limits) {
    RunSpec spec;
    spec.limits = limits;
    spec.untilPc = address;
    return execute(spec);
}

RunResult Cpu::runUntilReturn(const RunLimits& limits) {
    RunSpec spec;
    spec.limits = limits;
    spec.untilReturn = true;
    return execute(spec);
}

RunResult Cpu::stepOver(const RunLimits& limits) {
    RunSpec spec;
    spec.limits = limits;
    spec.stepOver = true;
    return execute(spec);
}

}  // namespace z80
