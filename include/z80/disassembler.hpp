#pragma once

#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "z80/decoder.hpp"
#include "z80/format.hpp"
#include "z80/memory.hpp"

namespace z80 {

/// How the bytes after a CALL to a given routine are to be treated (inline data).
enum class InlineDataKind {
    StringBit7,  ///< characters up to and including the first byte with bit 7 set
    StringZero   ///< characters up to and including a 0 byte
};

struct DisassemblerOptions {
    NumberStyle style = NumberStyle::Ampersand;
    /// Addresses that carry a comment are used as additional entry points in flow analysis
    /// (this is what the original BASIC disassembler did).
    bool commentAddressesAreEntryPoints = true;

    /// Flow analysis does not follow a path into a run of this many 0x00 bytes: that is almost
    /// always uninitialised memory, which would otherwise be "disassembled" as a long NOP sled.
    /// 0 disables the check.
    unsigned zeroRunLimit = 16;
};

struct RenderOptions {
    bool asmSource = false;  ///< emit re-assemblable source (ORG, labels, DB) instead of a listing
    bool showBytes = true;   ///< listing only: show the raw instruction bytes
    bool showXrefs = true;   ///< listing only: show callers of jump/call targets
    std::optional<Word> from;  ///< default: lowest analysed address
    std::optional<Word> to;    ///< default: highest analysed address
};

/// Disassembler with code-flow analysis.
///
/// Typical use:
///     Disassembler d(memory);
///     d.loadComments(file);
///     d.analyzeFlow(0x5CF);          // follow calls and jumps ("chain mode")
///     std::cout << d.render({});
///
/// or `analyzeRange(first, last)` for a linear sweep over an address interval.
class Disassembler {
public:
    explicit Disassembler(const Memory& memory, DisassemblerOptions options = {})
        : memory_(memory), options_(options), covered_(kAddressSpace, 0), isStart_(kAddressSpace, 0) {}

    // ---- configuration ----------------------------------------------------------
    void addComment(Word address, std::string text);
    void addEntryPoint(Word address) { entryPoints_.insert(address); }
    void addInlineData(Word callTarget, InlineDataKind kind) { inlineData_[callTarget] = kind; }

    /// Restricts analysis to [first, last]: code outside is never decoded (e.g. the extent of a
    /// raw binary that was loaded into an otherwise empty address space). Default: everything.
    void setAnalysisRange(Word first, Word last) {
        rangeFirst_ = first;
        rangeLast_ = last;
    }

    /// Reads "address,text" lines (see docs/formats.md). Returns warnings for bad lines.
    std::vector<std::string> loadComments(std::istream& in);

    // ---- analysis ---------------------------------------------------------------
    /// Linear sweep over [first, last].
    void analyzeRange(Word first, Word last);

    /// Follows the code flow from `entry`: CALL targets, conditional jumps, comment
    /// addresses and extra entry points are visited; analysis of a path ends at RET,
    /// RST, JP (HL) or when it reaches code that was already seen.
    void analyzeFlow(Word entry);

    // ---- queries ----------------------------------------------------------------
    [[nodiscard]] bool isInstructionStart(Word address) const { return isStart_[address] != 0; }
    [[nodiscard]] bool isCovered(Word address) const { return covered_[address] != 0; }
    [[nodiscard]] bool hasResult() const { return anyCovered_; }
    [[nodiscard]] Word lowestAddress() const { return lowest_; }
    [[nodiscard]] Word highestAddress() const { return highest_; }
    [[nodiscard]] std::size_t instructionCount() const;
    [[nodiscard]] const std::map<Word, std::set<Word>>& callers() const { return callers_; }
    [[nodiscard]] const std::map<Word, std::set<Word>>& jumpers() const { return jumpers_; }

    // ---- output -----------------------------------------------------------------
    [[nodiscard]] std::string render(const RenderOptions& options = {}) const;

private:
    struct Line;

    std::optional<Instruction> decodeAndMark(Word address, Word& nextAddress);
    void markRange(Word first, std::size_t count, bool start);
    [[nodiscard]] bool isBranchTarget(Word address) const;
    [[nodiscard]] bool canAnalyse(Word address) const;
    [[nodiscard]] std::size_t runLengthAt(std::uint32_t address, std::uint32_t last) const;
    [[nodiscard]] std::vector<Line> buildLines(Word from, Word to, bool asmSource) const;

    const Memory& memory_;
    DisassemblerOptions options_;
    std::vector<std::uint8_t> covered_;  // byte belongs to analysed code/inline data
    std::vector<std::uint8_t> isStart_;  // byte starts an instruction
    Word rangeFirst_ = 0x0000;
    Word rangeLast_ = 0xFFFF;
    bool anyCovered_ = false;
    Word lowest_ = 0xFFFF;
    Word highest_ = 0;

    std::map<Word, std::vector<std::string>> comments_;
    std::set<Word> entryPoints_;
    std::map<Word, InlineDataKind> inlineData_;
    std::map<Word, std::set<Word>> callers_;   // call target -> addresses of CALL instructions
    std::map<Word, std::set<Word>> jumpers_;   // jump target -> addresses of JP/JR/DJNZ instructions
};

}  // namespace z80
