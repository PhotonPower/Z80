#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "z80/disassembler.hpp"
#include "z80/instruction.hpp"
#include "z80/memory.hpp"

namespace z80 {

/// A 16-bit operand of an instruction that may be an address of data: `LD HL,nn` (immediate) or the `(nn)` forms of LD.
struct DataOperand {
    Word value = 0;
    bool memory = false;  ///< `(nn)`: the instruction reads or writes memory at nn; false: nn is loaded into a register
    unsigned size = 0;    ///< memory operands: 1 (LD A,(nn) / LD (nn),A) or 2 (LD HL,(nn), LD (nn),DE, ...); 0 for immediates
};

/// The data operand of the instruction at `address`, if it has one.
std::optional<DataOperand> dataOperandAt(const Memory& memory, Word address);

/// A table of code addresses reached through an indirect jump.
struct JumpTable {
    enum class Kind {
        JumpRow,   ///< a row of `JP nn` instructions that a computed `JP (HL)` jumps into: every entry is code
        Pointers   ///< a row of 16-bit addresses of code (DW), read by the dispatch code or marked by the user
    };
    Kind kind = Kind::Pointers;
    std::optional<Word> jump;      ///< the JP (HL) / JP (IX) / JP (IY) / PUSH+RET that dispatches (none: found by scanning or annotation)
    Word table = 0;                ///< first byte of the table
    unsigned bytes = 0;            ///< size in bytes
    bool followed = true;          ///< the targets are analysed as code; false: only a suggestion (mark it as `pointers` to follow it)
    std::vector<Word> slots;       ///< address of each entry
    std::vector<Word> targets;     ///< code address each entry leads to
    std::string how;               ///< the evidence, for the reader
};

/// A guess about the type of bytes that are neither code nor annotated.
struct DataGuess {
    enum class Kind {
        Text,      ///< a string (printable characters; NUL-terminated, or the last character has bit 7 set)
        Zeros,     ///< a run of zero bytes (padding, an empty buffer)
        Pointers,  ///< a table of 16-bit addresses of strings
        Words      ///< 16-bit numbers: a table the code reads word by word, or a variable accessed with 16-bit loads only
    };
    Kind kind = Kind::Text;
    Word start = 0;
    unsigned length = 0;   ///< includes the terminator
    std::string reason;    ///< why this was guessed
    std::string preview;   ///< Text: the first characters
};

/// One subroutine as the code flow shows it.
struct RoutineInfo {
    Word entry = 0;
    unsigned start = 0, end = 0;  ///< hull: lowest address and the address after the highest instruction (end may be 0x10000)
    unsigned instructions = 0;
    unsigned bytes = 0;
    /// The routine's own code as contiguous blocks [first, end). A shared exit far away makes the hull large, the blocks do not.
    std::vector<std::pair<unsigned, unsigned>> blocks;
    std::set<Word> callers;     ///< CALL instructions and jumps from other routines (tail calls) that lead here
    std::set<Word> callees;     ///< entries of routines it calls or tail-calls
    std::set<Word> external;    ///< call targets outside the analysed code (firmware, ROM)
    std::string endsWith = "unknown";  ///< "ret", "jump" or "jump_indirect"
    int depth = 0;              ///< 0 = calls no other routine; else 1 + the deepest callee
    std::optional<Word> viaTable;  ///< a case of this jump table (reached through the table, nobody calls it)
};

/// Everything the analysis needs to know besides the code flow.
struct AnalysisContext {
    unsigned first = 0, last = 0xFFFF;              ///< the program: targets and data guesses stay inside [first, last]
    std::function<bool(Word)> isData;               ///< annotated data regions (never code)
    std::function<bool(Word)> claimed;              ///< bytes that are annotated already (data guesses leave them alone)
    std::vector<std::pair<Word, unsigned>> pointerRegions;  ///< annotated `pointers` regions: tables of code addresses to follow
};

/// Jump tables in the analysed code: rows of JP instructions and tables of addresses behind a dispatch (`LD HL,table` ... `LD E,(HL)` ...
/// `JP (HL)`) are followed; annotated pointer regions are followed; rows of code addresses behind a plain data reference are only suggested.
std::vector<JumpTable> findJumpTables(const Memory& memory, const Disassembler& dis, const AnalysisContext& context);

/// Types of the bytes in [context.first, context.last] that are neither code nor annotated nor table data.
std::vector<DataGuess> guessData(const Memory& memory, const Disassembler& dis, const std::vector<JumpTable>& tables, const AnalysisContext& context);

/// The routines that start at `entries` (call targets, annotated routines, program entries, cases of jump tables). A jump to the entry of
/// another routine is a tail call: the target is a callee, not part of the routine.
std::vector<RoutineInfo> computeRoutines(const Memory& memory, const Disassembler& dis, const std::set<Word>& entries,
                                         const std::vector<JumpTable>& tables);

}  // namespace z80
