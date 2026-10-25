#include "z80/disassembler.hpp"

#include <iterator>
#include <cctype>
#include <algorithm>
#include <istream>
#include <sstream>

#include "z80/expression.hpp"

namespace z80 {
namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string labelName(Word address) { return "L_" + hexDigits(address, 4); }

std::string padRight(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

}  // namespace

struct Disassembler::Line {
    Word address = 0;
    std::size_t length = 0;
    bool isCode = false;
    bool isRun = false;            // collapsed run of identical bytes: rendered as DS
    Byte runValue = 0;
    Instruction instruction;       // valid if isCode
    std::vector<Byte> dataBytes;   // if !isCode && !isRun
};

namespace {
constexpr std::size_t kMinRunLength = 16;  // identical bytes needed to collapse into DS
}

// ---- configuration --------------------------------------------------------------------

void Disassembler::addComment(Word address, std::string text) {
    comments_[address].push_back(std::move(text));
    if (options_.commentAddressesAreEntryPoints) {
        entryPoints_.insert(address);
    }
}

std::vector<std::string> Disassembler::loadComments(std::istream& in) {
    std::vector<std::string> warnings;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t.front() == ';') continue;

        const auto comma = t.find(',');
        const auto address = comma == std::string::npos ? std::nullopt : parseConstant(trim(t.substr(0, comma)));
        if (!address || *address < 0 || *address > 0xFFFF) {
            warnings.push_back("comment file line " + std::to_string(lineNo) + ": expected 'address,text'");
            continue;
        }
        std::string text = trim(t.substr(comma + 1));
        if (!text.empty() && text.front() == '"') {
            std::string unquoted;
            std::size_t i = 1;
            for (; i < text.size(); ++i) {
                if (text[i] == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        unquoted += '"';
                        ++i;
                    } else {
                        break;
                    }
                } else {
                    unquoted += text[i];
                }
            }
            text = unquoted;
        }
        addComment(static_cast<Word>(*address), text);
    }
    return warnings;
}

// ---- analysis -------------------------------------------------------------------------

void Disassembler::markRange(Word first, std::size_t count, bool start) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = static_cast<Word>(first + i);
        covered_[a] = 1;
        if (start && i == 0) isStart_[a] = 1;
        if (!anyCovered_) {
            lowest_ = highest_ = a;
            anyCovered_ = true;
        } else {
            lowest_ = std::min(lowest_, a);
            highest_ = std::max(highest_, a);
        }
    }
}

std::optional<Instruction> Disassembler::decodeAndMark(Word address, Word& nextAddress) {
    DecodeOptions decodeOptions;
    decodeOptions.style = options_.style;
    Instruction ins = decode(memory_, address, decodeOptions);
    // An instruction that reaches beyond the analysis range would read bytes that are not part
    // of the image; leave such bytes alone.
    if (static_cast<std::uint32_t>(address) + ins.length - 1 > rangeLast_) {
        return std::nullopt;
    }
    markRange(address, ins.length, true);
    nextAddress = ins.next();

    if (ins.target) {
        switch (ins.flow) {
            case FlowKind::Call:
            case FlowKind::ConditionalCall:
                callers_[*ins.target].insert(address);
                break;
            case FlowKind::Jump:
            case FlowKind::ConditionalJump:
                jumpers_[*ins.target].insert(address);
                break;
            default:
                break;
        }
    }

    // Inline data after a CALL (e.g. a "print string" routine followed by its text).
    if ((ins.flow == FlowKind::Call || ins.flow == FlowKind::ConditionalCall) && ins.target) {
        if (auto it = inlineData_.find(*ins.target); it != inlineData_.end()) {
            std::size_t count = 0;
            auto a = static_cast<Word>(nextAddress);
            while (count < 256) {
                const Byte b = memory_.read(a);
                ++count;
                a = static_cast<Word>(a + 1);
                if (it->second == InlineDataKind::StringBit7 && (b & 0x80) != 0) break;
                if (it->second == InlineDataKind::StringZero && b == 0) break;
            }
            markRange(nextAddress, count, false);
            nextAddress = a;
        }
    }
    return ins;
}

bool Disassembler::canAnalyse(Word address) const {
    return address >= rangeFirst_ && address <= rangeLast_;
}

void Disassembler::analyzeRange(Word first, Word last) {
    for (std::uint32_t a = std::max<std::uint32_t>(first, rangeFirst_);
         a <= std::min<std::uint32_t>(last, rangeLast_);) {
        if (covered_[a]) {
            ++a;
            continue;
        }
        Word next = 0;
        if (!decodeAndMark(static_cast<Word>(a), next)) {
            ++a;
            continue;
        }
        const std::uint32_t advance = static_cast<Word>(next - a);
        a += advance == 0 ? 1 : advance;
    }
}

