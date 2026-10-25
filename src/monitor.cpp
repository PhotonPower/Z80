#include "z80/monitor.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "z80/assembler.hpp"
#include "z80/decoder.hpp"
#include "z80/encoder.hpp"
#include "z80/expression.hpp"
#include "z80/format.hpp"
#include "z80/image_io.hpp"
#include "z80/version.hpp"

namespace z80 {
namespace {

struct MonitorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string upperCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string h4(unsigned v) { return hexDigits(v & 0xFFFF, 4); }
std::string h2(unsigned v) { return hexDigits(v & 0xFF, 2); }

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

/// Splits a line into words; "double quoted text" stays one word (quotes kept).
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : line) {
        if (quoted) {
            current += c;
            if (c == '"') quoted = false;
        } else if (c == '"') {
            current += c;
            quoted = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) out.push_back(std::move(current)), current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

}  // namespace

struct Monitor::Impl {
    Monitor& m;
    std::ostringstream out;
    std::vector<std::string> args;  // arguments without the command word

    explicit Impl(Monitor& monitor) : m(monitor) {}

    // ---- argument parsing ---------------------------------------------------------------
    std::int64_t number(const std::string& text) const {
        // "123." = decimal
        if (text.size() > 1 && text.back() == '.' &&
            std::all_of(text.begin(), text.end() - 1, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            return std::stoll(text.substr(0, text.size() - 1));
        }
        ExpressionEvaluator ev(
            [this](const std::string& name) -> std::optional<std::int64_t> {
                if (name.size() > 1 && name[0] == '@') {
                    if (auto r = getRegister(m.cpu_.regs(), name.substr(1))) return static_cast<std::int64_t>(*r);
                    return std::nullopt;
                }
                if (auto it = m.symbols_.find(name); it != m.symbols_.end()) return it->second.value;
                return std::nullopt;
            },
            m.cpu_.regs().pc, true);
        const ExprResult r = ev.evaluate(text);
        if (!r.ok) throw MonitorError("'" + text + "': " + r.error);
        if (!r.undefinedNames.empty()) throw MonitorError("unknown symbol '" + r.undefinedNames[0] + "'");
        return r.value;
    }
    Word addr(const std::string& text) const {
        const std::int64_t v = number(text);
        if (v < 0 || v > 0xFFFF) throw MonitorError("address out of range: " + text);
        return static_cast<Word>(v);
    }
    Word addrArg(std::size_t i, std::optional<Word> def = std::nullopt) const {
        if (i < args.size()) return addr(args[i]);
        if (def) return *def;
        throw MonitorError("missing argument " + std::to_string(i + 1));
    }
    std::int64_t countArg(std::size_t i, std::int64_t def, std::int64_t max) const {
        if (i >= args.size()) return def;
        const std::int64_t v = number(args[i]);
        if (v < 1 || v > max) throw MonitorError("count must be between 1 and " + std::to_string(max));
        return v;
    }
    /// Bytes from words starting at `first`; "text" words become ASCII bytes.
    std::vector<Byte> bytesFrom(std::size_t first) const {
        std::vector<Byte> bytes;
        for (std::size_t i = first; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a.size() >= 2 && a.front() == '"' && a.back() == '"') {
                for (std::size_t k = 1; k + 1 < a.size(); ++k) bytes.push_back(static_cast<Byte>(a[k]));
            } else {
                const std::int64_t v = number(a);
                if (v < -128 || v > 255) throw MonitorError("byte out of range: " + a);
                bytes.push_back(static_cast<Byte>(v));
            }
        }
        if (bytes.empty()) throw MonitorError("no bytes given");
        return bytes;
    }
    void requireFiles() const {
        if (!m.options_.allowFiles) throw MonitorError("file access is disabled");
    }
    const std::string& word(std::size_t i, const char* what) const {
        if (i >= args.size()) throw MonitorError(std::string("missing ") + what);
        return args[i];
    }

    // ---- output helpers -----------------------------------------------------------------
    std::map<Word, std::string> labelMap(bool onlyLabels) const {
        std::map<Word, std::string> labels;
        for (const auto& [name, sym] : m.symbols_) {
            if (onlyLabels && sym.isConstant) continue;
            if (sym.value >= 0 && sym.value <= 0xFFFF) labels.emplace(static_cast<Word>(sym.value), name);
        }
        return labels;
    }

