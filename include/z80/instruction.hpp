#pragma once

#include <array>
#include <optional>
#include <string>

#include "z80/types.hpp"

namespace z80 {

/// How an instruction affects control flow. Used by the disassembler to follow code.
enum class FlowKind {
    Sequential,         ///< continues with the next instruction
    Jump,               ///< unconditional JP / JR (target known)
    ConditionalJump,    ///< JP cc / JR cc / DJNZ
    JumpIndirect,       ///< JP (HL) / JP (IX) / JP (IY): target unknown
    Call,               ///< CALL nn
    ConditionalCall,    ///< CALL cc,nn
    Return,             ///< RET / RETI / RETN
    ConditionalReturn,  ///< RET cc
    Restart             ///< RST n (often followed by inline data)
};

struct Instruction {
    Word address = 0;
    std::array<Byte, 4> bytes{};
    std::uint8_t length = 0;

    std::string mnemonic;  ///< e.g. "LD"
    std::string operands;  ///< e.g. "A,(IX+&05)"; empty if none

    FlowKind flow = FlowKind::Sequential;
    std::optional<Word> target;  ///< branch target for JP/JR/DJNZ/CALL/RST

    /// false if the bytes do not form a (documented) instruction; then the
    /// instruction is rendered as DB.
    bool valid = true;

    [[nodiscard]] std::string text() const {
        return operands.empty() ? mnemonic : mnemonic + " " + operands;
    }
    [[nodiscard]] Word next() const { return static_cast<Word>(address + length); }
};

}  // namespace z80
