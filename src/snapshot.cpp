#include "z80/snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "z80/expression.hpp"
#include "z80/format.hpp"

namespace z80 {
namespace {

constexpr char kMagic[8] = {'Z', '8', '0', 'E', 'M', 'U', 'S', '1'};

void put16(std::vector<Byte>& out, Word v) {
    out.push_back(static_cast<Byte>(v));
    out.push_back(static_cast<Byte>(v >> 8));
}
void put64(std::vector<Byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<Byte>(v >> (8 * i)));
}

struct Reader {
    std::span<const Byte> data;
    std::size_t pos = 0;
    Byte u8() {
        if (pos >= data.size()) throw std::runtime_error("snapshot is truncated");
        return data[pos++];
    }
    Word u16() {
        const Byte lo = u8();
        const Byte hi = u8();
        return static_cast<Word>(lo | (hi << 8));
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
};

std::string upperCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string hex8(unsigned v) { return formatHex(v & 0xFF, 2, NumberStyle::CPrefix); }
std::string hex16(unsigned v) { return formatHex(v & 0xFFFF, 4, NumberStyle::CPrefix); }

}  // namespace

Snapshot captureSnapshot(const Cpu& cpu) {
    Snapshot s;
    s.state = cpu.state();
    const auto bytes = cpu.memory().bytes();
    s.memory.assign(bytes.begin(), bytes.end());
    return s;
}

void restoreSnapshot(Cpu& cpu, const Snapshot& snapshot) {
    if (snapshot.memory.size() != kAddressSpace) throw std::runtime_error("snapshot has the wrong memory size");
    cpu.setState(snapshot.state);
    cpu.memory().load(0, snapshot.memory);
}

std::vector<Byte> serializeSnapshot(const Snapshot& snapshot) {
    if (snapshot.memory.size() != kAddressSpace) throw std::runtime_error("snapshot has the wrong memory size");
    const CpuState& st = snapshot.state;
    const Registers& r = st.regs;
    std::vector<Byte> out(kMagic, kMagic + sizeof kMagic);
    for (Byte b : r.main) out.push_back(b);
    for (Byte b : r.alt) out.push_back(b);
    for (Word w : {r.ix, r.iy, r.sp, r.pc, r.wz}) put16(out, w);
    out.push_back(r.i);
    out.push_back(r.r);
    out.push_back(r.im);
    out.push_back(static_cast<Byte>((r.iff1 ? 1 : 0) | (r.iff2 ? 2 : 0) | (r.halted ? 4 : 0) | (st.eiDelay ? 8 : 0) |
                                    (st.irqPending ? 16 : 0) | (st.nmiPending ? 32 : 0)));
    out.push_back(st.irqData);
    put64(out, st.cycles);
    put64(out, st.instructions);
    out.insert(out.end(), snapshot.memory.begin(), snapshot.memory.end());
    return out;
}

Snapshot deserializeSnapshot(std::span<const Byte> bytes) {
    if (bytes.size() < sizeof kMagic || !std::equal(kMagic, kMagic + sizeof kMagic, bytes.begin(),
                                                    [](char m, Byte b) { return static_cast<Byte>(m) == b; })) {
        throw std::runtime_error("not a Z80 emulator snapshot (bad magic)");
    }
    Reader in{bytes, sizeof kMagic};
    Snapshot s;
    Registers& r = s.state.regs;
    for (Byte& b : r.main) b = in.u8();
    for (Byte& b : r.alt) b = in.u8();
    r.ix = in.u16();
    r.iy = in.u16();
    r.sp = in.u16();
    r.pc = in.u16();
    r.wz = in.u16();
    r.i = in.u8();
    r.r = in.u8();
    r.im = in.u8();
    if (r.im > 2) throw std::runtime_error("snapshot has an invalid interrupt mode");
    const Byte bits = in.u8();
    r.iff1 = bits & 1;
    r.iff2 = bits & 2;
    r.halted = bits & 4;
    s.state.eiDelay = bits & 8;
    s.state.irqPending = bits & 16;
    s.state.nmiPending = bits & 32;
    s.state.irqData = in.u8();
    s.state.cycles = in.u64();
    s.state.instructions = in.u64();
    if (bytes.size() - in.pos != kAddressSpace) throw std::runtime_error("snapshot has the wrong size");
    s.memory.assign(bytes.begin() + static_cast<std::ptrdiff_t>(in.pos), bytes.end());
    return s;
}

// ---- registers by name ----------------------------------------------------------------

namespace {

struct Slot {
    enum Kind { Main8, Alt8, Pair, AltPair, Word16, Byte8, Bool1 } kind;
    int a = 0;  // index
    int b = 0;  // second index (pairs: low byte)
};

std::optional<Slot> lookup(std::string name) {
    name = upperCase(std::move(name));
    if (name.size() > 1 && name.back() == '\'') name = name.substr(0, name.size() - 1) + "2";
    static const std::pair<const char*, Slot> table[] = {
        {"B", {Slot::Main8, 0}}, {"C", {Slot::Main8, 1}}, {"D", {Slot::Main8, 2}}, {"E", {Slot::Main8, 3}},
        {"H", {Slot::Main8, 4}}, {"L", {Slot::Main8, 5}}, {"F", {Slot::Main8, 6}}, {"A", {Slot::Main8, 7}},
        {"B2", {Slot::Alt8, 0}}, {"C2", {Slot::Alt8, 1}}, {"D2", {Slot::Alt8, 2}}, {"E2", {Slot::Alt8, 3}},
        {"H2", {Slot::Alt8, 4}}, {"L2", {Slot::Alt8, 5}}, {"F2", {Slot::Alt8, 6}}, {"A2", {Slot::Alt8, 7}},
        {"AF", {Slot::Pair, 7, 6}}, {"BC", {Slot::Pair, 0, 1}}, {"DE", {Slot::Pair, 2, 3}}, {"HL", {Slot::Pair, 4, 5}},
        {"AF2", {Slot::AltPair, 7, 6}}, {"BC2", {Slot::AltPair, 0, 1}}, {"DE2", {Slot::AltPair, 2, 3}},
        {"HL2", {Slot::AltPair, 4, 5}},
        {"IX", {Slot::Word16, 0}}, {"IY", {Slot::Word16, 1}}, {"SP", {Slot::Word16, 2}}, {"PC", {Slot::Word16, 3}},
        {"WZ", {Slot::Word16, 4}},
        {"IXH", {Slot::Byte8, 0}}, {"IXL", {Slot::Byte8, 1}}, {"IYH", {Slot::Byte8, 2}}, {"IYL", {Slot::Byte8, 3}},
        {"I", {Slot::Byte8, 4}}, {"R", {Slot::Byte8, 5}}, {"IM", {Slot::Byte8, 6}},
        {"IFF1", {Slot::Bool1, 0}}, {"IFF2", {Slot::Bool1, 1}}, {"HALTED", {Slot::Bool1, 2}},
    };
    for (const auto& [n, slot] : table) {
        if (name == n) return slot;
    }
    return std::nullopt;
}

Word& word16(Registers& r, int which) {
    switch (which) {
        case 0: return r.ix;
        case 1: return r.iy;
        case 2: return r.sp;
        case 3: return r.pc;
        default: return r.wz;
    }
}

}  // namespace

std::optional<std::uint32_t> getRegister(const Registers& regs, std::string name) {
    const auto slot = lookup(std::move(name));
    if (!slot) return std::nullopt;
    Registers& r = const_cast<Registers&>(regs);  // read-only use below
    const auto a = static_cast<std::size_t>(slot->a);
    const auto b = static_cast<std::size_t>(slot->b);
    switch (slot->kind) {
        case Slot::Main8: return regs.main[a];
        case Slot::Alt8: return regs.alt[a];
        case Slot::Pair: return static_cast<std::uint32_t>(regs.main[a] << 8 | regs.main[b]);
        case Slot::AltPair: return static_cast<std::uint32_t>(regs.alt[a] << 8 | regs.alt[b]);
        case Slot::Word16: return word16(r, slot->a);
        case Slot::Byte8:
            switch (slot->a) {
                case 0: return regs.ix >> 8;
                case 1: return regs.ix & 0xFF;
                case 2: return regs.iy >> 8;
                case 3: return regs.iy & 0xFF;
                case 4: return regs.i;
                case 5: return regs.r;
                default: return regs.im;
            }
        case Slot::Bool1:
            return (slot->a == 0 ? regs.iff1 : slot->a == 1 ? regs.iff2 : regs.halted) ? 1u : 0u;
    }
    return std::nullopt;
}

bool setRegister(Registers& regs, std::string name, std::uint32_t value) {
    const auto slot = lookup(std::move(name));
    if (!slot) return false;
    const auto a = static_cast<std::size_t>(slot->a);
    const auto b = static_cast<std::size_t>(slot->b);
    switch (slot->kind) {
        case Slot::Main8:
        case Slot::Alt8:
            if (value > 0xFF) return false;
            (slot->kind == Slot::Main8 ? regs.main : regs.alt)[a] = static_cast<Byte>(value);
            return true;
        case Slot::Pair:
        case Slot::AltPair: {
            if (value > 0xFFFF) return false;
            auto& arr = slot->kind == Slot::Pair ? regs.main : regs.alt;
            arr[a] = static_cast<Byte>(value >> 8);
            arr[b] = static_cast<Byte>(value);
            return true;
        }
        case Slot::Word16:
            if (value > 0xFFFF) return false;
            word16(regs, slot->a) = static_cast<Word>(value);
            return true;
        case Slot::Byte8: {
            if (value > 0xFF) return false;
            const auto v = static_cast<Byte>(value);
            switch (slot->a) {
                case 0: regs.ix = static_cast<Word>((regs.ix & 0x00FF) | (v << 8)); break;
                case 1: regs.ix = static_cast<Word>((regs.ix & 0xFF00) | v); break;
                case 2: regs.iy = static_cast<Word>((regs.iy & 0x00FF) | (v << 8)); break;
                case 3: regs.iy = static_cast<Word>((regs.iy & 0xFF00) | v); break;
                case 4: regs.i = v; break;
                case 5: regs.r = v; break;
                default:
                    if (v > 2) return false;
                    regs.im = v;
            }
            return true;
        }
        case Slot::Bool1:
            if (value > 1) return false;
            (slot->a == 0 ? regs.iff1 : slot->a == 1 ? regs.iff2 : regs.halted) = value != 0;
            return true;
    }
    return false;
}

std::string flagsToString(Byte f) {
    static constexpr char kNames[] = "SZYHXPNC";
    std::string s;
    for (int bit = 7; bit >= 0; --bit) s += (f & (1u << bit)) ? kNames[7 - bit] : '.';
    return s;
}

// ---- JSON -----------------------------------------------------------------------------

Json registersToJson(const Registers& r) {
    Json j = Json::object();
    j.set("AF", hex16(r.af())).set("BC", hex16(r.bc())).set("DE", hex16(r.de())).set("HL", hex16(r.hl()));
    j.set("AF'", hex16(r.af2())).set("BC'", hex16(r.bc2())).set("DE'", hex16(r.de2())).set("HL'", hex16(r.hl2()));
    j.set("IX", hex16(r.ix)).set("IY", hex16(r.iy)).set("SP", hex16(r.sp)).set("PC", hex16(r.pc));
    j.set("A", hex8(r.a())).set("I", hex8(r.i)).set("R", hex8(r.r));
    j.set("flags", flagsToString(r.f()));
    j.set("IM", r.im).set("IFF1", r.iff1).set("IFF2", r.iff2).set("halted", r.halted);
    return j;
}

Json stateToJson(const CpuState& st) {
    const Registers& r = st.regs;
    Json j = Json::object();
    for (const char* name : {"AF", "BC", "DE", "HL", "AF2", "BC2", "DE2", "HL2", "IX", "IY", "SP", "PC", "WZ"}) {
        j.set(name, hex16(*getRegister(r, name)));
    }
    j.set("I", hex8(r.i)).set("R", hex8(r.r)).set("IM", r.im);
    j.set("IFF1", r.iff1).set("IFF2", r.iff2).set("halted", r.halted);
    j.set("eiDelay", st.eiDelay).set("irqPending", st.irqPending).set("irqData", hex8(st.irqData));
    j.set("nmiPending", st.nmiPending);
    j.set("cycles", st.cycles).set("instructions", st.instructions);
    return j;
}

namespace {

std::int64_t numberOf(const Json& v, const std::string& what) {
    if (v.isNumber()) return v.asInt();
    if (v.isBool()) return v.asBool() ? 1 : 0;
    if (v.isString()) {
        if (const auto n = parseConstant(v.asString())) return *n;
    }
    throw JsonError("invalid value for '" + what + "'");
}

}  // namespace

CpuState stateFromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("state must be a JSON object");
    CpuState st;
    st.regs = Registers{};
    for (const auto& [key, value] : j.members()) {
        const std::int64_t n = numberOf(value, key);
        if (key == "cycles") { st.cycles = static_cast<std::uint64_t>(n); continue; }
        if (key == "instructions") { st.instructions = static_cast<std::uint64_t>(n); continue; }
        if (key == "eiDelay") { st.eiDelay = n != 0; continue; }
        if (key == "irqPending") { st.irqPending = n != 0; continue; }
        if (key == "nmiPending") { st.nmiPending = n != 0; continue; }
        if (key == "irqData") { st.irqData = static_cast<Byte>(n); continue; }
        if (n < 0 || !setRegister(st.regs, key == "halted" ? "HALTED" : key, static_cast<std::uint32_t>(n))) {
            // unknown member names are ignored; out-of-range values are errors
            if (lookup(key)) throw JsonError("value out of range for '" + key + "'");
        }
    }
    return st;
}

}  // namespace z80
