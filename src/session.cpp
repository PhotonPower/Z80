#include "z80/session.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

#include "z80/decoder.hpp"
#include "z80/expression.hpp"
#include "z80/format.hpp"
#include "z80/image_io.hpp"

namespace z80 {
namespace {

struct SessionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string hex16(unsigned v) { return formatHex(v & 0xFFFF, 4, NumberStyle::CPrefix); }
std::string hex8(unsigned v) { return formatHex(v & 0xFF, 2, NumberStyle::CPrefix); }

std::string toHex(const std::vector<Byte>& bytes) {
    std::string s;
    for (Byte b : bytes) s += hexDigits(b, 2);
    return s;
}

std::vector<Byte> fromHex(const std::string& text) {
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

std::string asciiOf(const std::vector<Byte>& bytes) {
    std::string s;
    for (Byte b : bytes) s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    return s;
}

}  // namespace

struct Session::Impl {
    Session& s;
    const Json& req;
    Impl(Session& session, const Json& request) : s(session), req(request) {}

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

    RunLimits limits() const {
        RunLimits l;
        l.maxInstructions = static_cast<std::uint64_t>(integer("max_instructions", 1'000'000, 0, 4'000'000'000LL));
        l.maxCycles = static_cast<std::uint64_t>(integer("max_cycles", 0, 0, 1LL << 50));
        return l;
    }

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

    Json disassemble(Word from, int count) const {
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
        if (!r.empty()) out.set("start", hex16(r.lowAddress())).set("end", hex16(r.highAddress()));
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

    Json cmdRead() const {
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
        }
        Json j = Json::object();
        j.set("regs", regs_());
        return j;
    }
    Json regs_() const { return regs(); }

    Json cmdStep() {
        const int n = static_cast<int>(integer("n", 1, 1, 1'000'000));
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
        const RunResult res = s.cpu_.runUntil(sentinel, limits());
        Json j = runJson(res);
        j.set("returned", res.reason == StopReason::Address && s.cpu_.regs().sp == spAfterReturn);
        return j;
    }

    Json cmdDiff() {
        const std::string name = text("name");
        auto it = s.checkpoints_.find(name);
        if (it == s.checkpoints_.end()) throw SessionError("unknown checkpoint '" + name + "'");
        const std::vector<Byte>& old = it->second.memory;
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

    Json cmdHelp() const {
        static const char* kHelp[][2] = {
            {"asm", "assemble {source, origin?, entry?} into memory; symbols become usable in later arguments"},
            {"dis", "disassemble {addr? (default PC), count? (default 8)}"},
            {"read", "read memory {addr, len?} -> hex + ascii"},
            {"write", "write memory {addr, hex | bytes[]}"},
            {"load_file", "load a binary file {file, addr?} (needs --allow-files)"},
            {"regs", "show all registers"},
            {"set", "set registers {regs: {\"A\": 5, \"HL\": \"0x4000\"}}"},
            {"step", "execute {n?, trace?} instructions"},
            {"step_over", "execute one instruction; run through a CALL"},
            {"run", "run {max_instructions?, max_cycles?, until?} until breakpoint/limit/halt"},
            {"run_until_return", "run until the current routine returns"},
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
            {"help", "this list"},
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
        if (cmd == "step_over") return runJson(cpu.stepOver(limits()));
        if (cmd == "run") {
            if (const Json* u = req.find("until")) return runJson(cpu.runUntil(address(*u, "until"), limits()));
            return runJson(cpu.run(limits()));
        }
        if (cmd == "run_until_return") return runJson(cpu.runUntilReturn(limits()));
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
        if (cmd == "checkpoint") { s.checkpoints_[text("name")] = captureSnapshot(cpu); return j; }
        if (cmd == "rollback") {
            auto it = s.checkpoints_.find(text("name"));
            if (it == s.checkpoints_.end()) throw SessionError("unknown checkpoint");
            restoreSnapshot(cpu, it->second);
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
            for (const auto& [port, v] : s.ioLog_) {
                Json e = Json::object();
                e.set("port", hex16(port)).set("value", hex8(v));
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
            const auto data = readFile(text("file"));
            const Word a = addr("addr", Word{0});
            if (looksLikeSna(data) && !req.find("addr")) s.memory_ = loadSna(data);
            else s.memory_.load(a, data);
            j.set("loaded", data.size());
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
        throw SessionError("unknown command '" + cmd + "' (try \"help\")");
    }
};

Session::Session(SessionOptions options)
    : options_(options), memoryStore_(std::make_unique<Memory>()), memory_(*memoryStore_), cpu_(memory_) {
    cpu_.setPortHandlers(
        [this](Word) -> Byte {
            if (input_.empty()) return 0xFF;
            const Byte b = input_.front();
            input_.pop_front();
            return b;
        },
        [this](Word port, Byte value) {
            ioLog_.emplace_back(port, value);
            while (ioLog_.size() > options_.maxIoLog) ioLog_.pop_front();
        });
}

Json Session::handle(const Json& request) {
    Json reply = Json::object();
    reply.set("ok", true);
    Impl impl(*this, request);
    try {
        if (!request.isObject()) throw SessionError("request must be a JSON object");
        const Json body = impl.run();
        for (const auto& [k, v] : body.members()) reply.set(k, v);
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        if (impl.detailed_.isArray()) err.set("errors", impl.detailed_);
        return err;
    }
    return reply;
}

std::string Session::handleLine(std::string_view line) {
    try {
        return handle(Json::parse(line)).dump();
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        return err.dump();
    }
}

}  // namespace z80