    Instruction decodeAt(Word a, const std::map<Word, std::string>& labels) const {
        DecodeOptions options;
        options.symbolizer = [&labels](Word t) -> std::optional<std::string> {
            if (auto it = labels.find(t); it != labels.end()) return it->second;
            return std::nullopt;
        };
        return decode(m.memory_, a, options);
    }

    static std::string bytesText(const Instruction& ins) {
        std::string s;
        for (int i = 0; i < ins.length; ++i) {
            if (i) s += ' ';
            s += h2(ins.bytes[static_cast<std::size_t>(i)]);
        }
        return s;
    }

    std::string instructionLine(Word a, bool markPc, const std::map<Word, std::string>& labels) const {
        const Instruction ins = decodeAt(a, labels);
        return std::string(markPc && a == m.cpu_.regs().pc ? "> " : "  ") + h4(a) + "  " + pad(bytesText(ins), 12) +
               " " + ins.text() + "\n";
    }

    void showRegisters() {
        const Registers& r = m.cpu_.regs();
        out << " PC=" << h4(r.pc) << " SP=" << h4(r.sp) << "  AF=" << h4(r.af()) << " BC=" << h4(r.bc())
            << " DE=" << h4(r.de()) << " HL=" << h4(r.hl()) << " IX=" << h4(r.ix) << " IY=" << h4(r.iy) << "\n";
        out << " AF'=" << h4(r.af2()) << " BC'=" << h4(r.bc2()) << " DE'=" << h4(r.de2()) << " HL'=" << h4(r.hl2())
            << "  I=" << h2(r.i) << " R=" << h2(r.r) << "  IM" << static_cast<int>(r.im) << (r.iff1 ? " EI" : " DI")
            << (r.halted ? " HALT" : "") << "  " << flagsToString(r.f()) << "\n";
    }

    void showNext() {
        out << instructionLine(m.cpu_.regs().pc, false, labelMap(false));
    }

    void report(const RunResult& r) {
        out << "stopped: " << toString(r.reason);
        if (r.reason == StopReason::Breakpoint || r.reason == StopReason::Address) out << " at " << h4(r.pc);
        if (r.reason == StopReason::Watchpoint && r.watchAddress) {
            out << " at " << h4(*r.watchAddress) << (r.watchWrite ? " (write)" : " (read)");
        }
        out << "  [" << r.instructions << " instructions, " << r.cycles << " T-states]\n";
        showRegisters();
        showNext();
    }

    RunLimits limits() const {
        RunLimits l;
        l.maxInstructions = m.limit_;
        return l;
    }

    // ---- commands -----------------------------------------------------------------------
    void cmdDisassemble() {
        Word a = addrArg(0, m.nextDisassembly_);
        const auto count = countArg(1, 12, 4096);
        const auto labels = labelMap(false);
        const auto codeLabels = labelMap(true);
        for (std::int64_t i = 0; i < count; ++i) {
            if (auto it = codeLabels.find(a); it != codeLabels.end()) out << it->second << ":\n";
            const Instruction ins = decodeAt(a, labels);
            out << instructionLine(a, true, labels);
            a = ins.next();
        }
        m.nextDisassembly_ = a;
    }

    void cmdDump() {
        Word a = addrArg(0, m.nextDump_);
        const auto count = static_cast<std::size_t>(countArg(1, 128, 65536));
        for (std::size_t done = 0; done < count; done += 16) {
            const std::size_t n = std::min<std::size_t>(16, count - done);
            std::string hex, ascii;
            for (std::size_t i = 0; i < 16; ++i) {
                if (i < n) {
                    const Byte b = m.memory_.read(static_cast<Word>(a + i));
                    hex += h2(b) + ' ';
                    ascii += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
                } else {
                    hex += "   ";
                }
                if (i == 7) hex += ' ';
            }
            out << h4(a) << "  " << hex << " |" << ascii << "|\n";
            a = static_cast<Word>(a + n);
        }
        m.nextDump_ = a;
    }

    void cmdEdit() {
        const Word a = addrArg(0);
        const auto bytes = bytesFrom(1);
        for (std::size_t i = 0; i < bytes.size(); ++i) m.memory_.write(static_cast<Word>(a + i), bytes[i]);
        out << bytes.size() << " byte(s) written at " << h4(a) << "\n";
    }

