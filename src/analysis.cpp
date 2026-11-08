#include "z80/analysis.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>

#include "z80/decoder.hpp"
#include "z80/format.hpp"

namespace z80 {
namespace {

bool textChar(Byte b) { return (b >= 0x20 && b < 0x7F) || b == 0x09 || b == 0x0A || b == 0x0D; }
bool letterOrSpace(Byte b) { return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == ' '; }
/// The last character of a string in the CPC/Spectrum convention: a printable character with bit 7 set.
bool bit7End(Byte b) { return b >= 0xA0 && b < 0xFF && textChar(static_cast<Byte>(b & 0x7F)); }

std::string at(Word a) { return " at &" + hexDigits(a, 4); }

/// The instruction that ends right before `a` (inside the analysed code), if there is one.
std::optional<Instruction> previousInstruction(const Memory& memory, const Disassembler& dis, Word a) {
    for (unsigned back = 1; back <= 4; ++back) {
        if (back > a) break;
        const auto p = static_cast<Word>(a - back);
        if (!dis.isInstructionStart(p)) continue;
        Instruction i = decode(memory, p, {});
        if (i.next() == a) return i;
    }
    return std::nullopt;
}

/// The register pair an immediate 16-bit load writes ("HL" for LD HL,nn), or "" if the instruction is something else.
std::string immediateLoadPair(const Instruction& i) {
    if (!i.valid || i.mnemonic != "LD") return "";
    const Byte op = i.bytes[0];
    if (i.length == 3 && (op == 0x01 || op == 0x11 || op == 0x21)) return op == 0x01 ? "BC" : op == 0x11 ? "DE" : "HL";
    if (i.length == 4 && (op == 0xDD || op == 0xFD) && i.bytes[1] == 0x21) return op == 0xDD ? "IX" : "IY";
    return "";
}
Word immediateValue(const Instruction& i) {
    return i.length == 4 ? static_cast<Word>(i.bytes[2] | (i.bytes[3] << 8)) : static_cast<Word>(i.bytes[1] | (i.bytes[2] << 8));
}

/// True if the instruction reads a byte from memory through a register pair: LD r,(HL) / LD A,(DE) / LD r,(IX+d).
bool readsThroughPair(const Instruction& i) {
    if (!i.valid || i.mnemonic != "LD") return false;
    const auto comma = i.operands.find(',');
    if (comma == std::string::npos) return false;
    const std::string src = i.operands.substr(comma + 1);
    return src == "(HL)" || src == "(DE)" || src == "(BC)" || src.rfind("(IX", 0) == 0 || src.rfind("(IY", 0) == 0;
}

/// The register pair (or its halves) the instruction writes as a destination: "LD HL,..." "POP HL" "INC HL" "EX DE,HL" (both) ...
bool writesPair(const Instruction& i, const std::string& pair) {
    const std::string& o = i.operands;
    const std::string hi = pair == "HL" ? "H" : pair == "DE" ? "D" : pair == "BC" ? "B" : "";
    const std::string lo = pair == "HL" ? "L" : pair == "DE" ? "E" : pair == "BC" ? "C" : "";
    auto dest = [&](const std::string& r) { return o == r || o.rfind(r + ",", 0) == 0; };
    if (i.mnemonic == "LD" || i.mnemonic == "POP" || i.mnemonic == "IN") return dest(pair) || (!hi.empty() && (dest(hi) || dest(lo)));
    if (i.mnemonic == "EX" || i.mnemonic == "EXX") return true;
    if (i.mnemonic == "LDIR" || i.mnemonic == "LDDR" || i.mnemonic == "LDI" || i.mnemonic == "LDD") return pair != "IX" && pair != "IY";
    return false;
}

}  // namespace

std::optional<DataOperand> dataOperandAt(const Memory& memory, Word address) {
    std::optional<DataOperand> found;
    DecodeOptions options;
    options.operandSymbolizer = [&found](Word value, bool memoryOperand) -> std::optional<std::string> {
        DataOperand d;
        d.value = value;
        d.memory = memoryOperand;
        found = d;
        return std::nullopt;
    };
    const Instruction i = decode(memory, address, options);
    if (!found || !i.valid) return std::nullopt;
    if (!found->memory && i.length == 3 && i.bytes[0] == 0x31) return std::nullopt;  // LD SP,nn: the top of the stack, not data
    if (found->memory) {
        const std::string& o = i.operands;
        found->size = (o.rfind("A,", 0) == 0 || (o.size() >= 2 && o.compare(o.size() - 2, 2, ",A") == 0)) ? 1 : 2;
    }
    return found;
}

// ---- interrupt handlers ----------------------------------------------------------------------------------------------------------

std::vector<Word> findInterruptHandlers(const Memory& memory, const Disassembler& dis) {
    std::vector<Word> out;
    for (unsigned a = 0; a < kAddressSpace; ++a) {
        if (!dis.isInstructionStart(static_cast<Word>(a))) continue;
        const Instruction store = decode(memory, static_cast<Word>(a), {});
        // LD (&0039),HL  /  LD (&0039),DE / LD (&0039),BC
        const bool viaHl = store.length == 3 && store.bytes[0] == 0x22 && store.bytes[1] == 0x39 && store.bytes[2] == 0x00;
        const bool viaEd = store.length == 4 && store.bytes[0] == 0xED && (store.bytes[1] == 0x53 || store.bytes[1] == 0x43) &&
                           store.bytes[2] == 0x39 && store.bytes[3] == 0x00;
        if (!viaHl && !viaEd) continue;
        const std::string pair = viaHl ? "HL" : store.bytes[1] == 0x53 ? "DE" : "BC";
        Word cur = static_cast<Word>(a);
        for (int k = 0; k < 6; ++k) {
            const auto p = previousInstruction(memory, dis, cur);
            if (!p || p->flow != FlowKind::Sequential) break;
            if (immediateLoadPair(*p) == pair) {
                out.push_back(immediateValue(*p));
                break;
            }
            if (writesPair(*p, pair)) break;
            cur = p->address;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---- jump tables ---------------------------------------------------------------------------------------------------------------

std::vector<JumpTable> findJumpTables(const Memory& memory, const Disassembler& dis, const AnalysisContext& ctx) {
    std::vector<JumpTable> out;
    std::vector<std::uint8_t> taken(kAddressSpace, 0);  // bytes of a table found already
    auto inProgram = [&](unsigned a) { return a >= ctx.first && a <= ctx.last; };
    auto isData = [&](Word a) { return ctx.isData && ctx.isData(a); };
    auto plausibleTarget = [&](Word t) {
        if (!inProgram(t) || isData(t)) return false;
        if (dis.isCovered(t) && !dis.isInstructionStart(t)) return false;
        if (memory.read(t) == 0 && memory.read(static_cast<Word>(t + 1)) == 0 && memory.read(static_cast<Word>(t + 2)) == 0) return false;  // empty memory
        return decode(memory, t, {}).valid;
    };
    auto looksLikeText = [&](Word t) {  // a string decodes as instructions too: four printable characters, three of them letters/digits/blanks
        unsigned alnum = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const Byte b = memory.read(static_cast<Word>(t + i));
            if (b < 0x20 || b >= 0x7F) return false;
            if (std::isalnum(b) || b == ' ') ++alnum;
        }
        return alnum >= 3;
    };

    // data references and branch targets: where one table ends and the next thing begins
    std::set<Word> refs;
    std::set<Word> immediates;
    for (unsigned a = 0; a < kAddressSpace; ++a) {
        if (!dis.isInstructionStart(static_cast<Word>(a))) continue;
        if (auto d = dataOperandAt(memory, static_cast<Word>(a))) {
            refs.insert(d->value);
            if (!d->memory) immediates.insert(d->value);
        }
    }
    for (const auto& [t, c] : dis.callers()) (void)c, refs.insert(t);
    for (const auto& [t, c] : dis.jumpers()) (void)c, refs.insert(t);

    auto claim = [&](const JumpTable& t) {
        for (unsigned i = 0; i < t.bytes && t.table + i < kAddressSpace; ++i) taken[t.table + i] = 1;
    };

    // (1) dispatch code: JP (HL)/(IX)/(IY), or PUSH rr + RET, with a LD rr,table shortly before
    for (unsigned a = 0; a < kAddressSpace; ++a) {
        const auto addr = static_cast<Word>(a);
        if (!dis.isInstructionStart(addr)) continue;
        const Instruction jump = decode(memory, addr, {});
        bool dispatch = jump.flow == FlowKind::JumpIndirect;
        if (!dispatch && jump.valid && jump.mnemonic == "RET" && jump.operands.empty()) {
            if (auto p = previousInstruction(memory, dis, addr); p && p->mnemonic == "PUSH" && p->operands != "AF") dispatch = true;
        }
        if (!dispatch) continue;

        std::optional<Word> table;
        bool reads = false;
        Word cur = addr;
        for (int k = 0; k < 12; ++k) {
            const auto p = previousInstruction(memory, dis, cur);
            if (!p) break;
            if (p->flow == FlowKind::Jump || p->flow == FlowKind::Return || p->flow == FlowKind::JumpIndirect || p->flow == FlowKind::Call ||
                p->flow == FlowKind::Restart) {
                break;
            }
            if (readsThroughPair(*p)) reads = true;
            if (!immediateLoadPair(*p).empty()) {
                const Word v = immediateValue(*p);
                if (inProgram(v)) {
                    table = v;
                    break;
                }
            }
            cur = p->address;
        }
        if (!table || taken[*table]) continue;

        // (A) a row of JP nn that the computed jump lands in
        {
            JumpTable jt;
            jt.kind = JumpTable::Kind::JumpRow;
            jt.jump = addr;
            jt.table = *table;
            unsigned p = *table;
            while (p + 2 <= ctx.last && jt.slots.size() < 256 && !taken[p]) {
                if (memory.read(static_cast<Word>(p)) != 0xC3) break;
                if (dis.isCovered(static_cast<Word>(p)) && !dis.isInstructionStart(static_cast<Word>(p))) break;
                const Word t = memory.read16(static_cast<Word>(p + 1));
                if (!plausibleTarget(t)) break;
                jt.slots.push_back(static_cast<Word>(p));
                jt.targets.push_back(t);
                p += 3;
            }
            if (jt.slots.size() >= 2) {
                jt.bytes = p - *table;
                jt.how = "row of JP instructions entered by " + jump.text() + at(addr);
                claim(jt);
                out.push_back(std::move(jt));
                continue;
            }
        }
        // (B) a table of addresses that the dispatch code reads
        if (!reads) continue;
        JumpTable jt;
        jt.kind = JumpTable::Kind::Pointers;
        jt.jump = addr;
        jt.table = *table;
        unsigned minTarget = 0x10000;
        for (unsigned slot = *table; slot + 1 <= ctx.last && jt.slots.size() < 256; slot += 2) {
            if (taken[slot] || taken[slot + 1] || dis.isCovered(static_cast<Word>(slot)) || dis.isCovered(static_cast<Word>(slot + 1))) break;
            if (slot != *table && refs.count(static_cast<Word>(slot))) break;  // something else starts here
            if (slot >= minTarget) break;                                      // the table ends where the first target begins
            const Word t = memory.read16(static_cast<Word>(slot));
            if (!plausibleTarget(t) || (t >= *table && t < slot + 2)) break;
            jt.slots.push_back(static_cast<Word>(slot));
            jt.targets.push_back(t);
            if (t > *table) minTarget = std::min<unsigned>(minTarget, t);
        }
        if (jt.slots.size() < 2) continue;
        jt.bytes = static_cast<unsigned>(jt.slots.size() * 2);
        jt.how = "addresses read by the dispatch code ending in " + jump.text() + at(addr);
        claim(jt);
        out.push_back(std::move(jt));
    }

    // (2) regions the user or the model marked as pointers: tables of code addresses
    for (const auto& [start, length] : ctx.pointerRegions) {
        JumpTable jt;
        jt.kind = JumpTable::Kind::Pointers;
        jt.table = start;
        jt.bytes = length;
        for (unsigned slot = start; slot + 1 < static_cast<unsigned>(start) + length && slot + 1 < kAddressSpace; slot += 2) {
            const Word t = memory.read16(static_cast<Word>(slot));
            if (!inProgram(t) || isData(t) || (dis.isCovered(t) && !dis.isInstructionStart(t))) continue;
            jt.slots.push_back(static_cast<Word>(slot));
            jt.targets.push_back(t);
        }
        jt.how = "marked as a pointer table";
        if (!jt.slots.empty()) {
            claim(jt);
            out.push_back(std::move(jt));
        }
    }

    // (3) suggestions: three or more code addresses in a row behind a plain data reference (LD HL,table) - could also be numbers
    for (Word r : immediates) {
        if (!inProgram(r) || dis.isCovered(r) || taken[r] || isData(r)) continue;
        JumpTable jt;
        jt.kind = JumpTable::Kind::Pointers;
        jt.table = r;
        jt.followed = false;
        for (unsigned slot = r; slot + 1 <= ctx.last && jt.slots.size() < 256; slot += 2) {
            if (taken[slot] || taken[slot + 1] || dis.isCovered(static_cast<Word>(slot)) || dis.isCovered(static_cast<Word>(slot + 1)) || isData(static_cast<Word>(slot))) break;
            if (slot != r && refs.count(static_cast<Word>(slot))) break;
            const Word t = memory.read16(static_cast<Word>(slot));
            if (!plausibleTarget(t) || looksLikeText(t) || (t >= r && t < slot + 2)) break;
            jt.slots.push_back(static_cast<Word>(slot));
            jt.targets.push_back(t);
        }
        if (jt.slots.size() < 3) continue;
        jt.bytes = static_cast<unsigned>(jt.slots.size() * 2);
        jt.how = "a row of code addresses behind a data reference (could also be numbers)";
        claim(jt);
        out.push_back(std::move(jt));
    }
    std::sort(out.begin(), out.end(), [](const JumpTable& x, const JumpTable& y) { return x.table < y.table; });
    return out;
}

// ---- data types ----------------------------------------------------------------------------------------------------------------

std::vector<DataGuess> guessData(const Memory& memory, const Disassembler& dis, const std::vector<JumpTable>& tables, const AnalysisContext& ctx) {
    std::vector<std::uint8_t> used(kAddressSpace, 0);
    for (const JumpTable& t : tables) {
        if (t.kind == JumpTable::Kind::Pointers && t.followed) {
            for (unsigned i = 0; i < t.bytes && t.table + i < kAddressSpace; ++i) used[t.table + i] = 1;
        }
    }
    auto rd = [&](unsigned a) { return memory.read(static_cast<Word>(a)); };
    auto free = [&](unsigned a) {
        return a >= ctx.first && a <= ctx.last && a < kAddressSpace && !dis.isCovered(static_cast<Word>(a)) && !used[a] &&
               !(ctx.isData && ctx.isData(static_cast<Word>(a))) && !(ctx.claimed && ctx.claimed(static_cast<Word>(a)));
    };
    const unsigned end = std::min<unsigned>(ctx.last, 0xFFFF) + 1;

    // references from the code
    std::set<Word> refs, immediates, targets;
    std::map<Word, std::set<unsigned>> sizes;  // memory operand -> access sizes
    std::map<Word, std::string> how;
    for (unsigned a = 0; a < kAddressSpace; ++a) {
        if (!dis.isInstructionStart(static_cast<Word>(a))) continue;
        if (auto d = dataOperandAt(memory, static_cast<Word>(a))) {
            refs.insert(d->value);
            if (d->memory) {
                sizes[d->value].insert(d->size);
                if (!how.count(d->value)) how[d->value] = decode(memory, static_cast<Word>(a), {}).text() + at(static_cast<Word>(a));
            } else {
                immediates.insert(d->value);
            }
        }
    }
    for (const auto& [t, c] : dis.callers()) (void)c, targets.insert(t);
    for (const auto& [t, c] : dis.jumpers()) (void)c, targets.insert(t);

    std::vector<DataGuess> out;
    auto take = [&](const DataGuess& g) {
        for (unsigned i = 0; i < g.length; ++i) used[g.start + i] = 1;
        out.push_back(g);
    };
    /// printable characters up to a NUL or a character with bit 7 set (at most 80): what a pointer of a string table points at
    auto stringAt = [&](unsigned t, unsigned& length) {
        unsigned n = 0;
        while (n < 80 && t + n < end && free(t + n) && textChar(rd(t + n))) ++n;
        if (t + n >= end || !free(t + n)) return false;
        if (rd(t + n) == 0 && n > 0) {
            length = n + 1;
            return true;
        }
        if (bit7End(rd(t + n))) {
            length = n + 1;
            return true;
        }
        return false;
    };
    auto unmistakableString = [&](unsigned start) {
        unsigned q = start, letters = 0;
        while (q < end && free(q) && textChar(rd(q))) letters += letterOrSpace(rd(q++)) ? 1U : 0U;
        const unsigned chars = q - start;
        return chars >= 8 && letters * 10 >= chars * 8 && q < end && (rd(q) == 0 || bit7End(rd(q)));
    };

    // (1) tables of 16-bit numbers: LD HL,table (also DE, BC) and then the pair is read word by word: LD r,(HL) / INC HL / LD r,(HL)
    for (unsigned a = 0; a < kAddressSpace; ++a) {
        if (!dis.isInstructionStart(static_cast<Word>(a))) continue;
        const Instruction load = decode(memory, static_cast<Word>(a), {});
        std::string pair = immediateLoadPair(load);
        if (pair.empty() || pair == "IX" || pair == "IY") continue;
        const Word table = immediateValue(load);
        if (!free(table)) continue;
        std::vector<Instruction> seq;
        Word p = load.next();
        bool enteredCall = false;
        for (int k = 0; k < 24 && dis.isInstructionStart(p); ++k) {
            Instruction i = decode(memory, p, {});
            // the table is often passed to a routine in HL: follow one CALL into the callee
            if (i.flow == FlowKind::Call && i.target && !enteredCall && dis.isInstructionStart(*i.target)) {
                enteredCall = true;
                p = *i.target;
                continue;
            }
            if (i.flow == FlowKind::Jump || i.flow == FlowKind::Return || i.flow == FlowKind::Call || i.flow == FlowKind::JumpIndirect ||
                i.flow == FlowKind::ConditionalCall) {
                break;
            }
            seq.push_back(i);
            p = i.next();
        }
        bool words = false;
        std::string evidence;
        for (std::size_t k = 0; k + 2 < seq.size(); ++k) {
            const std::string ptr = "(" + pair + ")";
            auto readsPtr = [&](const Instruction& i) {
                const auto comma = i.operands.find(',');
                return i.mnemonic == "LD" && comma != std::string::npos && i.operands.substr(comma + 1) == ptr;
            };
            if (readsPtr(seq[k]) && seq[k + 1].mnemonic == "INC" && seq[k + 1].operands == pair && readsPtr(seq[k + 2])) {
                words = true;
                evidence = seq[k].text() + " / " + seq[k + 1].text() + " / " + seq[k + 2].text() + at(seq[k].address);
                break;
            }
            if (seq[k].mnemonic == "EX" && seq[k].operands == "DE,HL") {
                pair = pair == "HL" ? "DE" : pair == "DE" ? "HL" : pair;
            } else if (writesPair(seq[k], pair) && !(seq[k].mnemonic == "ADD")) {
                break;  // the pair does not mean the table any more
            }
        }
        if (!words || used[table]) continue;
        if (unmistakableString(table)) continue;
        unsigned e = table;
        while (e < end && free(e) && (e == table || !(refs.count(static_cast<Word>(e)) || targets.count(static_cast<Word>(e))))) ++e;
        const unsigned length = (e - table) / 2 * 2;
        if (length < 4) continue;
        unsigned stringy = 0;
        for (unsigned q = table; q + 1 < table + length && q < table + 16u; q += 2) {
            unsigned len = 0;
            if (stringAt(memory.read16(static_cast<Word>(q)), len)) ++stringy;
        }
        if (stringy >= 3) continue;  // addresses of strings: a pointer table (found below)
        bool allZero = true;
        for (unsigned i = 0; i < length && allZero; ++i) allZero = rd(table + i) == 0;
        if (allZero) continue;  // an empty buffer
        DataGuess g;
        g.kind = DataGuess::Kind::Words;
        g.start = table;
        g.length = length;
        g.reason = "read word by word: " + evidence;
        take(g);
    }

    // (2) tables of string pointers behind a data reference; the strings may be short ("No", "OK"). Before the strings: the high byte of an
    // address is often printable (&40 = '@') and would be taken for the first character of the string behind the table.
    for (Word r : refs) {
        if (!free(r)) continue;
        unsigned n = 0, longOnes = 0;
        std::map<Word, unsigned> strings;
        for (unsigned p = r; p + 1 < end && free(p) && free(p + 1); p += 2) {
            if (p != r && refs.count(static_cast<Word>(p))) break;
            const Word t = memory.read16(static_cast<Word>(p));
            unsigned len = 0;
            if (t < ctx.first || t > ctx.last || (t >= r && t < p + 2) || !stringAt(t, len) || len < 2) break;
            strings[t] = len;
            if (len >= 5) ++longOnes;
            ++n;
        }
        if (!((n >= 2 && longOnes >= 1) || n >= 3)) continue;
        DataGuess g;
        g.kind = DataGuess::Kind::Pointers;
        g.start = r;
        g.length = n * 2;
        g.reason = "table of " + std::to_string(n) + " string pointers";
        take(g);
        for (const auto& [t, len] : strings) {
            if (used[t]) continue;
            DataGuess sg;
            sg.kind = DataGuess::Kind::Text;
            sg.start = t;
            sg.length = len;
            sg.reason = std::string(refs.count(t) ? "referenced by an instruction, " : "") + "pointed to by a table of string pointers";
            for (unsigned i = 0; i < len; ++i) {
                const Byte b = rd(t + i);
                if (b != 0) sg.preview += static_cast<char>(b & 0x7F);
            }
            take(sg);
        }
    }

    // (3) strings
    for (unsigned a = std::max<unsigned>(ctx.first, 0); a < end;) {
        if (!free(a) || !textChar(rd(a))) {
            ++a;
            continue;
        }
        unsigned e = a;
        while (e < end && free(e) && textChar(rd(e))) ++e;
        unsigned chars = e - a;
        bool terminated = false;
        unsigned total = chars;
        if (e < end && free(e) && rd(e) == 0) {
            terminated = true;
            total = chars + 1;
        } else if (e < end && free(e) && bit7End(rd(e))) {
            terminated = true;
            ++chars;
            total = chars;
        }
        const bool referenced = refs.count(static_cast<Word>(a)) != 0;
        unsigned letters = 0;
        for (unsigned i = a; i < a + chars; ++i) letters += letterOrSpace(static_cast<Byte>(rd(i) & 0x7F)) ? 1U : 0U;
        // "~~~~" or "!!!!" is a run of instructions or a fill pattern, not a string: a period of 1 or 2 only counts if referenced
        bool periodic = chars >= 4;
        for (unsigned i = a + 2; i < e && periodic; ++i) periodic = rd(i) == rd(i - 2);
        const bool enough = chars >= 4 && letters * 10 >= chars * 6 && (referenced || terminated || chars >= 12) && (!periodic || referenced);
        if (!enough) {
            a = e;
            continue;
        }
        DataGuess g;
        g.kind = DataGuess::Kind::Text;
        g.start = static_cast<Word>(a);
        g.length = total;
        g.reason = std::string(referenced ? "referenced by an instruction, " : "") +
                   (terminated ? (rd(a + chars - 1) >= 0x80 ? "last character has bit 7 set" : "NUL-terminated") : "long run of printable characters");
        for (unsigned i = a; i < a + chars && g.preview.size() < 40; ++i) g.preview += static_cast<char>(rd(i) & 0x7F);
        take(g);
        a += total;
    }

    // (4) word variables: a memory operand that is only ever accessed with 16-bit loads (LD HL,(nn) / LD (nn),DE ...) and whose address is
    // not taken (LD HL,nn). Bytes say nothing: that is what the listing shows anyway.
    for (const auto& [r, set] : sizes) {
        if (set.size() != 1 || *set.begin() != 2 || immediates.count(r)) continue;
        if (!free(r) || !free(static_cast<unsigned>(r) + 1) || refs.count(static_cast<Word>(r + 1))) continue;
        DataGuess g;
        g.kind = DataGuess::Kind::Words;
        g.start = r;
        g.length = 2;
        g.reason = "accessed as a 16-bit variable: " + how[r];
        take(g);
    }

    // (5) runs of zero bytes
    for (unsigned a = ctx.first; a < end;) {
        if (!free(a) || rd(a) != 0) {
            ++a;
            continue;
        }
        unsigned e = a;
        while (e < end && free(e) && rd(e) == 0) ++e;
        if (e - a >= 16) {
            DataGuess g;
            g.kind = DataGuess::Kind::Zeros;
            g.start = static_cast<Word>(a);
            g.length = e - a;
            g.reason = "zero fill";
            out.push_back(g);
        }
        a = e;
    }
    std::sort(out.begin(), out.end(), [](const DataGuess& x, const DataGuess& y) { return x.start < y.start; });
    return out;
}

// ---- routines ------------------------------------------------------------------------------------------------------------------

std::vector<RoutineInfo> computeRoutines(const Memory& memory, const Disassembler& dis, const std::set<Word>& entryList,
                                         const std::vector<JumpTable>& tables) {
    std::set<Word> entries;
    for (Word e : entryList) {
        if (dis.isInstructionStart(e)) entries.insert(e);
    }
    std::map<Word, Word> viaTable;
    for (const JumpTable& t : tables) {
        if (!t.followed) continue;
        for (Word target : t.targets) {
            if (dis.isInstructionStart(target)) viaTable.emplace(target, t.table);
        }
    }

    std::map<Word, RoutineInfo> infos;
    std::map<Word, std::set<Word>> tailCallers;
    for (Word entry : entries) {
        RoutineInfo info;
        info.entry = entry;
        info.start = entry;
        info.end = entry;
        std::set<Word> seen;
        std::vector<Word> work{entry};
        bool sawReturn = false, sawIndirect = false;
        // A routine is everything its jumps reach (CALLs lead to other routines, backward jumps below the entry belong to someone else,
        // a jump to the entry of another routine is a tail call).
        while (!work.empty() && seen.size() < 4096) {
            Word a = work.back();
            work.pop_back();
            while (a >= entry && dis.isInstructionStart(a) && seen.size() < 4096 && seen.insert(a).second) {
                const Instruction ins = decode(memory, a, {});
                if ((ins.flow == FlowKind::Call || ins.flow == FlowKind::ConditionalCall) && ins.target) {
                    if (dis.isInstructionStart(*ins.target)) info.callees.insert(*ins.target);
                    else info.external.insert(*ins.target);
                }
                if ((ins.flow == FlowKind::Jump || ins.flow == FlowKind::ConditionalJump) && ins.target) {
                    if (*ins.target != entry && entries.count(*ins.target)) {
                        info.callees.insert(*ins.target);
                        tailCallers[*ins.target].insert(a);
                    } else if (*ins.target >= entry) {
                        work.push_back(*ins.target);
                    }
                }
                if (ins.flow == FlowKind::Return) { sawReturn = true; break; }
                if (ins.flow == FlowKind::Jump) break;
                if (ins.flow == FlowKind::JumpIndirect) { sawIndirect = true; break; }
                if (ins.flow == FlowKind::ConditionalReturn) sawReturn = true;
                if (static_cast<unsigned>(a) + ins.length > 0xFFFF) break;
                a = ins.next();
                if (a != entry && entries.count(a)) {  // falls through into the next routine
                    info.callees.insert(a);
                    break;
                }
            }
        }
        for (Word a : seen) {  // `seen` is ordered: neighbouring instructions form one block
            const Instruction ins = decode(memory, a, {});
            const unsigned e = static_cast<unsigned>(a) + ins.length;
            ++info.instructions;
            info.bytes += ins.length;
            info.start = std::min<unsigned>(info.start, a);
            info.end = std::max<unsigned>(info.end, e);
            if (!info.blocks.empty() && info.blocks.back().second == a) info.blocks.back().second = e;
            else info.blocks.emplace_back(a, e);
        }
        info.endsWith = sawReturn ? "ret" : (sawIndirect ? "jump_indirect" : "jump");
        if (auto it = dis.callers().find(entry); it != dis.callers().end()) info.callers.insert(it->second.begin(), it->second.end());
        if (auto vt = viaTable.find(entry); vt != viaTable.end()) info.viaTable = vt->second;
        infos[entry] = std::move(info);
    }
    for (auto& [entry, info] : infos) {
        if (auto it = tailCallers.find(entry); it != tailCallers.end()) {
            for (Word j : it->second) {
                if (!(j >= info.start && j < info.end)) info.callers.insert(j);  // its own loop back to the entry is not a call
            }
        }
    }

    // depth over the call graph: leaves first (cycles are cut by treating a routine under evaluation as depth 0)
    std::map<Word, int> depth;
    std::set<Word> active;
    std::function<int(Word)> eval = [&](Word e) -> int {
        if (auto it = depth.find(e); it != depth.end()) return it->second;
        if (!active.insert(e).second) return 0;
        int d = 0;
        for (Word c : infos[e].callees) {
            if (infos.count(c) && c != e) d = std::max(d, 1 + eval(c));
        }
        active.erase(e);
        return depth[e] = d;
    };
    std::vector<RoutineInfo> out;
    for (auto& [entry, info] : infos) {
        info.depth = eval(entry);
        out.push_back(info);
    }
    return out;
}

}  // namespace z80
