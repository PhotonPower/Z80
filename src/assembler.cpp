#include "z80/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "z80/encoder.hpp"
#include "z80/expression.hpp"
#include "z80/format.hpp"

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

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '@' ||
           c == '?';
}
bool isIdentChar(char c) { return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool isValidLabel(std::string_view s) {
    if (s.empty() || !isIdentStart(s.front())) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return isIdentChar(c); });
}

/// Removes a trailing ";comment" (ignoring semicolons inside string/char literals).
std::string stripComment(std::string_view line) {
    char quote = 0;
    char prev = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
        } else if (c == '"' || (c == '\'' && !std::isalnum(static_cast<unsigned char>(prev)))) {
            quote = c;
        } else if (c == ';') {
            return std::string(line.substr(0, i));
        }
        prev = c;
    }
    return std::string(line);
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            lines.push_back(current);
            current.clear();
        } else if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

bool isPseudo(const std::string& u) {
    return u == "ORG" || u == "EQU" || u == "END" || u == "DB" || u == "DEFB" || u == "DW" ||
           u == "DEFW" || u == "DM" || u == "DEFM" || u == "DS" || u == "DEFS";
}

class Run {
public:
    Run(const AssemblerOptions& options, const std::vector<std::string>& lines,
        std::map<std::string, Symbol> symbols, bool finalPass)
        : symbols_(std::move(symbols)), options_(options), lines_(lines), final_(finalPass) {}

    void execute() {
        pc_ = options_.origin;
        for (std::size_t i = 0; i < lines_.size() && !ended_; ++i) {
            lineNo_ = static_cast<int>(i) + 1;
            processLine(lines_[i]);
        }
    }

    std::map<std::string, Symbol> symbols_;
    std::vector<Diagnostic> diagnostics;
    std::vector<Segment> segments;
    std::vector<ListingLine> listing;

private:
    // ---- diagnostics ------------------------------------------------------------
    void error(const std::string& message) {
        if (!final_) return;
        diagnostics.push_back({Diagnostic::Severity::Error, lineNo_, message, lines_[lineNo_ - 1]});
    }

    // ---- expression evaluation --------------------------------------------------
    ExprResult eval(std::string_view text, std::int64_t pc) {
        ExpressionEvaluator evaluator(
            [this](const std::string& name) -> std::optional<std::int64_t> {
                auto it = symbols_.find(name);
                if (it == symbols_.end()) return std::nullopt;
                return it->second.value;
            },
            pc);
        ExprResult r = evaluator.evaluate(text);
        if (final_) {
            if (!r.ok) error(r.error);
            for (const auto& name : r.undefinedNames) error("undefined symbol '" + name + "'");
            r.undefinedNames.clear();
            r.error.clear();
            r.ok = true;
        }
        return r;
    }

    // ---- output -----------------------------------------------------------------
    void emit(const std::vector<Byte>& bytes) {
        if (!final_ || bytes.empty()) return;
        if (static_cast<std::uint32_t>(pc_) + bytes.size() > kAddressSpace) {
            error("code exceeds address space (&FFFF)");
            return;
        }
        if (!segments.empty() && segments.back().end() == pc_) {
            segments.back().bytes.insert(segments.back().bytes.end(), bytes.begin(), bytes.end());
        } else {
            segments.push_back({pc_, bytes});
        }
    }

    void record(const std::string& source, const std::vector<Byte>& bytes, bool hasAddress,
                Word address) {
        if (!final_) return;
        listing.push_back({lineNo_, address, bytes, source, hasAddress});
    }

    void advance(std::size_t count) { pc_ = static_cast<Word>(pc_ + count); }

    // ---- symbols ----------------------------------------------------------------
    void define(const std::string& name, std::int64_t value, bool isEqu) {
        const std::string key = upper(name);
        if (definedThisPass_.count(key) != 0) {
            error("duplicate label '" + name + "'");
            return;
        }
        definedThisPass_.insert(key);
        symbols_[key] = Symbol{value, lineNo_, isEqu};
    }