    void cmdFill() {
        const Word from = addrArg(0), to = addrArg(1);
        if (to < from) throw MonitorError("end is below start");
        const auto pattern = bytesFrom(2);
        for (std::uint32_t a = from, k = 0; a <= to; ++a, ++k) m.memory_.write(static_cast<Word>(a), pattern[k % pattern.size()]);
        out << (to - from + 1) << " byte(s) filled\n";
    }

    void cmdCompare() {
        const Word from = addrArg(0), to = addrArg(1), other = addrArg(2);
        if (to < from) throw MonitorError("end is below start");
        int diffs = 0;
        for (std::uint32_t a = from; a <= to; ++a) {
            const Byte x = m.memory_.read(static_cast<Word>(a));
            const Byte y = m.memory_.read(static_cast<Word>(other + (a - from)));
            if (x != y && ++diffs <= 32) out << h4(a) << ": " << h2(x) << "  " << h4(other + (a - from)) << ": " << h2(y) << "\n";
        }
        out << (diffs == 0 ? "equal" : std::to_string(diffs) + " difference(s)") << "\n";
    }

    void cmdTransfer() {
        const Word from = addrArg(0), to = addrArg(1), dest = addrArg(2);
        if (to < from) throw MonitorError("end is below start");
        std::vector<Byte> buffer;
        for (std::uint32_t a = from; a <= to; ++a) buffer.push_back(m.memory_.read(static_cast<Word>(a)));
        for (std::size_t i = 0; i < buffer.size(); ++i) m.memory_.write(static_cast<Word>(dest + i), buffer[i]);
        out << buffer.size() << " byte(s) copied to " << h4(dest) << "\n";
    }

    void cmdHunt() {
        const Word from = addrArg(0), to = addrArg(1);
        if (to < from) throw MonitorError("end is below start");
        const auto pattern = bytesFrom(2);
        int found = 0;
        for (std::uint32_t a = from; a <= to && found < 64; ++a) {
            bool match = true;
            for (std::size_t i = 0; match && i < pattern.size(); ++i) match = m.memory_.read(static_cast<Word>(a + i)) == pattern[i];
            if (match) {
                out << h4(a) << "\n";
                ++found;
            }
        }
        if (found == 0) out << "not found\n";
    }

    void cmdRegisters() {
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::string name = args[i], value;
            if (auto eq = name.find('='); eq != std::string::npos) {
                value = name.substr(eq + 1);
                name = name.substr(0, eq);
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                throw MonitorError("usage: r [name=value ...]");
            }
            const std::int64_t v = number(value);
            if (v < 0 || !setRegister(m.cpu_.regs(), name, static_cast<std::uint32_t>(v))) {
                throw MonitorError("cannot set " + name + " to " + value);
            }
            if (upperCase(name) == "PC") m.cpu_.regs().halted = false;  // repositioning leaves HALT
        }
        showRegisters();
        showNext();
    }

    void cmdStep() {
        const auto count = countArg(0, 1, 1'000'000);
        const bool verbose = count <= 20;
        const auto labels = labelMap(false);
        RunResult last;
        std::uint64_t executed = 0, cycles = 0;
        for (std::int64_t i = 0; i < count; ++i) {
            if (verbose && count > 1) out << instructionLine(m.cpu_.regs().pc, false, labels);
            last = m.cpu_.stepInstruction();
            executed += last.instructions;
            cycles += last.cycles;
            if (last.reason != StopReason::Stepped) break;
        }
        last.instructions = executed;
        last.cycles = cycles;
        if (last.reason != StopReason::Stepped) {
            report(last);
        } else {
            showRegisters();
            showNext();
        }
    }

    void cmdBreak() {
        if (args.empty()) {
            if (m.cpu_.breakpoints().empty() && m.cpu_.watchpoints().empty()) out << "no breakpoints\n";
            for (Word a : m.cpu_.breakpoints()) out << "break " << h4(a) << "\n";
            for (const auto& [a, kind] : m.cpu_.watchpoints()) {
                out << "watch " << h4(a) << (kind == WatchKind::Read ? " r" : kind == WatchKind::Write ? " w" : " rw") << "\n";
            }
            return;
        }
        for (const auto& a : args) m.cpu_.addBreakpoint(addr(a));
        out << "ok\n";
    }

    void cmdBreakClear() {
        if (args.empty()) {
            m.cpu_.clearBreakpoints();
            m.cpu_.clearWatchpoints();
        } else {
            for (const auto& a : args) {
                m.cpu_.removeBreakpoint(addr(a));
                m.cpu_.removeWatchpoint(addr(a));
            }
        }
        out << "ok\n";
    }

