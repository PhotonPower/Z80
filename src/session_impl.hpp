#pragma once
// Internal: implementation of the Session command interface. Shared by session.cpp (core machine
// commands) and session_analysis.cpp (annotation, listing, screen and CPC commands).

#include <algorithm>
#include <cctype>
#include <functional>

#include "z80/cpc.hpp"
#include "z80/decoder.hpp"
#include "z80/dsk.hpp"
#include "z80/expression.hpp"
#include "z80/format.hpp"
#include "z80/image_io.hpp"
#include "z80/listing.hpp"
#include "z80/session.hpp"

namespace z80 {
namespace detail {

struct SessionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string hex16(unsigned v) { return formatHex(v & 0xFFFF, 4, NumberStyle::CPrefix); }
inline std::string hex8(unsigned v) { return formatHex(v & 0xFF, 2, NumberStyle::CPrefix); }

inline std::string toHex(const std::vector<Byte>& bytes) {
    std::string s;
    for (Byte b : bytes) s += hexDigits(b, 2);
    return s;
}

inline std::vector<Byte> fromHex(const std::string& text) {
    std::string digits;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw SessionError("invalid hex string");
        digits += c;
    }
    if (digits.size() % 2 != 0) throw SessionError("hex string has an odd number of digits");
    std::vector<Byte> out;
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        out.push_back(static_cast<Byte>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    }
    return out;
}

inline std::string asciiOf(const std::vector<Byte>& bytes) {
    std::string s;
    for (Byte b : bytes) s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    return s;
}

}  // namespace detail
using namespace detail;

struct Session::Impl {
    Session& s;
    const Json& req;
    Impl(Session& session, const Json& request) : s(session), req(request) {}

    /// While alive, the CPU's ROM overlays are switched off so that reads see the RAM.
    struct RamView {
        Session& s;
        bool low, high;
        explicit RamView(Session& session) : s(session), low(session.memory_.lowRomEnabled()), high(session.memory_.highRomEnabled()) {
            s.memory_.enableRoms(false, false);
        }
        ~RamView() { s.memory_.enableRoms(low, high); }
        RamView(const RamView&) = delete;
        RamView& operator=(const RamView&) = delete;
    };

    // commands implemented in session_analysis.cpp
    bool analysisCommand(const std::string& cmd, Json& result);
    Json cmdMachine();
    Json cmdLoadSna();
    Json cmdLoadRom();
    Json cmdVideo();
    Json cmdAnalyze();
    Json cmdRoutines();
    Json cmdJumpTables();
    Json cmdDataRegions();
    Json cmdReferences();
    AnalysisContext analysisContext() const;
    std::set<Word> routineEntries() const;
    Json cmdListing();
    Json cmdAnnotate(const std::string& cmd);
    Json cmdScreen(const std::string& cmd);

    // ---- argument helpers ---------------------------------------------------------------
    std::int64_t value(const Json& v, const std::string& what) const {
        if (v.isNumber()) return v.asInt();
        if (v.isString()) {
            ExpressionEvaluator evaluator([this](const std::string& name) -> std::optional<std::int64_t> {
                if (auto it = s.symbols_.find(name); it != s.symbols_.end()) return it->second.value;
                if (auto r = getRegister(s.cpu_.regs(), name)) return static_cast<std::int64_t>(*r);
                return std::nullopt;
            }, s.cpu_.regs().pc);
            const ExprResult r = evaluator.evaluate(v.asString());
            if (!r.ok) throw SessionError("'" + what + "': " + r.error);
            if (!r.undefinedNames.empty()) throw SessionError("'" + what + "': unknown symbol " + r.undefinedNames[0]);
            return r.value;
        }
        throw SessionError("'" + what + "' must be a number or an expression string");
    }

    Word address(const Json& v, const std::string& what) const {
        const std::int64_t n = value(v, what);
        if (n < 0 || n > 0xFFFF) throw SessionError("'" + what + "' is out of range (0..65535)");
        return static_cast<Word>(n);
    }

    Word addr(const char* key, std::optional<Word> def = std::nullopt) const {
        if (const Json* v = req.find(key)) return address(*v, key);
        if (def) return *def;
        throw SessionError(std::string("missing argument '") + key + "'");
    }

