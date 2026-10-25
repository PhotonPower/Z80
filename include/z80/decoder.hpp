#pragma once

#include <functional>
#include <optional>
#include <string>

#include "z80/format.hpp"
#include "z80/instruction.hpp"
#include "z80/memory.hpp"

namespace z80 {

/// Returns a symbolic name for a branch target, or nullopt to print the number.
using Symbolizer = std::function<std::optional<std::string>(Word address)>;

/// Symbolic form of a 16-bit operand that is not a branch target: `memoryOperand` is true for
/// "(nn)" (LD A,(nn) ...) and false for an immediate 16-bit value (LD HL,nn).
using OperandSymbolizer = std::function<std::optional<std::string>(Word value, bool memoryOperand)>;

struct DecodeOptions {
    NumberStyle style = NumberStyle::Ampersand;
    Symbolizer symbolizer;                  ///< optional; used for JP/JR/DJNZ/CALL targets
    OperandSymbolizer operandSymbolizer;    ///< optional; used for LD rr,nn and the (nn) forms of LD
};

/// Decodes the single instruction at `address`.
///
/// Only documented Z80 instructions are decoded. Everything else (undocumented
/// opcodes, stray prefixes) yields a one- to four-byte `DB` with valid == false.
Instruction decode(const Memory& memory, Word address, const DecodeOptions& options = {});

}  // namespace z80
