#include "z80/listing.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "z80/assembler.hpp"
#include "z80/cpc.hpp"
#include "z80/decoder.hpp"
#include "z80/format.hpp"

namespace z80 {
namespace {

std::string h2(unsigned v) { return hexDigits(v & 0xFF, 2); }
std::string h4(unsigned v) { return hexDigits(v & 0xFFFF, 4); }
std::string num2(unsigned v) { return "&" + h2(v); }
std::string num4(unsigned v) { return "&" + h4(v); }

std::string padTo(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

/// Splits text at newlines and wraps long lines at `width` characters.
std::vector<std::string> wrapText(const std::string& text, std::size_t width) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string paragraph;
    while (std::getline(in, paragraph)) {
        std::istringstream words(paragraph);
        std::string word, line;
        while (words >> word) {
            if (!line.empty() && line.size() + 1 + word.size() > width) {
                lines.push_back(line);
                line.clear();
            }
            line += (line.empty() ? "" : " ") + word;
        }
        lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

bool isPrintableAscii(unsigned b) { return b >= 0x20 && b < 0x7F; }

struct Line {
    Word address = 0;
    std::size_t length = 0;
    std::vector<std::string> before;  // label lines, comment blocks (without trailing newline)
    std::string code;                 // instruction or directive text (without indentation)
    std::string comment;              // trailing comment
};

class Builder {
public:
    Builder(const Memory& memory, const Disassembler& dis, const Annotations& ann, const ListingOptions& options,
            Word from, Word to)
        : ram_(memory), dis_(dis), ann_(ann), options_(options), from_(from), to_(to) {}

    ListingResult build();

private:
    // ---- names ----------------------------------------------------------------------------
    bool inRange(Word a) const { return a >= from_ && a <= to_; }
    bool isTarget(Word a) const { return dis_.callers().count(a) != 0 || dis_.jumpers().count(a) != 0; }
    std::optional<std::string> autoName(Word a) const {
        if (dis_.callers().count(a)) return "sub_" + h4(a);
        if (dis_.jumpers().count(a)) return "loc_" + h4(a);
        if (dataRefs_.count(a)) return "data_" + h4(a);
        return std::nullopt;
    }
    /// Addresses of data (not code) that instructions in the listing load or store through.
    void collectDataRefs() {
        DecodeOptions options;
        options.operandSymbolizer = [this](Word v, bool memory) -> std::optional<std::string> {
            if (inRange(v) && (memory || v >= 0x0100) && !dis_.isCovered(v) && !ann_.nameAt(v)) dataRefs_.insert(v);
            return std::nullopt;
        };
        for (std::size_t a = from_; a <= to_; ++a) {
            if (dis_.isInstructionStart(static_cast<Word>(a))) decode(ram_, static_cast<Word>(a), options);
        }
    }
    /// Label that is *defined* at `a` inside the listing (primary name).
    std::optional<std::string> labelAt(Word a) const {
        if (auto n = ann_.nameAt(a)) return n;
        if (inRange(a)) return autoName(a);
        return std::nullopt;
    }
    bool hasBoundary(Word a) const {
        return ann_.nameAt(a) || autoName(a) || ann_.commentAt(a) || ann_.blockCommentAt(a) || ann_.dataAt(a) || dis_.isInstructionStart(a);
    }

    std::optional<std::string> branchSymbol(Word target) {
        if (auto n = ann_.nameAt(target)) return n;
        if (inRange(target)) {
            if (auto n = autoName(target)) return n;
        }
        if (options_.platformSymbols) {
            if (auto fw = cpc::firmwareName(target)) {
                usedFirmware_.emplace(*fw, target);
                return fw;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> operandSymbol(Word value, bool memory) {
        if (!memory && value < 0x0100) return std::nullopt;  // small constants are rarely addresses
        if (auto e = ann_.expressionFor(value)) return e;
        if (inRange(value)) {
            if (auto n = autoName(value)) return n;
        }
        if (!memory && options_.platformSymbols) {
            if (auto fw = cpc::firmwareName(value)) {
                usedFirmware_.emplace(*fw, value);
                return fw;
            }
        }
        return std::nullopt;
    }

    // ---- lines ----------------------------------------------------------------------------
    std::vector<std::string> headerFor(Word a);
    void codeLine(Word a, Line& line);
    void dataRegionLines(const DataRegion& region, std::size_t& index);
    void rawDataLines(Word a, Word limit, std::size_t& consumed);
    std::string wordText(Word value);
    static std::string textRow(const std::vector<Byte>& bytes);

    const Memory& ram_;
    const Disassembler& dis_;
    const Annotations& ann_;
    ListingOptions options_;
    Word from_, to_;

    std::vector<Line> lines_;
    std::set<Word> dataRefs_;
    std::map<std::string, Word> usedFirmware_;
    std::map<std::string, Word> usedPorts_;
    std::optional<Word> port_;        // port number currently held in BC (from LD BC,nn / LD B,n)
    bool portLowKnown_ = false;       // low byte (= value of C) is known
    std::optional<Byte> knownA_;      // A after an immediate load
    std::optional<Word> knownHl_;     // HL after an immediate load
    bool jpAtVector_ = false;         // a JP opcode (&C3) was just stored at &0038
    std::size_t userComments_ = 0;
    void resetContext() {
        port_.reset();
        knownA_.reset();
        knownHl_.reset();
        jpAtVector_ = false;
    }
    std::size_t instructions_ = 0;
    std::size_t commented_ = 0;

public:
    ListingResult result_;
};

std::string Builder::wordText(Word value) {
    if (auto e = ann_.expressionFor(value)) return *e;
    if (inRange(value)) {
        if (auto n = autoName(value)) return *n;
    }
    if (options_.platformSymbols) {
        if (auto fw = cpc::firmwareName(value)) {
            usedFirmware_.emplace(*fw, value);
            return *fw;
        }
    }
    return num4(value);
}

std::string Builder::textRow(const std::vector<Byte>& bytes) {
    std::string out;
    std::string current;
    auto flush = [&] {
        if (!current.empty()) {
            out += (out.empty() ? "" : ",") + std::string("\"") + current + "\"";
            current.clear();
        }
    };
    for (Byte b : bytes) {
        if (isPrintableAscii(b) && b != '"') {
            current += static_cast<char>(b);
            continue;
        }
        flush();
        const unsigned low = b & 0x7Fu;
        if (b >= 0xA0 && isPrintableAscii(low) && low != '"' && low != '\'' && low != ';') {
            out += (out.empty() ? "" : ",") + std::string("'") + static_cast<char>(low) + "'+&80";
        } else {
            out += (out.empty() ? "" : ",") + num2(b);
        }
    }
    flush();
    return out;
}

std::vector<std::string> Builder::headerFor(Word a) {
    std::vector<std::string> out;
    if (const std::string* block = ann_.blockCommentAt(a)) {
        out.push_back("");
        for (const auto& l : wrapText(*block, 76)) out.push_back("; " + l);
        ++commented_;
        ++userComments_;
    }
    const RoutineDoc* doc = ann_.routineAt(a);
    const bool isCall = dis_.callers().count(a) != 0;
    if (doc || isCall) {
        out.push_back("");
        out.push_back("; " + std::string(75, '-'));
        const std::string title = doc ? doc->name
                                      : (ann_.nameAt(a) ? *ann_.nameAt(a) + " (not yet documented)" : "sub_" + h4(a) + " (not yet documented)");
        out.push_back("; " + title);
        if (doc) {
            for (const auto& l : wrapText(doc->summary, 74)) {
                if (!l.empty()) out.push_back(";   " + l);
            }
            auto field = [&](const char* label, const std::string& value) {
                if (value.empty()) return;
                const auto wrapped = wrapText(value, 60);
                for (std::size_t i = 0; i < wrapped.size(); ++i) out.push_back(";   " + padTo(i == 0 ? label : "", 10) + wrapped[i]);
            };
            field("in:", doc->inputs);
            field("out:", doc->outputs);
            field("clobbers:", doc->clobbers);
            if (!doc->confidence.empty() && doc->confidence != "high") {
                out.push_back(";   confidence: " + doc->confidence + (doc->confidence == "low" ? " - a guess, please verify" : " - not fully verified"));
            }
            ++commented_;
        }
        if (options_.showXrefs && isCall) {
            std::string refs;
            int n = 0;
            for (Word c : dis_.callers().at(a)) {
                if (n++ == 8) {
                    refs += " ...";
                    break;
                }
                refs += (refs.empty() ? "" : ", ") + h4(c);
            }
            out.push_back(";   called from: " + refs);
        }
        out.push_back("; " + std::string(75, '-'));
    }
    return out;
}

namespace {

bool isPortOut(const Instruction& i) { return i.valid && i.mnemonic == "OUT" && i.operands.rfind("(C),", 0) == 0; }
bool isPortIn(const Instruction& i) {
    return i.valid && i.mnemonic == "IN" && i.operands.size() > 4 && i.operands.compare(i.operands.size() - 4, 4, ",(C)") == 0;
}

/// True if the instruction reads or writes register A (explicitly or implicitly).
bool involvesA(const Instruction& i) {
    static const std::set<std::string> implicitA = {"SUB", "AND", "XOR", "OR", "CP", "RLA", "RRA", "RLCA", "RRCA", "DAA", "CPL", "NEG", "RLD", "RRD", "POP", "EX", "EXX", "LDIR", "LDDR"};
    if (implicitA.count(i.mnemonic)) return true;
    std::string token;
    for (char c : i.operands + ",") {
        if (c == ',' || c == '(' || c == ')' || c == '+' || c == '-') {
            if (token == "A" || token == "AF") return true;
            token.clear();
        } else {
            token += c;
        }
    }
    return false;
}

/// True if the instruction reads or writes the register pair `pair` ("HL", "DE", ...) explicitly or implicitly.
bool involvesPair(const Instruction& i, const std::string& pair) {
    static const std::set<std::string> blockOps = {"LDI", "LDD", "LDIR", "LDDR", "CPI", "CPD", "CPIR", "CPDR", "INI", "IND", "INIR", "INDR",
                                                   "OUTI", "OUTD", "OTIR", "OTDR", "EXX", "POP", "RLD", "RRD"};
    if (pair == "HL" && blockOps.count(i.mnemonic) && i.mnemonic != "POP") return true;
    if (pair == "DE" && (i.mnemonic == "LDI" || i.mnemonic == "LDD" || i.mnemonic == "LDIR" || i.mnemonic == "LDDR" || i.mnemonic == "EXX")) return true;
    std::string token;
    for (char c : i.operands + ",") {
        if (c == ',' || c == '(' || c == ')' || c == '+' || c == '-') {
            if (token == pair || (pair == "HL" && (token == "H" || token == "L"))) return true;
            token.clear();
        } else {
            token += c;
        }
    }
    return false;
}

/// True if the instruction changes B or C (so a port number held in BC is no longer known).
bool changesBc(const Instruction& i) {
    const std::string& o = i.operands;
    if (i.mnemonic == "DJNZ" || i.mnemonic == "EXX" || i.mnemonic == "LDIR" || i.mnemonic == "LDDR") return true;
    if (i.mnemonic == "POP" || i.mnemonic == "INC" || i.mnemonic == "DEC") return o == "B" || o == "C" || o == "BC";
    if (i.mnemonic == "LD" || i.mnemonic == "IN") return o.rfind("B,", 0) == 0 || o.rfind("C,", 0) == 0 || o.rfind("BC,", 0) == 0;
    return false;
}

}  // namespace

void Builder::codeLine(Word a, Line& line) {
    DecodeOptions options;
    options.style = NumberStyle::Ampersand;
    options.symbolizer = [this](Word t) { return branchSymbol(t); };
    options.operandSymbolizer = [this](Word v, bool memory) { return operandSymbol(v, memory); };
    const Instruction ins = decode(ram_, a, options);

    // Is a port access (OUT (C),r / IN r,(C)) coming up before B/C are changed again?
    auto portAccessAhead = [&] {
        Word next = ins.next();
        for (int k = 0; k < 4; ++k) {
            const Instruction i = decode(ram_, next, {});
            if (isPortOut(i) || isPortIn(i)) return true;
            if (!i.valid || i.flow != FlowKind::Sequential || changesBc(i)) return false;
            next = i.next();
        }
        return false;
    };

    std::string operands = ins.operands;
    std::string autoComment;

    if (options_.platformSymbols && ins.mnemonic == "LD") {
        if (ins.length == 3 && ins.operands.rfind("BC,", 0) == 0 && portAccessAhead()) {
            const Word value = static_cast<Word>(ins.bytes[1] | (ins.bytes[2] << 8));
            if (auto expr = cpc::portExpression(value)) {
                operands = "BC," + *expr;
                for (const auto& p : cpc::portSymbols()) {
                    if (expr->rfind(p.name, 0) == 0) usedPorts_.emplace(p.name, p.address);
                }
                port_ = value;
                portLowKnown_ = true;
            }
        } else if (ins.length == 2 && ins.operands.rfind("B,", 0) == 0 && portAccessAhead()) {
            if (auto expr = cpc::portHighExpression(ins.bytes[1])) {
                operands = "B," + *expr;
                const std::string name = expr->substr(5, expr->size() - 6);  // HIGH(NAME)
                for (const auto& p : cpc::portSymbols()) {
                    if (name == p.name) usedPorts_.emplace(p.name, p.address);
                }
                port_ = static_cast<Word>(ins.bytes[1] << 8);
                portLowKnown_ = false;
            }
        }
    }

    if (options_.hardwareComments && port_) {
        if (isPortOut(ins)) {
            const std::string reg = ins.operands.substr(4);
            std::optional<Byte> value;
            if (reg == "C" && portLowKnown_) value = static_cast<Byte>(*port_ & 0xFF);
            else if (reg == "A" && knownA_) value = knownA_;
            if (value) {
                autoComment = cpc::describePortWrite(*port_, *value);
            } else {
                const std::string what = cpc::describePortWrite(*port_, 0);
                const auto colon = what.find(':');
                const std::string device = what.empty() ? "" : (colon == std::string::npos ? "ROM select" : what.substr(0, colon));
                if (!device.empty()) autoComment = device + ": write " + reg;
            }
        } else if (isPortIn(ins)) {
            autoComment = cpc::describePortRead(*port_);
        }
    }

    // well-known idioms: say what they mean, because they are easy to misread
    if (options_.hardwareComments && autoComment.empty()) {
        const Word nn = ins.length == 3 ? static_cast<Word>(ins.bytes[1] | (ins.bytes[2] << 8)) : Word{0};
        if (ins.length == 3 && ins.bytes[0] == 0x31) {
            autoComment = "stack pointer: the stack starts at &" + h4(nn) + " and grows DOWNWARDS (it uses the RAM below &" + h4(nn) + ")";
        } else if (ins.mnemonic == "HALT") {
            autoComment = "waits for the next interrupt (a CPC raises one every 3.3 ms)";
        } else if (ins.mnemonic == "IM" && ins.operands == "1") {
            autoComment = "interrupt mode 1: every interrupt jumps to &0038";
        } else if (ins.mnemonic == "IM" && ins.operands == "2") {
            autoComment = "interrupt mode 2: vector table at I*256 + data byte";
        } else if (ins.length == 3 && ins.bytes[0] == 0x32 && nn < 0x0040) {  // LD (nn),A into the RST area
            if (nn == 0x0038 && knownA_ && *knownA_ == 0xC3) {
                autoComment = "stores the JP opcode (&C3) at &0038, the interrupt entry (IM 1): installs an interrupt handler";
                jpAtVector_ = true;
            } else {
                autoComment = "writes into the RST / interrupt vector area &0000-&003F";
            }
        } else if (ins.length == 3 && ins.bytes[0] == 0x22 && nn == 0x0039 && jpAtVector_ && knownHl_) {
            autoComment = "target of that JP: the interrupt handler is at &" + h4(*knownHl_);
        } else if (ins.length == 3 && ins.bytes[0] == 0x22 && nn < 0x0040) {
            autoComment = "writes into the RST / interrupt vector area &0000-&003F";
        }
    }

    // keep the tracked context up to date for the next instruction
    if (ins.mnemonic == "LD" && ins.length == 2 && ins.operands.rfind("A,", 0) == 0 && ins.operands.find('(') == std::string::npos) {
        knownA_ = ins.bytes[1];
    } else if (involvesA(ins) && !isPortOut(ins)) {
        knownA_.reset();
    }
    if (ins.mnemonic == "LD" && ins.length == 3 && ins.bytes[0] == 0x21) {
        knownHl_ = static_cast<Word>(ins.bytes[1] | (ins.bytes[2] << 8));
    } else if (involvesPair(ins, "HL") && !(ins.length == 3 && ins.bytes[0] == 0x22)) {
        knownHl_.reset();
    }
    if (!(ins.length == 3 && ins.bytes[0] == 0x22) && !(ins.length == 3 && ins.bytes[0] == 0x32) && !(ins.mnemonic == "LD" && ins.length == 3 && ins.bytes[0] == 0x21))
        jpAtVector_ = jpAtVector_ && (ins.mnemonic == "LD");  // only loads may sit between the two stores
    if (ins.flow != FlowKind::Sequential || !ins.valid) resetContext();
    if (changesBc(ins) && !(ins.mnemonic == "LD" && (operands.rfind("BC,", 0) == 0 || operands.rfind("B,", 0) == 0) && port_)) {
        port_.reset();
    }

    if (options_.hardwareComments && autoComment.empty() && ins.mnemonic == "LD" && ins.length == 3) {
        static const std::set<Byte> withAddress = {0x01, 0x11, 0x21, 0x31, 0x22, 0x2A, 0x32, 0x3A};
        if (withAddress.count(ins.bytes[0])) {
            const std::string where = cpc::describeScreenAddress(static_cast<Word>(ins.bytes[1] | (ins.bytes[2] << 8)));
            if (!where.empty()) autoComment = "screen at &C000: " + where;
        }
    }
    line.length = ins.length;
    line.code = operands.empty() ? ins.mnemonic : ins.mnemonic + " " + operands;
    if (const std::string* c = ann_.commentAt(a)) {
        line.comment = *c;
        ++commented_;
        ++userComments_;
    } else if (!autoComment.empty()) {
        line.comment = autoComment;
        ++commented_;
    }
    ++instructions_;
}

void Builder::dataRegionLines(const DataRegion& region, std::size_t& index) {
    const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(region.address) + region.length, static_cast<std::size_t>(to_) + 1);
    std::size_t a = region.address;
    const std::size_t perRow = region.kind == DataKind::Words || region.kind == DataKind::Pointers ? 4 : (region.kind == DataKind::Text ? 24 : 8);
    const std::size_t unit = region.kind == DataKind::Words || region.kind == DataKind::Pointers ? 2 : 1;
    bool first = true;
    while (a < end) {
        Line line;
        line.address = static_cast<Word>(a);
        if (first) {
            line.before = headerFor(line.address);
            if (!region.comment.empty()) {
                for (const auto& l : wrapText(region.comment, 74)) line.before.push_back("; " + l);
                ++commented_;
            }
            first = false;
        }
        if (auto label = labelAt(line.address)) line.before.push_back(*label + ":");
        const std::size_t n = std::min(perRow * unit, end - a);
        std::vector<Byte> bytes;
        for (std::size_t i = 0; i < n; ++i) bytes.push_back(ram_.readRam(static_cast<Word>(a + i)));
        if (unit == 2 && n % 2 == 0) {
            std::string text = "DW ";
            for (std::size_t i = 0; i < n; i += 2) {
                const Word v = static_cast<Word>(bytes[i] | (bytes[i + 1] << 8));
                text += (i ? "," : "") + (region.kind == DataKind::Pointers ? wordText(v) : num4(v));
            }
            line.code = text;
        } else if (region.kind == DataKind::Text) {
            line.code = "DB " + textRow(bytes);
        } else {
            std::string text = "DB ";
            for (std::size_t i = 0; i < n; ++i) text += (i ? "," : "") + num2(bytes[i]);
            line.code = text;
        }
        line.length = n;
        if (const std::string* c = ann_.commentAt(line.address)) line.comment = *c;
        lines_.push_back(std::move(line));
        a += n;
    }
    index = a;
}

void Builder::rawDataLines(Word start, Word limit, std::size_t& consumed) {
    // Longest run of identical bytes from `start` that contains no boundary.
    auto runLength = [&](std::size_t a) {
        if (a > limit) return std::size_t{0};
        const Byte v = ram_.readRam(static_cast<Word>(a));
        std::size_t n = 1;
        while (a + n <= limit && ram_.readRam(static_cast<Word>(a + n)) == v && !hasBoundary(static_cast<Word>(a + n))) ++n;
        return n;
    };
    Line line;
    line.address = start;
    line.before = headerFor(start);
    if (auto label = labelAt(start)) line.before.push_back(*label + ":");
    if (const std::string* c = ann_.commentAt(start)) line.comment = *c;

    if (const std::size_t run = runLength(start); run >= 16) {
        line.code = "DS " + std::to_string(run) + "," + num2(ram_.readRam(start));
        line.length = run;
    } else {
        std::vector<Byte> bytes;
        std::size_t a = start;
        while (a <= limit && bytes.size() < 8 && (bytes.empty() || !hasBoundary(static_cast<Word>(a))) && (bytes.empty() || runLength(a) < 16)) {
            bytes.push_back(ram_.readRam(static_cast<Word>(a)));
            ++a;
        }
        std::string text = "DB ";
        for (std::size_t i = 0; i < bytes.size(); ++i) text += (i ? "," : "") + num2(bytes[i]);
        line.code = text;
        line.length = bytes.size();
    }
    consumed = start + line.length;
    lines_.push_back(std::move(line));
}

ListingResult Builder::build() {
    collectDataRefs();
    // ---- pass 1: lines ----
    for (std::size_t a = from_; a <= to_;) {
        const auto address = static_cast<Word>(a);
        if (const DataRegion* region = ann_.dataAt(address)) {
            bool overlapsCode = false;
            for (std::size_t k = a; k < std::min<std::size_t>(a + region->length, static_cast<std::size_t>(to_) + 1); ++k) {
                if (dis_.isInstructionStart(static_cast<Word>(k))) overlapsCode = true;
            }
            if (!overlapsCode) {
                dataRegionLines(*region, a);
                continue;
            }
        }
        if (dis_.isInstructionStart(address)) {
            const Instruction probe = decode(ram_, address, {});
            if (a + probe.length - 1 <= to_) {
                Line line;
                line.address = address;
                line.before = headerFor(address);
                if (auto label = labelAt(address)) {
                    line.before.push_back(*label + ":");
                    resetContext();
                }
                codeLine(address, line);
                lines_.push_back(std::move(line));
                a += lines_.back().length;
                continue;
            }
        }
        std::size_t next = a;
        rawDataLines(address, to_, next);
        a = next;
    }

    // ---- names that cannot be attached to a line start become EQUs ----
    std::set<Word> lineStarts;
    for (const auto& l : lines_) lineStarts.insert(l.address);
    std::map<std::string, std::pair<Word, std::string>> extraEqus;  // name -> (value, comment)
    for (const auto& [addr, name] : ann_.labels()) {
        if (!inRange(addr) || lineStarts.count(addr) == 0) {
            std::string comment;
            for (const auto& v : ann_.variables()) {
                if (v.address == addr) comment = v.comment + (v.size > 1 ? " (" + std::to_string(v.size) + " bytes)" : "");
            }
            extraEqus.emplace(name, std::make_pair(addr, comment));
        }
    }
    for (const auto& l : lines_) {
        // auto names that are referenced but sit in the middle of something
        (void)l;
    }
    std::set<Word> autoNeeded(dataRefs_);
    for (const auto& [t, refs] : dis_.callers()) (void)refs, autoNeeded.insert(t);
    for (const auto& [t, refs] : dis_.jumpers()) (void)refs, autoNeeded.insert(t);
    std::size_t autoCount = 0;
    for (Word t : autoNeeded) {
        if (!inRange(t) || ann_.nameAt(t)) continue;
        if (lineStarts.count(t) == 0) extraEqus.emplace(*autoName(t), std::make_pair(t, std::string("branch target inside an instruction or data row")));
        else ++autoCount;
    }

    // ---- output ----
    std::ostringstream out;
    const std::string title = ann_.title().empty() ? "Annotated Z80 listing" : ann_.title();
    out << "; " << std::string(75, '=') << "\n";
    out << "; " << title << "\n";
    out << "; Range &" << h4(from_) << "-&" << h4(to_) << "   (generated by z80tool; re-assemblable)\n";
    out << "; " << std::string(75, '=') << "\n";
    if (!usedFirmware_.empty()) {
        out << "\n; --- Amstrad CPC firmware entry points used ---\n";
        std::map<Word, std::string> byAddr;
        for (const auto& [n, a] : usedFirmware_) byAddr[a] = n;
        for (const auto& [a, n] : byAddr) out << padTo(n, 24) << "EQU " << num4(a) << "\n";
    }
    if (!usedPorts_.empty()) {
        out << "\n; --- Hardware ports used ---\n";
        for (const auto& p : cpc::portSymbols()) {
            if (usedPorts_.count(p.name)) out << padTo(p.name, 24) << "EQU " << num4(p.address) << "   ; " << p.description << "\n";
        }
    }
    if (!extraEqus.empty()) {
        out << "\n; --- Names outside the listing (variables, tables) ---\n";
        std::vector<std::pair<Word, std::string>> ordered;
        std::map<std::string, std::pair<Word, std::string>> byName = extraEqus;
        for (const auto& [name, v] : byName) ordered.emplace_back(v.first, name);
        std::sort(ordered.begin(), ordered.end());
        for (const auto& [addr, name] : ordered) {
            const std::string& comment = byName[name].second;
            out << padTo(name, 24) << "EQU " << num4(addr) << (comment.empty() ? "" : "   ; " + comment) << "\n";
        }
    }
    out << "\n        ORG " << num4(from_) << "\n";

    for (const auto& l : lines_) {
        for (const auto& b : l.before) out << b << "\n";
        std::string code = "        " + l.code;
        if (!l.comment.empty()) {
            const auto wrapped = wrapText(l.comment, 60);
            code = padTo(code, 44) + "; " + wrapped[0];
            out << code << "\n";
            for (std::size_t i = 1; i < wrapped.size(); ++i) out << std::string(44, ' ') << "; " << wrapped[i] << "\n";
        } else {
            out << code << "\n";
        }
    }

    result_.text = out.str();
    result_.from = from_;
    result_.to = to_;
    result_.instructions = instructions_;
    result_.autoLabels = autoCount;
    result_.namedLabels = 0;
    for (const auto& [addr, name] : ann_.labels()) {
        (void)name;
        if (inRange(addr)) ++result_.namedLabels;
    }
    result_.commentedLines = commented_;
    result_.userComments = userComments_;
    return result_;
}

}  // namespace

ListingResult renderListing(const Memory& memory, const Disassembler& dis, const Annotations& ann, const ListingOptions& options) {
    Memory ram;
    ram.load(0, memory.bytes());  // plain RAM, no ROM overlay
    if (!dis.hasResult() && !(options.from && options.to)) {
        ListingResult r;
        r.text = "; nothing analysed yet\n";
        return r;
    }
    const Word from = options.from.value_or(dis.lowestAddress());
    Word to = options.to.value_or(dis.highestAddress());
    if (!options.to) {
        // annotated data that follows the code closely belongs to the listing
        for (bool grown = true; grown;) {
            grown = false;
            for (const auto& [a, region] : ann.dataRegions()) {
                const unsigned end = std::min<unsigned>(0xFFFFu, static_cast<unsigned>(a) + region.length - 1);
                if (a > to && a <= static_cast<unsigned>(to) + 0x40 && end > to) {
                    to = static_cast<Word>(end);
                    grown = true;
                }
            }
        }
    }
    if (from > to) {
        ListingResult r;
        r.text = "; empty range\n";
        return r;
    }
    Builder builder(ram, dis, ann, options, from, to);
    ListingResult result = builder.build();

    if (options.verify) {
        AssemblerOptions assemblerOptions;
        const AssemblyResult assembled = Assembler(assemblerOptions).assemble(result.text);
        if (!assembled.ok()) {
            result.verifyDetail = "listing does not assemble: " + assembled.diagnostics.front().message + " (line " + std::to_string(assembled.diagnostics.front().line) + ")";
        } else {
            const auto bytes = assembled.flatten();
            const std::size_t expected = static_cast<std::size_t>(to) - from + 1;
            if (assembled.empty() || assembled.lowAddress() != from || bytes.size() != expected) {
                result.verifyDetail = "assembled size differs: " + std::to_string(bytes.size()) + " bytes instead of " + std::to_string(expected);
            } else {
                result.verified = true;
                for (std::size_t i = 0; i < expected; ++i) {
                    if (bytes[i] != ram.readRam(static_cast<Word>(from + i))) {
                        result.verified = false;
                        result.verifyDetail = "first difference at &" + h4(static_cast<unsigned>(from + i));
                        break;
                    }
                }
            }
        }
    }
    return result;
}

}  // namespace z80