void Disassembler::analyzeFlow(Word entry) {
    // Same priorities as the original tool: pending calls first, then conditional JP, then JR/DJNZ.
    std::vector<Word> calls;
    std::vector<Word> jumps;
    std::vector<Word> relatives;

    auto isZeroRun = [&](Word pc) {
        if (options_.zeroRunLimit == 0) return false;
        for (unsigned i = 0; i < options_.zeroRunLimit; ++i) {
            if (memory_.read(static_cast<Word>(pc + i)) != 0) return false;
        }
        return true;
    };

    auto follow = [&](Word pc) {
        while (!covered_[pc] && canAnalyse(pc)) {
            if (isZeroRun(pc)) return;
            Word next = 0;
            const auto decoded = decodeAndMark(pc, next);
            if (!decoded) return;
            const Instruction& ins = *decoded;
            switch (ins.flow) {
                case FlowKind::Call:
                case FlowKind::ConditionalCall:
                    calls.push_back(*ins.target);
                    pc = next;
                    break;
                case FlowKind::ConditionalJump:
                    (ins.mnemonic == "JP" ? jumps : relatives).push_back(*ins.target);
                    pc = next;
                    break;
                case FlowKind::Jump:
                    pc = *ins.target;
                    break;
                case FlowKind::Return:
                case FlowKind::Restart:
                case FlowKind::JumpIndirect:
                    return;
                case FlowKind::Sequential:
                case FlowKind::ConditionalReturn:
                    pc = next;
                    break;
            }
        }
    };

    auto pop = [](std::vector<Word>& stack, Word& out) {
        if (stack.empty()) return false;
        out = stack.back();
        stack.pop_back();
        return true;
    };

    follow(entry);
    std::set<Word> pendingEntries = entryPoints_;
    for (;;) {
        Word pc = 0;
        if (pop(calls, pc) || pop(jumps, pc) || pop(relatives, pc)) {
            follow(pc);
        } else if (!pendingEntries.empty()) {
            pc = *pendingEntries.rbegin();
            pendingEntries.erase(std::prev(pendingEntries.end()));
            follow(pc);
        } else {
            break;
        }
    }
}

std::size_t Disassembler::instructionCount() const {
    return static_cast<std::size_t>(std::count(isStart_.begin(), isStart_.end(), 1));
}

bool Disassembler::isBranchTarget(Word address) const {
    return callers_.count(address) != 0 || jumpers_.count(address) != 0;
}

/// Number of identical, non-code bytes starting at `address` (stops at labels and comments).
std::size_t Disassembler::runLengthAt(std::uint32_t address, std::uint32_t last) const {
    if (address > last || isStart_[address]) return 0;
    const Byte value = memory_.read(static_cast<Word>(address));
    std::size_t n = 1;
    while (address + n <= last) {
        const auto a = static_cast<Word>(address + n);
        if (isStart_[a] || memory_.read(a) != value || isBranchTarget(a) || comments_.count(a) != 0) break;
        ++n;
    }
    return n;
}

// ---- output ---------------------------------------------------------------------------