    void cmdWatch() {
        const Word a = addrArg(0);
        WatchKind kind = WatchKind::Access;
        if (args.size() > 1) {
            const std::string k = lowerCase(args[1]);
            if (k == "r") kind = WatchKind::Read;
            else if (k == "w") kind = WatchKind::Write;
            else if (k != "rw" && k != "a") throw MonitorError("kind must be r, w or rw");
        }
        m.cpu_.addWatchpoint(a, kind);
        out << "ok\n";
    }

    void cmdAssembleMode() {
        m.assembleAt_ = addrArg(0, m.cpu_.regs().pc);
        out << "assemble mode: type assembler lines (numbers like &1F or 31), empty line to finish\n";
    }

    /// One line typed in assemble mode.
    void assembleLine(const std::string& line) {
        {   // A single word without ':' that is no instruction is almost certainly a typo; the
            // assembler would silently take it as a label.
            const auto words = tokenize(line);
            if (words.size() == 1 && words[0].find(':') == std::string::npos && !isMnemonic(words[0])) {
                out << "? unknown mnemonic '" << words[0] << "' (write 'name:' to define a label)\n";
                return;
            }
        }
        AssemblerOptions options;
        options.origin = *m.assembleAt_;
        for (const auto& [name, sym] : m.symbols_) options.predefined[name] = sym.value;
        const AssemblyResult r = Assembler(options).assemble(line + "\n");
        if (!r.ok()) {
            for (const auto& d : r.diagnostics) {
                out << "? " << d.message << "\n";
                if (d.message.rfind("undefined symbol", 0) == 0) {
                    out << "  (lines are assembled one at a time: define it first with `sym name addr`)\n";
                }
            }
            return;
        }
        for (const auto& [name, sym] : r.symbols) {
            if (!sym.isEqu || m.symbols_.count(name) == 0) m.symbols_[name] = Sym{sym.value, sym.isEqu};
        }
        if (r.empty()) return;
        r.applyTo(m.memory_);
        const std::size_t size = r.size();
        const Word start = r.lowAddress();
        const auto labels = labelMap(false);
        const Instruction ins = decodeAt(start, labels);
        std::string bytes, text;
        if (ins.length == size) {
            bytes = bytesText(ins);
            text = ins.text();
        } else {  // data (DB/DW/DS): show the first bytes
            for (std::size_t i = 0; i < std::min<std::size_t>(size, 4); ++i) bytes += (i ? " " : "") + h2(m.memory_.read(static_cast<Word>(start + i)));
            if (size > 4) bytes += " ..";
            text = std::to_string(size) + " byte(s) of data";
        }
        out << "  " << h4(start) << "  " << pad(bytes, 12) << " " << text << "\n";
        m.assembleAt_ = static_cast<Word>(*m.assembleAt_ + size);
    }

    void cmdSymbols() {
        if (args.empty()) {
            if (m.symbols_.empty()) out << "no symbols\n";
            for (const auto& [name, sym] : m.symbols_) out << pad(name, 16) << h4(static_cast<unsigned>(sym.value)) << (sym.isConstant ? "  =" : "") << "\n";
            return;
        }
        if (args[0].front() == '-') {
            m.symbols_.erase(upperCase(args[0].substr(1)));
            out << "ok\n";
            return;
        }
        if (args.size() < 2) throw MonitorError("usage: sym [name value | -name]");
        m.symbols_[upperCase(args[0])] = Sym{number(args[1]), true};
        out << "ok\n";
    }

    void cmdAsmFile() {
        requireFiles();
        const std::string& file = word(0, "file name");
        const auto bytes = readFile(file);
        AssemblerOptions options;
        if (args.size() > 1) options.origin = addr(args[1]);
        for (const auto& [name, sym] : m.symbols_) options.predefined[name] = sym.value;
        const AssemblyResult r = Assembler(options).assemble(std::string(bytes.begin(), bytes.end()));
        if (!r.ok()) {
            out << r.formatDiagnostics(file);
            out << r.errorCount() << " error(s), nothing loaded\n";
            return;
        }
        r.applyTo(m.memory_);
        for (const auto& [name, sym] : r.symbols) m.symbols_[name] = Sym{sym.value, sym.isEqu};
        if (r.empty()) {
            out << "no code generated\n";
        } else {
            out << r.size() << " bytes at " << h4(r.lowAddress()) << "-" << h4(r.highAddress()) << ", " << r.symbols.size() << " symbol(s)\n";
        }
    }