    // ---- line processing --------------------------------------------------------
    void processLine(const std::string& rawLine) {
        const std::string code = stripComment(rawLine);
        const std::string stripped = trim(code);
        const Word lineAddress = pc_;
        if (stripped.empty()) {
            record(rawLine, {}, false, 0);
            return;
        }

        // Tokenise: [label] [mnemonic] [operands]
        std::string rest = stripped;
        std::string label;
        std::string word = firstWord(rest);

        // "label:" or "label:mnemonic"
        if (auto colon = word.find(':'); colon != std::string::npos) {
            label = word.substr(0, colon);
            rest = trim(rest.substr(colon + 1));
            if (!isValidLabel(label)) {
                error("invalid label '" + label + "'");
                record(rawLine, {}, false, 0);
                return;
            }
        } else if (!isMnemonic(word) && !isPseudo(upper(word))) {
            // Not an instruction: must be a label (only valid starting in column 0).
            if (!std::isspace(static_cast<unsigned char>(code.front())) && isValidLabel(word)) {
                label = word;
                rest = trim(rest.substr(word.size()));
            } else {
                error("unknown mnemonic '" + word + "'");
                record(rawLine, {}, false, 0);
                return;
            }
        }
        if (!label.empty() && isReservedName(label)) {
            error("'" + label + "' is a reserved word and cannot be a label");
            label.clear();
        }

        std::string mnemonic;
        std::string operandText;
        if (!rest.empty()) {
            mnemonic = firstWord(rest);
            operandText = trim(rest.substr(mnemonic.size()));
        }
        const std::string mn = upper(mnemonic);

        if (mn == "EQU") {
            if (label.empty()) {
                error("EQU needs a label");
            } else if (operandText.empty()) {
                error("EQU needs a value");
            } else {
                const ExprResult r = eval(operandText, pc_);
                define(label, r.value, true);
            }
            const auto it = symbols_.find(upper(label));
            record(rawLine, {}, it != symbols_.end(),
                   it != symbols_.end() ? static_cast<Word>(it->second.value) : Word{0});
            return;
        }

        if (!label.empty()) define(label, pc_, false);

        if (mn.empty()) {
            record(rawLine, {}, true, lineAddress);
            return;
        }

        std::vector<Byte> bytes;
        if (isPseudo(mn)) {
            bytes = pseudo(mn, operandText);
        } else {
            bytes = instruction(mnemonic, operandText);
        }
        emit(bytes);
        record(rawLine, bytes, mn != "ORG" && mn != "END", lineAddress);
        advance(bytes.size());
    }

    static std::string firstWord(const std::string& s) {
        std::size_t i = 0;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(0, i);
    }

    std::vector<Byte> instruction(const std::string& mnemonic, const std::string& operandText) {
        const std::vector<std::string> operands = splitOperands(operandText);
        const Word pc = pc_;
        EncodeResult r = encodeInstruction(
            mnemonic, operands, pc,
            [this, pc](std::string_view expr) { return eval(expr, pc); });
        if (!r.error.empty()) error(r.error);
        return r.bytes;
    }

    std::vector<Byte> pseudo(const std::string& mn, const std::string& operandText) {
        std::vector<Byte> out;
        if (mn == "END") {
            ended_ = true;
        } else if (mn == "ORG") {
            if (operandText.empty()) {
                error("ORG needs an address");
            } else {
                const std::int64_t v = eval(operandText, pc_).value;
                if (v < 0 || v > 0xFFFF) {
                    error("ORG address out of range");
                } else {
                    pc_ = static_cast<Word>(v);
                }
            }
        } else if (mn == "DB" || mn == "DEFB" || mn == "DM" || mn == "DEFM") {
            for (const auto& item : splitOperands(operandText)) {
                if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
                    for (std::size_t i = 1; i + 1 < item.size(); ++i) {
                        out.push_back(static_cast<Byte>(item[i]));
                    }
                    continue;
                }
                const std::int64_t v = eval(item, pc_).value;
                if (v < -128 || v > 255) error("value out of 8-bit range: " + std::to_string(v));
                out.push_back(static_cast<Byte>(v & 0xFF));
            }
            if (out.empty() && operandText.empty()) error(mn + " needs at least one value");
        } else if (mn == "DW" || mn == "DEFW") {
            for (const auto& item : splitOperands(operandText)) {
                const std::int64_t v = eval(item, pc_).value;
                if (v < -32768 || v > 65535) error("value out of 16-bit range: " + std::to_string(v));
                out.push_back(static_cast<Byte>(v & 0xFF));
                out.push_back(static_cast<Byte>((v >> 8) & 0xFF));
            }
            if (out.empty()) error(mn + " needs at least one value");
        } else {  // DS / DEFS
            const auto items = splitOperands(operandText);
            if (items.empty() || items.size() > 2) {
                error("DS needs a count and an optional fill value");
            } else {
                const std::int64_t count = eval(items[0], pc_).value;
                const std::int64_t fill = items.size() == 2 ? eval(items[1], pc_).value : 0;
                if (count < 0 || count > 0xFFFF) {
                    error("DS count out of range");
                } else {
                    out.assign(static_cast<std::size_t>(count), static_cast<Byte>(fill & 0xFF));
                }
            }
        }
        return out;
    }

    const AssemblerOptions& options_;
    const std::vector<std::string>& lines_;
    bool final_;
    Word pc_ = 0;
    int lineNo_ = 0;
    bool ended_ = false;
    std::set<std::string> definedThisPass_;
};