std::vector<Disassembler::Line> Disassembler::buildLines(Word from, Word to, bool asmSource) const {
    DecodeOptions decodeOptions;
    decodeOptions.style = options_.style;
    if (asmSource) {
        decodeOptions.symbolizer = [this](Word target) -> std::optional<std::string> {
            if (isBranchTarget(target)) return labelName(target);
            return std::nullopt;
        };
    }

    constexpr std::size_t kBytesPerDataLine = 8;
    std::vector<Line> lines;
    for (std::uint32_t a = from; a <= to;) {
        Line line;
        line.address = static_cast<Word>(a);
        Instruction candidate;
        if (isStart_[a]) candidate = decode(memory_, line.address, decodeOptions);
        if (isStart_[a] && a + candidate.length - 1 <= to) {
            line.isCode = true;
            line.instruction = candidate;
            line.length = candidate.length;
        } else if (isStart_[a]) {
            // The instruction continues beyond the printed range: show the visible bytes as data.
            for (std::uint32_t i = a; i <= to; ++i) line.dataBytes.push_back(memory_.read(static_cast<Word>(i)));
            line.length = line.dataBytes.size();
        } else if (const std::size_t run = runLengthAt(a, to); run >= kMinRunLength) {
            line.isRun = true;
            line.runValue = memory_.read(line.address);
            line.length = run;
        } else {
            std::size_t n = 0;
            while (a + n <= to && n < kBytesPerDataLine && !isStart_[a + n]) {
                const auto address = static_cast<Word>(a + n);
                if (n > 0 && (isBranchTarget(address) || comments_.count(address) != 0)) break;
                if (n > 0 && runLengthAt(a + static_cast<std::uint32_t>(n), to) >= kMinRunLength) break;  // let the run collapse
                line.dataBytes.push_back(memory_.read(address));
                ++n;
            }
            line.length = n;
        }
        a += static_cast<std::uint32_t>(line.length);
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string Disassembler::render(const RenderOptions& options) const {
    std::ostringstream out;
    if (!anyCovered_ && !options.from) {
        out << "; nothing analysed\n";
        return out.str();
    }
    const Word from = options.from.value_or(lowest_);
    const Word to = options.to.value_or(highest_);
    if (from > to) {
        out << "; empty range\n";
        return out.str();
    }

    const NumberStyle style = options_.style;
    const std::vector<Line> lines = buildLines(from, to, options.asmSource);

    auto asciiOf = [](const std::vector<Byte>& bytes) {
        std::string s;
        for (Byte b : bytes) s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
        return s;
    };
    auto commentsAt = [this](Word address) {
        std::string s;
        if (auto it = comments_.find(address); it != comments_.end()) {
            for (const auto& c : it->second) {
                if (!s.empty()) s += " / ";
                s += c;
            }
        }
        return s;
    };
    auto runText = [&](const Line& l) {
        return "DS " + std::to_string(l.length) + "," + formatHex(l.runValue, 2, style);
    };
    auto dataText = [&](const std::vector<Byte>& bytes) {
        std::string s = "DB ";
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i != 0) s += ",";
            s += formatHex(bytes[i], 2, style);
        }
        return s;
    };

    out << "; Z80 disassembly  " << formatHex(from, 4, style) << " - " << formatHex(to, 4, style) << "\n";

    if (options.asmSource) {
        // Labels that do not fall on a line boundary (or outside the range) become EQUs.
        std::set<Word> lineStarts;
        for (const auto& l : lines) lineStarts.insert(l.address);
        std::set<Word> targets;
        for (const auto& [t, _] : callers_) targets.insert(t);
        for (const auto& [t, _] : jumpers_) targets.insert(t);
        for (Word t : targets) {
            if (lineStarts.count(t) == 0) {
                out << padRight(labelName(t), 8) << " EQU " << formatHex(t, 4, style) << "\n";
            }
        }
        out << "        ORG " << formatHex(from, 4, style) << "\n";
    }

    bool previousEndedFlow = false;
    for (const auto& l : lines) {
        const std::string userComment = commentsAt(l.address);
        const bool target = isBranchTarget(l.address);

        if (previousEndedFlow) out << "\n";
        previousEndedFlow = false;

        if (options.asmSource) {
            if (target) out << labelName(l.address) << ":\n";
            std::string text = "        " + (l.isCode ? l.instruction.text() : l.isRun ? runText(l) : dataText(l.dataBytes));
            std::string comment = userComment;
            if (!l.isCode && !l.isRun) {
                comment += (comment.empty() ? "" : "  ") + std::string("|") + asciiOf(l.dataBytes) + "|";
            }
            if (!comment.empty()) text = padRight(text, 32) + " ; " + comment;
            out << text << "\n";
        } else {
            std::string row = hexDigits(l.address, 4) + (target ? "  -> " : "     ");
            if (options.showBytes) {
                std::string bytes;
                if (l.isCode) {
                    for (std::size_t i = 0; i < l.length; ++i) {
                        if (i != 0) bytes += ' ';
                        bytes += hexDigits(l.instruction.bytes[i], 2);
                    }
                }
                row += padRight(bytes, 12) + " ";
            }
            std::string text = l.isCode ? l.instruction.text() : l.isRun ? runText(l) : dataText(l.dataBytes);
            std::string comment = userComment;
            auto add = [&comment](const std::string& part) {
                if (!comment.empty()) comment += "  ";
                comment += part;
            };
            if (l.isCode && l.instruction.target) {
                if (auto c = comments_.find(*l.instruction.target); c != comments_.end()) {
                    add("-> " + c->second.front());
                }
            }
            if (!l.isCode && !l.isRun) add("|" + asciiOf(l.dataBytes) + "|");
            if (options.showXrefs && target) {
                std::set<Word> from_;
                if (auto it = callers_.find(l.address); it != callers_.end()) from_.insert(it->second.begin(), it->second.end());
                if (auto it = jumpers_.find(l.address); it != jumpers_.end()) from_.insert(it->second.begin(), it->second.end());
                std::string refs = "<-";
                std::size_t shown = 0;
                for (Word f : from_) {
                    if (shown++ == 8) {
                        refs += " ...";
                        break;
                    }
                    refs += " " + hexDigits(f, 4);
                }
                add(refs);
            }
            row += comment.empty() ? text : padRight(text, 26) + " ; " + comment;
            out << row << "\n";
        }

        if (l.isCode) {
            const FlowKind f = l.instruction.flow;
            previousEndedFlow = f == FlowKind::Return || f == FlowKind::Jump || f == FlowKind::JumpIndirect;
        }
    }

    if (!options.asmSource) {
        out << "\n; ---- summary ----\n";
        out << "; instructions : " << instructionCount() << "\n";
        out << "; call targets : " << callers_.size() << "\n";
        for (const auto& [t, from_] : callers_) {
            out << ";   " << formatHex(t, 4, style) << "  x" << from_.size() << "\n";
        }
    }
    return out.str();
}

}  // namespace z80