    void cmdLoad() {
        requireFiles();
        const auto data = readFile(word(0, "file name"));
        if (looksLikeSna(data) && args.size() < 2) {
            m.memory_.load(0, loadSna(data).bytes());
            out << "snapshot loaded (64 KiB RAM)\n";
        } else {
            const Word a = addrArg(1, Word{0});
            m.memory_.load(a, data);
            out << data.size() << " bytes loaded at " << h4(a) << "\n";
        }
    }

    void cmdSave() {
        requireFiles();
        const Word from = addrArg(1), to = addrArg(2);
        if (to < from) throw MonitorError("end is below start");
        std::vector<Byte> data;
        for (std::uint32_t a = from; a <= to; ++a) data.push_back(m.memory_.read(static_cast<Word>(a)));
        writeFile(word(0, "file name"), data);
        out << data.size() << " bytes saved\n";
    }

    void cmdSymbolFile(bool load) {
        requireFiles();
        const std::string& file = word(0, "file name");
        if (load) {
            const auto bytes = readFile(file);
            std::istringstream in(std::string(bytes.begin(), bytes.end()));
            std::string name, value, rest;
            int n = 0;
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                if (!(ls >> name >> value)) continue;
                const auto v = parseConstant("0x" + value);
                if (!v) continue;
                const bool equ = (ls >> rest) && rest == "EQU";
                m.symbols_[upperCase(name)] = Sym{*v, equ};
                ++n;
            }
            out << n << " symbol(s) loaded\n";
        } else {
            std::string text;
            for (const auto& [name, sym] : m.symbols_) text += pad(name, 16) + h4(static_cast<unsigned>(sym.value)) + (sym.isConstant ? "  EQU" : "") + "\n";
            writeFile(file, std::vector<Byte>(text.begin(), text.end()));
            out << m.symbols_.size() << " symbol(s) saved\n";
        }
    }

    void cmdHelp() {
        out << "Numbers are hexadecimal: 4000  FF  &FF  0x4000  100.(decimal)  start+3  @HL (register)\n"
               "\n"
               "  d [addr [n]]         disassemble              m [addr [n]]       dump memory\n"
               "  e addr b..|\"text\"    write bytes              f from to b..      fill\n"
               "  c from to other      compare                  t from to dest     copy\n"
               "  h from to b..|\"t\"    hunt (search)            a [addr]           assemble lines at addr\n"
               "  r [name=value ..]    registers                n [count]          step\n"
               "  o                    step over a CALL         u                  run until return\n"
               "  g [addr]             go (run from addr)       ru addr            run until addr\n"
               "  b [addr..]           breakpoint(s)/list       bc [addr..]        clear (all if none)\n"
               "  w addr [r|w|rw]      watchpoint               limit n            max instructions per run\n"
               "  sym [name value|-name]  symbols               symload/symsave f  symbol files\n"
               "  asm file [org]       assemble a source file   l file [addr]      load binary / .sna\n"
               "  sv file from to      save memory              ck name / rb name  checkpoint / rollback\n"
               "  int [data] / nmi     interrupts               io / input b..     OUT log / queue IN data\n"
               "  savestate f / loadstate f   machine snapshot  reset [mem]        reset CPU (and RAM)\n"
               "  q                    quit\n";
    }

    // ---- dispatch -----------------------------------------------------------------------
    void run(const std::string& command) {
        if (command == "d") return cmdDisassemble();
        if (command == "m") return cmdDump();
        if (command == "e") return cmdEdit();
        if (command == "f") return cmdFill();
        if (command == "c") return cmdCompare();
        if (command == "t") return cmdTransfer();
        if (command == "h") return cmdHunt();
        if (command == "a") return cmdAssembleMode();
        if (command == "r") return cmdRegisters();
        if (command == "n" || command == "s") return cmdStep();
        if (command == "o") return report(m.cpu_.stepOver(limits()));
        if (command == "u") return report(m.cpu_.runUntilReturn(limits()));
        if (command == "g") {
            if (!args.empty()) {
                m.cpu_.regs().pc = addr(args[0]);
                m.cpu_.regs().halted = false;
            }
            return report(m.cpu_.run(limits()));
        }
        if (command == "ru") return report(m.cpu_.runUntil(addrArg(0), limits()));
        if (command == "b") return cmdBreak();
        if (command == "bc") return cmdBreakClear();
        if (command == "w") return cmdWatch();
        if (command == "limit") {
            m.limit_ = static_cast<std::uint64_t>(countArg(0, static_cast<std::int64_t>(m.limit_), 4'000'000'000LL));
            out << "limit " << m.limit_ << " instructions per run\n";
            return;
        }
        if (command == "sym") return cmdSymbols();
        if (command == "symload") return cmdSymbolFile(true);
        if (command == "symsave") return cmdSymbolFile(false);
        if (command == "asm") return cmdAsmFile();
        if (command == "l") return cmdLoad();
        if (command == "sv") return cmdSave();
        if (command == "ck") {
            m.checkpoints_[word(0, "name")] = captureSnapshot(m.cpu_);
            out << "checkpoint '" << args[0] << "' saved\n";
            return;
        }
        if (command == "rb") {
            auto it = m.checkpoints_.find(word(0, "name"));
            if (it == m.checkpoints_.end()) throw MonitorError("unknown checkpoint '" + args[0] + "'");
            restoreSnapshot(m.cpu_, it->second);
            showRegisters();
            showNext();
            return;
        }
        if (command == "int") {
            m.cpu_.requestInterrupt(static_cast<Byte>(args.empty() ? 0xFF : number(args[0])));
            out << "interrupt requested\n";
            return;
        }
        if (command == "nmi") {
            m.cpu_.requestNmi();
            out << "NMI requested\n";
            return;
        }
        if (command == "io") {
            if (m.ioLog_.empty()) out << "no output\n";
            for (const auto& [port, v] : m.ioLog_) out << "OUT " << h4(port) << " <- " << h2(v) << "\n";
            m.ioLog_.clear();
            return;
        }
        if (command == "input") {
            for (Byte b : bytesFrom(0)) m.input_.push_back(b);
            out << m.input_.size() << " byte(s) queued\n";
            return;
        }
        if (command == "savestate") {
            requireFiles();
            writeFile(word(0, "file name"), serializeSnapshot(captureSnapshot(m.cpu_)));
            out << "saved\n";
            return;
        }
        if (command == "loadstate") {
            requireFiles();
            restoreSnapshot(m.cpu_, deserializeSnapshot(readFile(word(0, "file name"))));
            showRegisters();
            showNext();
            return;
        }
        if (command == "reset") {
            m.cpu_.reset();
            if (!args.empty() && lowerCase(args[0]) == "mem") m.memory_.clear();
            showRegisters();
            return;
        }
        if (command == "?" || command == "help") return cmdHelp();
        if (command == "q" || command == "quit" || command == "x") {
            m.finished_ = true;
            return;
        }
        throw MonitorError("unknown command '" + command + "' (? for help)");
    }
};

Monitor::Monitor(MonitorOptions options)
    : options_(options), memoryStore_(std::make_unique<Memory>()), memory_(*memoryStore_), cpu_(memory_),
      limit_(options.maxInstructions) {
    cpu_.setPortHandlers(
        [this](Word) -> Byte {
            if (input_.empty()) return 0xFF;
            const Byte b = input_.front();
            input_.pop_front();
            return b;
        },
        [this](Word port, Byte value) {
            ioLog_.emplace_back(port, value);
            while (ioLog_.size() > 4096) ioLog_.pop_front();
        });
}

std::string Monitor::banner() {
    return std::string("Z80 monitor ") + kVersion + "  (? for help, q to quit)\n";
}

std::string Monitor::prompt() const {
    return assembleAt_ ? hexDigits(*assembleAt_, 4) + ": " : "- ";
}

std::string Monitor::execute(std::string_view line) {
    Impl impl(*this);
    try {
        if (assembleAt_) {
            std::string text(line);
            if (text.find_first_not_of(" \t\r") == std::string::npos) {
                assembleAt_.reset();
                return "";
            }
            impl.assembleLine(text);
            return impl.out.str();
        }
        auto words = tokenize(line);
        if (words.empty()) return "";
        const std::string command = lowerCase(words.front());
        impl.args.assign(words.begin() + 1, words.end());
        impl.run(command);
    } catch (const std::exception& e) {
        impl.out << "? " << e.what() << "\n";
    }
    return impl.out.str();
}

}  // namespace z80
