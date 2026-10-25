#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "z80/memory.hpp"
#include "z80/types.hpp"

namespace z80 {

struct Diagnostic {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    int line = 0;  ///< 1-based source line
    std::string message;
    std::string source;  ///< the offending source line
};

struct Symbol {
    std::int64_t value = 0;
    int line = 0;       ///< line of definition
    bool isEqu = false;  ///< defined via EQU (a constant) rather than as a label
};

/// A contiguous run of assembled bytes.
struct Segment {
    Word start = 0;
    std::vector<Byte> bytes;
    [[nodiscard]] std::uint32_t end() const { return start + static_cast<std::uint32_t>(bytes.size()); }
};

struct ListingLine {
    int line = 0;
    Word address = 0;
    std::vector<Byte> bytes;
    std::string source;
    bool hasAddress = false;
};

struct AssemblerOptions {
    Word origin = 0x0000;  ///< initial program counter (before the first ORG)
};

struct AssemblyResult {
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, Symbol> symbols;  ///< keys are upper-case
    std::vector<Segment> segments;           ///< in source order, adjacent runs merged
    std::vector<ListingLine> listing;

    [[nodiscard]] std::size_t errorCount() const;
    [[nodiscard]] bool ok() const { return errorCount() == 0; }

    [[nodiscard]] bool empty() const { return segments.empty(); }
    [[nodiscard]] Word lowAddress() const;   ///< first assembled address (0 if empty)
    [[nodiscard]] Word highAddress() const;  ///< last assembled address (0 if empty)
    [[nodiscard]] std::size_t size() const;  ///< number of assembled bytes

    /// One contiguous block from lowAddress() to highAddress(); gaps are filled with `fill`.
    [[nodiscard]] std::vector<Byte> flatten(Byte fill = 0) const;

    /// Writes all segments into `memory`.
    void applyTo(Memory& memory) const;

    /// "file:line: error: message" for every diagnostic.
    [[nodiscard]] std::string formatDiagnostics(const std::string& fileName) const;
    [[nodiscard]] std::string formatListing() const;
    [[nodiscard]] std::string formatSymbols() const;
};

/// Z80 assembler.
///
/// Source format (one statement per line):
///     [label[:]]  [mnemonic [operands]]  [; comment]
/// Pseudo instructions: ORG, EQU, END, DB/DEFB, DW/DEFW, DM/DEFM, DS/DEFS.
/// Labels are case-insensitive; forward references are resolved automatically.
class Assembler {
public:
    explicit Assembler(AssemblerOptions options = {}) : options_(options) {}

    [[nodiscard]] AssemblyResult assemble(std::string_view source) const;

private:
    AssemblerOptions options_;
};

}  // namespace z80