std::map<std::string, std::int64_t> valuesOf(const std::map<std::string, Symbol>& symbols) {
    std::map<std::string, std::int64_t> v;
    for (const auto& [name, sym] : symbols) v[name] = sym.value;
    return v;
}

}  // namespace

// ---- AssemblyResult ---------------------------------------------------------------

std::size_t AssemblyResult::errorCount() const {
    return static_cast<std::size_t>(std::count_if(
        diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; }));
}

Word AssemblyResult::lowAddress() const {
    if (segments.empty()) return 0;
    Word low = segments.front().start;
    for (const auto& s : segments) low = std::min(low, s.start);
    return low;
}

Word AssemblyResult::highAddress() const {
    if (segments.empty()) return 0;
    std::uint32_t high = 0;
    for (const auto& s : segments) high = std::max(high, s.end());
    return static_cast<Word>(high - 1);
}

std::size_t AssemblyResult::size() const {
    std::size_t n = 0;
    for (const auto& s : segments) n += s.bytes.size();
    return n;
}

std::vector<Byte> AssemblyResult::flatten(Byte fill) const {
    if (segments.empty()) return {};
    const Word low = lowAddress();
    std::vector<Byte> out(static_cast<std::size_t>(highAddress()) - low + 1, fill);
    for (const auto& s : segments) {
        std::copy(s.bytes.begin(), s.bytes.end(), out.begin() + (s.start - low));
    }
    return out;
}

void AssemblyResult::applyTo(Memory& memory) const {
    for (const auto& s : segments) memory.load(s.start, s.bytes);
}

std::string AssemblyResult::formatDiagnostics(const std::string& fileName) const {
    std::ostringstream out;
    for (const auto& d : diagnostics) {
        out << fileName << ":" << d.line << ": "
            << (d.severity == Diagnostic::Severity::Error ? "error" : "warning") << ": " << d.message
            << "\n    " << trim(d.source) << "\n";
    }
    return out.str();
}

std::string AssemblyResult::formatListing() const {
    std::ostringstream out;
    for (const auto& l : listing) {
        constexpr std::size_t kBytesPerRow = 4;
        std::string addr = l.hasAddress ? hexDigits(l.address, 4) : "    ";
        std::size_t index = 0;
        do {
            std::string bytes;
            for (std::size_t i = index; i < std::min(l.bytes.size(), index + kBytesPerRow); ++i) {
                bytes += hexDigits(l.bytes[i], 2);
            }
            std::string row = addr + "  " + bytes;
            row.append(row.size() < 14 ? 14 - row.size() : 0, ' ');
            if (index == 0) {
                std::string number = std::to_string(l.line);
                row += std::string(5 - std::min<std::size_t>(5, number.size()), ' ') + number + "  " +
                       l.source;
            }
            out << row << "\n";
            index += kBytesPerRow;
            addr = l.hasAddress ? hexDigits(static_cast<Word>(l.address + index), 4) : "    ";
        } while (index < l.bytes.size());
    }
    return out.str();
}

std::string AssemblyResult::formatSymbols() const {
    std::ostringstream out;
    for (const auto& [name, sym] : symbols) {
        out << name << std::string(name.size() < 16 ? 16 - name.size() : 1, ' ')
            << hexDigits(static_cast<unsigned>(sym.value & 0xFFFF), 4) << (sym.isEqu ? "  EQU" : "") << "\n";
    }
    return out.str();
}

// ---- Assembler --------------------------------------------------------------------

AssemblyResult Assembler::assemble(std::string_view source) const {
    const std::vector<std::string> lines = splitLines(source);

    // Resolve labels (and forward-referenced EQUs) by iterating until the symbol table is stable.
    std::map<std::string, Symbol> symbols;
    for (const auto& [name, value] : options_.predefined) {
        std::string upperName = name;
        for (char& c : upperName) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        symbols[upperName] = Symbol{value, 0, true};
    }
    constexpr int kMaxPasses = 16;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        Run run(options_, lines, symbols, false);
        run.execute();
        const bool stable = valuesOf(run.symbols_) == valuesOf(symbols);
        symbols = std::move(run.symbols_);
        if (stable && pass > 0) break;
    }

    Run finalRun(options_, lines, symbols, true);
    finalRun.execute();

    AssemblyResult result;
    result.diagnostics = std::move(finalRun.diagnostics);
    result.symbols = std::move(finalRun.symbols_);
    result.segments = std::move(finalRun.segments);
    result.listing = std::move(finalRun.listing);
    return result;
}

}  // namespace z80