    std::int64_t integer(const char* key, std::int64_t def, std::int64_t lo, std::int64_t hi) const {
        const Json* v = req.find(key);
        if (!v || v->isNull()) return def;
        const std::int64_t n = value(*v, key);
        if (n < lo || n > hi) throw SessionError(std::string("'") + key + "' must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        return n;
    }

    bool flag(const char* key, bool def) const {
        const Json* v = req.find(key);
        return v && v->isBool() ? v->asBool() : def;
    }

    std::string text(const char* key) const {
        const Json* v = req.find(key);
        if (!v || !v->isString()) throw SessionError(std::string("missing string argument '") + key + "'");
        return v->asString();
    }

    std::vector<Byte> bytesArg() const {
        if (const Json* h = req.find("hex")) {
            if (!h->isString()) throw SessionError("'hex' must be a string");
            return fromHex(h->asString());
        }
        if (const Json* b = req.find("bytes")) {
            if (!b->isArray()) throw SessionError("'bytes' must be an array");
            std::vector<Byte> out;
            for (const Json& item : b->items()) {
                const std::int64_t n = value(item, "bytes");
                if (n < -128 || n > 255) throw SessionError("byte value out of range");
                out.push_back(static_cast<Byte>(n));
            }
            return out;
        }
        throw SessionError("missing 'hex' or 'bytes'");
    }

    /// Upper bound for one run: a model must not be able to block the server for hours with a huge limit and an endless loop.
    static constexpr std::int64_t kMaxInstructions = 100'000'000;

    RunLimits limits(std::int64_t defaultInstructions = 1'000'000) const {
        RunLimits l;
        l.maxInstructions = static_cast<std::uint64_t>(integer("max_instructions", defaultInstructions, 0, kMaxInstructions));
        l.maxCycles = static_cast<std::uint64_t>(integer("max_cycles", 0, 0, kMaxInstructions * 23));
        if (l.maxInstructions == 0 && l.maxCycles == 0) throw SessionError("max_instructions 0 (no limit) needs max_cycles");
        return l;
    }

    /// Counts instructions fetched from empty memory (eight zero bytes: a NOP slide through RAM nothing was loaded into) while alive.
    /// That is nearly always a jump through a pointer that was never set, or a routine that needs state other code sets up first.
    struct EmptyFetchGuard {
        Cpu& cpu;
        std::uint64_t count = 0;
        Word first = 0;
        bool stubs = false;  // the firmware jump block is handled by the stubs: its zeros are not empty memory
        std::vector<Word> history;   // the last `keep` instruction addresses (request "history")
        std::size_t keep = 0, next = 0;
        std::vector<Byte>* coverage = nullptr;  // the session's coverage map, if on
        explicit EmptyFetchGuard(Cpu& c, bool firmwareStubs = false, std::size_t historySize = 0, std::vector<Byte>* cov = nullptr)
            : cpu(c), stubs(firmwareStubs), keep(historySize), coverage(cov && !cov->empty() ? cov : nullptr) {
            history.reserve(keep);
            cpu.setTrace([this](const Cpu& z) {
                const Word pc = z.regs().pc;
                if (coverage) (*coverage)[pc] = 1;
                if (keep) {
                    if (history.size() < keep) history.push_back(pc);
                    else history[next] = pc;
                    next = (next + 1) % keep;
                }
                if (stubs && pc >= 0xB900 && pc <= 0xBDFF) return;
                for (unsigned i = 0; i < 8; ++i) {
                    if (z.memory().read(static_cast<Word>(pc + i)) != 0) return;
                }
                if (count++ == 0) first = pc;
            });
        }
        ~EmptyFetchGuard() { cpu.setTrace(nullptr); }
        EmptyFetchGuard(const EmptyFetchGuard&) = delete;
        EmptyFetchGuard& operator=(const EmptyFetchGuard&) = delete;
        void report(Json& j) const {
            if (keep && !history.empty()) {
                Json h = Json::array();
                const std::size_t n = history.size();
                for (std::size_t i = 0; i < n; ++i) h.push(hex16(history[(next + i) % n]));  // oldest first
                j.set("history", h);
            }
            if (count == 0) return;
            j.set("fetched_from_empty_memory", count).set("first_empty_fetch", hex16(first));
        }
    };

    /// run / runUntil, optionally raising an interrupt every `irq_every_cycles` T-states
    /// (the CPC's Gate Array does this about 300 times a second = every ~13300 T-states).
    RunResult runCommand(std::optional<Word> until, std::optional<RunLimits> given = std::nullopt) {
        const RunLimits l = given ? *given : limits();
        const auto irq = static_cast<std::uint64_t>(integer("irq_every_cycles", 0, 0, 1LL << 40));
        if (irq == 0) return until ? s.cpu_.runUntil(*until, l) : s.cpu_.run(l);
        RunResult total;
        std::uint64_t remaining = l.maxInstructions;
        for (;;) {
            RunLimits slice;
            // interrupts come at fixed points of the machine's time (on a CPC: in step with the vertical sync), however the runs are cut
            const std::uint64_t phase = s.cpu_.cycles() % irq;
            slice.maxCycles = irq - phase;
            slice.maxInstructions = remaining;
            RunResult r = until ? s.cpu_.runUntil(*until, slice) : s.cpu_.run(slice);
            if (r.reason == StopReason::Halted && s.cpu_.regs().iff1) {
                // HALT with interrupts enabled waits for the next one: the CPU idles to the end of the slice
                CpuState st = s.cpu_.state();
                const std::uint64_t idle = slice.maxCycles > r.cycles ? slice.maxCycles - r.cycles : 0;
                st.cycles += idle;
                s.cpu_.setState(st);
                r.cycles += idle;
                r.reason = StopReason::CycleLimit;
            }
            total.instructions += r.instructions;
            total.cycles += r.cycles;
            total.reason = r.reason;
            total.pc = r.pc;
            total.watchAddress = r.watchAddress;
            total.watchWrite = r.watchWrite;
            if (r.reason != StopReason::CycleLimit) break;
            // the real limits are checked first: no interrupt is left pending when the run ends
            if (l.maxCycles != 0 && total.cycles >= l.maxCycles) {
                total.reason = StopReason::CycleLimit;
                break;
            }
            if (remaining != 0) {
                if (r.instructions >= remaining) {
                    total.reason = StopReason::InstructionLimit;
                    break;
                }
                remaining -= r.instructions;
            }
            s.cpu_.requestInterrupt();
        }
        return total;
    }

    bool stubsActive() const { return s.cpc_ && !s.memory_.hasLowRom(); }
    std::size_t historySize() const { return static_cast<std::size_t>(integer("history", 0, 0, 10000)); }

    void requireFiles() const {
        if (!s.options_.allowFiles) throw SessionError("file access is disabled (start the server with --allow-files)");
    }

    // ---- result helpers -----------------------------------------------------------------
    Json regs() const { return registersToJson(s.cpu_.regs()); }

    Json runJson(const RunResult& r) const {
        Json j = Json::object();
        j.set("stop", toString(r.reason)).set("instructions", r.instructions).set("cycles", r.cycles);
        j.set("pc", hex16(r.pc));
        if (r.watchAddress) {
            Json w = Json::object();
            w.set("addr", hex16(*r.watchAddress)).set("write", r.watchWrite);
            j.set("watch", w);
        }
        j.set("regs", regs());
        return j;
    }

    std::map<Word, std::string> labels() const {
        std::map<Word, std::string> out;
        for (const auto& [name, sym] : s.symbols_) {
            if (!sym.isEqu && sym.value >= 0 && sym.value <= 0xFFFF) out.emplace(static_cast<Word>(sym.value), name);
        }
        return out;
    }

    Json disassemble(Word from, int count) {
        std::optional<RamView> ram;
        if (!req.find("view") || !req.find("view")->isString() || req.find("view")->asString() != "cpu") ram.emplace(s);
        const auto names = labels();
        DecodeOptions options;
        options.style = NumberStyle::CPrefix;
        options.symbolizer = [&names](Word a) -> std::optional<std::string> {
            if (auto it = names.find(a); it != names.end()) return it->second;
            return std::nullopt;
        };
        Json lines = Json::array();
        Word a = from;
        for (int i = 0; i < count; ++i) {
            const Instruction ins = decode(s.memory_, a, options);
            Json line = Json::object();
            line.set("addr", hex16(a));
            if (auto it = names.find(a); it != names.end()) line.set("label", it->second);
            line.set("bytes", toHex(std::vector<Byte>(ins.bytes.begin(), ins.bytes.begin() + ins.length)));
            line.set("text", ins.text());
            lines.push(std::move(line));
            a = ins.next();
        }
        return lines;
    }

    // ---- commands -----------------------------------------------------------------------
    Json cmdAsm() {
        AssemblerOptions options;
        options.origin = addr("origin", Word{0});
        options.optimize = flag("optimize", false);
        const AssemblyResult r = Assembler(options).assemble(text("source"));
        Json out = Json::object();
        if (!r.ok()) {
            Json errors = Json::array();
            for (const auto& d : r.diagnostics) {
                Json e = Json::object();
                e.set("line", d.line).set("message", d.message).set("source", d.source);
                errors.push(std::move(e));
            }
            throwWithDetail(errors, "assembly failed");
        }
        r.applyTo(s.memory_);
        s.symbols_ = r.symbols;
        Json segs = Json::array();
        for (const auto& seg : r.segments) {
            Json sj = Json::object();
            sj.set("start", hex16(seg.start)).set("size", seg.bytes.size());
            segs.push(std::move(sj));
        }
        Json syms = Json::object();
        for (const auto& [name, sym] : r.symbols) syms.set(name, hex16(static_cast<unsigned>(sym.value)));
        out.set("size", r.size()).set("segments", segs).set("symbols", syms);
        if (!r.empty()) {
            out.set("start", hex16(r.lowAddress())).set("end", hex16(r.highAddress()));
            s.programRange_ = std::make_pair(static_cast<Word>(r.lowAddress()), static_cast<Word>(r.highAddress()));
        }
        if (const Json* e = req.find("entry")) {
            s.cpu_.regs().pc = address(*e, "entry");
            out.set("pc", hex16(s.cpu_.regs().pc));
        }
        return out;
    }

    [[noreturn]] void throwWithDetail(const Json& detail, const std::string& message) const {
        detailed_ = detail;
        throw SessionError(message);
    }
    mutable Json detailed_;

    Json cmdRead() {
        std::optional<RamView> ram;
        if (!req.find("view") || req.find("view")->asString() != "cpu") ram.emplace(s);
        const Word a = addr("addr");
        const auto len = static_cast<std::size_t>(integer("len", 16, 1, 65536));
        std::vector<Byte> bytes;
        for (std::size_t i = 0; i < len; ++i) bytes.push_back(s.memory_.read(static_cast<Word>(a + i)));
        Json j = Json::object();
        j.set("addr", hex16(a)).set("hex", toHex(bytes)).set("ascii", asciiOf(bytes));
        return j;
    }

    Json cmdWrite() {
        const Word a = addr("addr");
        const auto bytes = bytesArg();
        for (std::size_t i = 0; i < bytes.size(); ++i) s.memory_.write(static_cast<Word>(a + i), bytes[i]);
        Json j = Json::object();
        j.set("written", bytes.size());
        return j;
    }

    Json cmdSet() {
        const Json* regs = req.find("regs");
        if (!regs || !regs->isObject()) throw SessionError("missing object 'regs'");
        for (const auto& [name, v] : regs->members()) {
            const std::int64_t n = value(v, name);
            if (n < 0 || !setRegister(s.cpu_.regs(), name, static_cast<std::uint32_t>(n))) {
                throw SessionError("cannot set register '" + name + "' to that value");
            }
            if (name.size() == 2 && (name[0] == 'P' || name[0] == 'p') && (name[1] == 'C' || name[1] == 'c')) {
                s.cpu_.regs().halted = false;  // repositioning leaves HALT
            }
        }
        Json j = Json::object();
        j.set("regs", regs_());
        return j;
    }
    Json regs_() const { return regs(); }

    Json cmdStep() {
        const int n = static_cast<int>(integer("n", 1, 1, 1'000'000));
        EmptyFetchGuard guard(s.cpu_, stubsActive(), historySize(), &s.coverage_);
        const bool trace = flag("trace", false);
        if (trace && n > 1000) throw SessionError("trace is limited to 1000 steps");
        Json traceLines = Json::array();
        RunResult last;
        std::uint64_t ins = 0, cyc = 0;
        for (int i = 0; i < n; ++i) {
            if (trace) traceLines.push(disassemble(s.cpu_.regs().pc, 1).items().front());
            last = s.cpu_.stepInstruction();
            ins += last.instructions;
            cyc += last.cycles;
            if (last.reason != StopReason::Stepped) break;
        }
        last.instructions = ins;
        last.cycles = cyc;
        Json j = runJson(last);
        if (trace) j.set("trace", traceLines);
        guard.report(j);
        return j;
    }

    Json cmdCall() {
        const Word target = addr("addr");
        const Word sentinel = addr("return_address", Word{0xFFF0});
        if (const Json* regs = req.find("regs")) {
            if (!regs->isObject()) throw SessionError("'regs' must be an object");
            for (const auto& [name, v] : regs->members()) {
                const std::int64_t n = value(v, name);
                if (n < 0 || !setRegister(s.cpu_.regs(), name, static_cast<std::uint32_t>(n))) {
                    throw SessionError("cannot set register '" + name + "'");
                }
            }
        }
        Registers& r = s.cpu_.regs();
        r.sp = static_cast<Word>(r.sp - 2);
        s.memory_.write(r.sp, static_cast<Byte>(sentinel));
        s.memory_.write(static_cast<Word>(r.sp + 1), static_cast<Byte>(sentinel >> 8));
        const Word spAfterReturn = static_cast<Word>(r.sp + 2);
        r.pc = target;
        r.halted = false;
        EmptyFetchGuard guard(s.cpu_, stubsActive(), historySize(), &s.coverage_);
        const RunResult res = runCommand(sentinel);
        Json j = runJson(res);
        // only the return address *with* the stack pointer from before the call counts: a jump through a pointer that is still 0
        // can slide through empty memory and pass the sentinel by chance
        const bool returned = res.reason == StopReason::Address && s.cpu_.regs().sp == spAfterReturn;
        j.set("returned", returned);
        if (res.reason == StopReason::Address && !returned) j.set("note", "reached the return address with a different stack pointer: not a real return");
        guard.report(j);
        return j;
    }

    Json cmdDiff() {
        const std::string name = text("name");
        auto it = s.checkpoints_.find(name);
        if (it == s.checkpoints_.end()) throw SessionError("unknown checkpoint '" + name + "'");
        const std::vector<Byte>& old = it->second.machine.memory;
        Json ranges = Json::array();
        std::size_t total = 0;
        std::size_t shown = 0;
        for (std::size_t a = 0; a < kAddressSpace;) {
            if (old[a] == s.memory_.read(static_cast<Word>(a))) { ++a; continue; }
            std::size_t end = a;
            while (end + 1 < kAddressSpace && old[end + 1] != s.memory_.read(static_cast<Word>(end + 1))) ++end;
            const std::size_t len = end - a + 1;
            total += len;
            if (shown < 50) {
                std::vector<Byte> before(old.begin() + static_cast<std::ptrdiff_t>(a), old.begin() + static_cast<std::ptrdiff_t>(a + std::min<std::size_t>(len, 64)));
                std::vector<Byte> after;
                for (std::size_t i = 0; i < std::min<std::size_t>(len, 64); ++i) after.push_back(s.memory_.read(static_cast<Word>(a + i)));
                Json r = Json::object();
                r.set("addr", hex16(static_cast<unsigned>(a))).set("len", len).set("before", toHex(before)).set("after", toHex(after));
                ranges.push(std::move(r));
                ++shown;
            }
            a = end + 1;
        }
        Json j = Json::object();
        j.set("changed_bytes", total).set("ranges", ranges);
        return j;
    }

    Json cmdUnpack(const std::string& cmd) {
        Memory& mem = s.memory_;
        auto rangesJson = [&](std::size_t minLength, std::size_t most) {
            auto ranges = mem.writtenRanges(minLength);
            std::stable_sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.second - a.first > b.second - b.first; });
            Json arr = Json::array();
            for (std::size_t i = 0; i < ranges.size() && i < most; ++i) {
                Json e = Json::object();
                e.set("start", hex16(ranges[i].first)).set("end", hex16(ranges[i].second - 1)).set("size", ranges[i].second - ranges[i].first);
                arr.push(std::move(e));
            }
            return arr;
        };
        Json j = Json::object();
        if (cmd == "track_writes") {
            const bool on = flag("on", true);
            if (on && (flag("clear", true) || !mem.tracking())) mem.trackWrites(true);
            if (!on) mem.trackWrites(false);
            s.unpackIgnore_.clear();
            s.lastUnpackHit_.reset();
            j.set("tracking", mem.tracking());
            return j;
        }
        if (cmd == "written") {
            j.set("tracking", mem.tracking()).set("bytes", mem.writtenBytes());
            const auto minLength = static_cast<std::size_t>(integer("min_length", 16, 1, 65536));
            const auto most = static_cast<std::size_t>(integer("limit", 20, 1, 200));
            j.set("ranges", rangesJson(minLength, most));
            return j;
        }
        if (cmd == "save_memory") {
            requireFiles();
            const Word a = addr("addr");
            const auto len = static_cast<std::size_t>(integer("len", 0, 1, 65536));
            std::vector<Byte> bytes;
            {
                RamView ram(s);
                for (std::size_t i = 0; i < len; ++i) bytes.push_back(mem.read(static_cast<Word>(a + i)));
            }
            writeFile(text("file"), bytes);
            j.set("file", text("file")).set("addr", hex16(a)).set("bytes", len);
            return j;
        }
        // run_until_unpacked
        const auto minWritten = static_cast<std::size_t>(integer("min_written", 1024, 1, 65536));
        if (!flag("continue", false) || !mem.tracking()) {
            mem.trackWrites(true);
            s.unpackIgnore_.clear();
            s.lastUnpackHit_.reset();
        } else if (s.lastUnpackHit_) {
            s.unpackIgnore_.push_back(*s.lastUnpackHit_);  // the stage that was seen last does not count again
        }
        std::vector<std::pair<unsigned, unsigned>> ignore = s.unpackIgnore_;
        if (const Json* extra = req.find("ignore")) {
            if (!extra->isArray()) throw SessionError("ignore must be a list of [first, last] pairs");
            for (const Json& pair : extra->items()) {
                if (!pair.isArray() || pair.items().size() != 2) throw SessionError("ignore must be a list of [first, last] pairs");
                ignore.emplace_back(address(pair.items()[0], "ignore"), static_cast<unsigned>(address(pair.items()[1], "ignore")) + 1);
            }
        }
        if (s.cpc_ && !s.memory_.hasLowRom()) ignore.emplace_back(0xB900, 0xBE00);  // the firmware jump block of the stubs never counts
        EmptyFetchGuard guard(s.cpu_, stubsActive(), historySize(), &s.coverage_);
        s.cpu_.watchWrittenCode(true, minWritten, ignore);
        RunResult r;
        try {
            r = runCommand(std::nullopt, limits(50'000'000));
        } catch (...) {
            s.cpu_.watchWrittenCode(false);
            throw;
        }
        s.cpu_.watchWrittenCode(false);
        j = runJson(r);
        guard.report(j);
        if (r.reason == StopReason::WrittenCode) {
            const Word pc = s.cpu_.regs().pc;
            for (const auto& [first, end] : mem.writtenRanges(1)) {
                if (pc >= first && pc < end) {
                    s.lastUnpackHit_ = std::make_pair(first, end);
                    Json region = Json::object();
                    region.set("start", hex16(first)).set("end", hex16(end - 1)).set("size", end - first);
                    j.set("region", region);
                    break;
                }
            }
        }
        j.set("written_bytes", mem.writtenBytes()).set("ranges", rangesJson(64, 8));
        return j;
    }

    Json cmdHelp() const {
        static const char* kHelp[][2] = {
            {"asm", "assemble {source, origin?, entry?, optimize?} into memory; symbols become usable in later arguments"},
            {"dis", "disassemble {addr? (default PC), count? (default 8)}"},
            {"read", "read memory {addr, len?} -> hex + ascii"},
            {"write", "write memory {addr, hex | bytes[]}"},
            {"load_file", "load a file {file, addr?, name? (file on a .dsk image), header?: false} (AMSDOS header: load address and PC from it; needs --allow-files)"},
            {"dsk_ls", "directory of a CPC disk image {file} (needs --allow-files)"},
            {"regs", "show all registers"},
            {"set", "set registers {regs: {\"A\": 5, \"HL\": \"0x4000\"}}"},
            {"step", "execute {n?, trace?} instructions"},
            {"step_over", "execute one instruction; run through a CALL"},
            {"run", "run {max_instructions?, max_cycles?, until?} until breakpoint/limit/halt"},
            {"run_until_return", "run until the current routine returns (run, step, call ...: history:n returns the last n instruction addresses)"},
            {"track_writes", "remember which bytes the program writes {on?, clear?}"},
            {"written", "the largest written ranges since track_writes {min_length?, limit?}"},
            {"run_until_unpacked", "run until code is fetched from a written range of at least min_written bytes {min_written?, continue?, ignore?, max_instructions?}"},
            {"save_memory", "write memory to a file {addr, len, file} (needs --allow-files)"},
            {"call", "call a routine {addr, regs?, return_address?, max_instructions?} and run until it returns"},
            {"break", "set breakpoint {addr}"}, {"unbreak", "remove breakpoint {addr}"},
            {"breaks", "list breakpoints/watchpoints"}, {"clear_breaks", "remove all breakpoints and watchpoints"},
            {"watch", "watch memory {addr, kind: read|write|access}"}, {"unwatch", "remove watchpoint {addr}"},
            {"checkpoint", "save machine state {name}"}, {"rollback", "restore {name}"},
            {"diff", "memory changes since checkpoint {name}"}, {"drop", "delete checkpoint {name}"},
            {"checkpoints", "list checkpoints"},
            {"state", "full CPU state as JSON"}, {"set_state", "load CPU state {state}"},
            {"save_state", "write snapshot file {file} (needs --allow-files)"},
            {"load_state", "read snapshot file {file} (needs --allow-files)"},
            {"interrupt", "raise maskable interrupt {data?}"}, {"nmi", "raise non-maskable interrupt"},
            {"io", "fetch (and clear) the OUT log"}, {"input", "queue bytes for IN {hex | bytes[]}"},
            {"reset", "reset CPU {memory?: true clears RAM}"}, {"symbols", "list symbols from the last asm"},
            {"help", "this list"}, {"info", "protocol version of this build"},
            {"machine", "select the machine {type:\"cpc\"}: video hardware, firmware stubs, screen"},
            {"load_sna", "load a CPC snapshot incl. registers and video state {file} (needs --allow-files)"},
            {"load_rom", "load a ROM image {file, which: lower|upper|both} (needs --allow-files)"},
            {"video", "show screen mode, palette, CRTC state"},
            {"screen", "render the screen as PNG {name?, border?} -> png_base64"},
            {"screen_diff", "compare screens {a, b?} -> changed region"}, {"screens", "list stored screens"},
            {"screen_drop", "delete a stored screen {name}"},
            {"firmware_log", "calls to firmware jump block entries seen while stubbed {clear?}"},
            {"console", "text printed through TXT_OUTPUT while stubbed {clear?}"},
            {"insert_disk", "a .dsk for the firmware stubs: CAS_IN_OPEN/DIRECT/CHAR/CLOSE read its files, the disc command READ SECTOR (&84, via KL_FIND_COMMAND) its sectors {file} (needs --allow-files)"},
            {"disk_log", "what was read from the inserted disk, and by which CALL {clear?}"},
            {"coverage", "where instructions were executed {on?, clear?, from?, to?, addresses?: true lists every executed instruction address} -> ranges of executed instruction addresses"},
            {"controls", "joystick and keys as the program sees them {joystick?: bits 0 up 1 down 2 left 3 right 4 fire2 5 fire1, joystick2?, keys?: [0-79]}"},
            {"save_disk", "write the inserted disk (with what the program wrote to it) to a file {file} (needs --allow-files)"},
            {"analyze", "analyse code flow {entries[], from?, to? (the program), inline_strings[], inline_words[] (CALL/RST targets followed by an address, e.g. \"0x18\"), inline_control_args?: {\"0x1F\": 2} (argument bytes of control codes in inline strings), tables?: false}"},
            {"routines", "list subroutines {order?: bottom_up|address, undocumented_only?}"},
            {"listing", "annotated, re-assemblable listing {from?, to?, verify?, max_chars?}"},
            {"ann_label", "name an address {addr, name}"}, {"ann_routine", "document a routine {addr, name, summary, inputs?, outputs?, clobbers?}"},
            {"ann_variable", "name a variable {addr, name, size?, comment?}"}, {"ann_data", "mark data {addr, length, kind: bytes|words|text|pointers, name?, comment?}"},
            {"ann_comment", "comment a line {addr, text, block?}"}, {"ann_remove", "remove a name, data region, comment or constant mark {addr}"},
            {"ann_constant", "the 16-bit operand of the instruction at addr is a number, not an address {addr, remove?}"},
            {"jump_tables", "jump tables found by analyze {max_entries?}"},
            {"data_regions", "guess strings, pointer tables, word tables and variables, zero fill {apply?}"},
            {"references", "data a stretch of code refers to, with type and contents {from, to?}"},
            {"ann_list", "all annotations"}, {"ann_title", "set the listing title {text}"},
            {"ann_save", "write annotations {file} (needs --allow-files)"}, {"ann_load", "read annotations {file} (needs --allow-files)"},
        };
        Json j = Json::object();
        Json cmds = Json::object();
        for (const auto& row : kHelp) cmds.set(row[0], row[1]);
        j.set("commands", cmds);
        return j;
    }

    Json run() {
        const std::string cmd = text("cmd");
        Cpu& cpu = s.cpu_;
        Json j = Json::object();

        if (cmd == "asm") return cmdAsm();
        if (cmd == "dis") {
            j.set("lines", disassemble(addr("addr", cpu.regs().pc), static_cast<int>(integer("count", 8, 1, 1000))));
            return j;
        }
        if (cmd == "read") return cmdRead();
        if (cmd == "write") return cmdWrite();
        if (cmd == "regs") { j.set("regs", regs()); return j; }
        if (cmd == "set") return cmdSet();
        if (cmd == "step") return cmdStep();
        if (cmd == "step_over" || cmd == "run" || cmd == "run_until_return") {
            EmptyFetchGuard guard(cpu, stubsActive(), historySize(), &s.coverage_);
            Json out;
            if (cmd == "step_over") out = runJson(cpu.stepOver(limits()));
            else if (cmd == "run_until_return") out = runJson(cpu.runUntilReturn(limits()));
            else if (const Json* u = req.find("until")) out = runJson(runCommand(address(*u, "until")));
            else out = runJson(runCommand(std::nullopt));
            guard.report(out);
            return out;
        }
        if (cmd == "track_writes" || cmd == "written" || cmd == "run_until_unpacked" || cmd == "save_memory") return cmdUnpack(cmd);
        if (cmd == "call") return cmdCall();
        if (cmd == "break") { cpu.addBreakpoint(addr("addr")); return j; }
        if (cmd == "unbreak") { cpu.removeBreakpoint(addr("addr")); return j; }
        if (cmd == "clear_breaks") { cpu.clearBreakpoints(); cpu.clearWatchpoints(); return j; }
        if (cmd == "breaks") {
            Json b = Json::array();
            for (Word a : cpu.breakpoints()) b.push(hex16(a));
            Json w = Json::array();
            for (const auto& [a, kind] : cpu.watchpoints()) {
                Json e = Json::object();
                e.set("addr", hex16(a)).set("kind", kind == WatchKind::Read ? "read" : kind == WatchKind::Write ? "write" : "access");
                w.push(std::move(e));
            }
            j.set("breakpoints", b).set("watchpoints", w);
            return j;
        }
        if (cmd == "watch") {
            WatchKind kind = WatchKind::Access;
            if (const Json* k = req.find("kind"); k && k->isString()) {
                if (k->asString() == "read") kind = WatchKind::Read;
                else if (k->asString() == "write") kind = WatchKind::Write;
                else if (k->asString() != "access") throw SessionError("kind must be read, write or access");
            }
            cpu.addWatchpoint(addr("addr"), kind);
            return j;
        }
        if (cmd == "unwatch") { cpu.removeWatchpoint(addr("addr")); return j; }
        if (cmd == "checkpoint") { s.checkpoints_[text("name")] = {captureSnapshot(cpu), s.video_}; return j; }
        if (cmd == "rollback") {
            auto it = s.checkpoints_.find(text("name"));
            if (it == s.checkpoints_.end()) throw SessionError("unknown checkpoint");
            restoreSnapshot(cpu, it->second.machine);
            s.video_ = it->second.video;
            if (s.cpc_) s.applyRomState();
            j.set("regs", regs());
            return j;
        }
        if (cmd == "drop") { s.checkpoints_.erase(text("name")); return j; }
        if (cmd == "checkpoints") {
            Json names = Json::array();
            for (const auto& [name, snap] : s.checkpoints_) { (void)snap; names.push(name); }
            j.set("checkpoints", names);
            return j;
        }
        if (cmd == "diff") return cmdDiff();
        if (cmd == "state") { j.set("state", stateToJson(cpu.state())); return j; }
        if (cmd == "set_state") {
            const Json* st = req.find("state");
            if (!st) throw SessionError("missing 'state'");
            cpu.setState(stateFromJson(*st));
            j.set("regs", regs());
            return j;
        }
        if (cmd == "interrupt") { cpu.requestInterrupt(static_cast<Byte>(integer("data", 0xFF, 0, 255))); return j; }
        if (cmd == "nmi") { cpu.requestNmi(); return j; }
        if (cmd == "io") {
            Json out = Json::array();
            for (const auto& w : s.ioLog_) {
                const Word port = w.port;
                const Byte v = w.value;
                Json e = Json::object();
                e.set("port", hex16(port)).set("value", hex8(v)).set("pc", hex16(w.pc));
                if (s.cpc_) {
                    const std::string what = cpc::describePortWrite(port, v);
                    if (!what.empty()) e.set("what", what);
                }
                out.push(std::move(e));
            }
            s.ioLog_.clear();
            j.set("output", out).set("pending_input", s.input_.size());
            return j;
        }
        if (cmd == "input") {
            for (Byte b : bytesArg()) s.input_.push_back(b);
            j.set("pending_input", s.input_.size());
            return j;
        }
        if (cmd == "reset") {
            cpu.reset();
            if (flag("memory", false)) s.memory_.clear();
            j.set("regs", regs());
            return j;
        }
        if (cmd == "symbols") {
            Json syms = Json::object();
            for (const auto& [name, sym] : s.symbols_) syms.set(name, hex16(static_cast<unsigned>(sym.value)));
            j.set("symbols", syms);
            return j;
        }
        if (cmd == "help") return cmdHelp();
        if (cmd == "load_file") {
            requireFiles();
            auto data = readFile(text("file"));
            if (DskImage::looksLikeDsk(data)) {
                // a file from a CPC disk image: {"file": "game.dsk", "name": "GAME.BIN"}
                const Json* n = req.find("name");
                if (!n || !n->isString()) throw SessionError("the file is a disk image: give the file on it with \"name\" (dsk_ls lists them)");
                try {
                    data = DskImage::parse(data).readFile(n->asString());
                } catch (const DskError& e) {
                    throw SessionError(e.what());
                }
            }
            if (const auto h = parseAmsdosHeader(data); h && flag("header", true)) {
                // the header says where the file belongs and where it starts
                data.erase(data.begin(), data.begin() + 128);
                if (h->length < data.size()) data.resize(h->length);
                const Word load = addr("addr", h->load);
                s.memory_.load(load, data);
                if (!data.empty()) s.programRange_ = std::make_pair(load, static_cast<Word>(std::min<std::size_t>(0xFFFF, load + data.size() - 1)));
                if (flag("set_pc", true)) s.cpu_.regs().pc = h->exec;
                Json hj = Json::object();
                hj.set("name", h->name).set("type", h->type).set("load", hex16(h->load)).set("entry", hex16(h->exec)).set("length", h->length);
                j.set("loaded", data.size()).set("amsdos", hj);
                return j;
            }
            const Word a = addr("addr", Word{0});
            if (looksLikeSna(data) && !req.find("addr")) {
                s.memory_.load(0, loadSna(data).bytes());
                s.programRange_.reset();
            } else {
                s.memory_.load(a, data);
                if (!data.empty()) s.programRange_ = std::make_pair(a, static_cast<Word>(std::min<std::size_t>(0xFFFF, a + data.size() - 1)));
            }
            j.set("loaded", data.size());
            return j;
        }
        if (cmd == "dsk_ls") {
            requireFiles();
            try {
                const DskImage disk = DskImage::parse(readFile(text("file")));
                Json files = Json::array();
                for (const DskFile& f : disk.files()) {
                    Json e = Json::object();
                    e.set("name", f.name).set("user", f.user).set("size", f.size).set("system", f.system).set("read_only", f.readOnly);
                    try {
                        if (const auto h = parseAmsdosHeader(disk.readFile(f.name, f.user))) {
                            e.set("load", hex16(h->load)).set("entry", hex16(h->exec)).set("type", h->type).set("length", h->length);
                        }
                    } catch (const DskError&) {
                    }
                    files.push(std::move(e));
                }
                j.set("format", disk.format()).set("tracks", disk.trackCount()).set("files", files);
            } catch (const DskError& e) {
                throw SessionError(e.what());
            }
            return j;
        }
        if (cmd == "save_state") {
            requireFiles();
            writeFile(text("file"), serializeSnapshot(captureSnapshot(cpu)));
            return j;
        }
        if (cmd == "load_state") {
            requireFiles();
            restoreSnapshot(cpu, deserializeSnapshot(readFile(text("file"))));
            j.set("regs", regs());
            return j;
        }
        if (Json analysis; analysisCommand(cmd, analysis)) return analysis;
        throw SessionError("unknown command '" + cmd + "' (try \"help\")");
    }
};

}  // namespace z80
