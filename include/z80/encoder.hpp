#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "z80/expression.hpp"
#include "z80/types.hpp"

namespace z80 {

struct EncodeResult {
    std::vector<Byte> bytes;  ///< empty if the instruction could not be encoded at all
    std::string error;        ///< non-empty on error; bytes may still hold a best-effort encoding
};

using ExpressionCallback = std::function<ExprResult(std::string_view)>;

/// True if `name` (case-insensitive) is a Z80 instruction mnemonic.
bool isMnemonic(std::string_view name);

/// True if `name` (case-insensitive) is a register or condition name that
/// cannot be used as a label.
bool isReservedName(std::string_view name);

/// Splits an operand field at top-level commas (not inside parentheses or quotes).
std::vector<std::string> splitOperands(std::string_view text);

/// Encodes one instruction located at `pc`.
///
/// `operands` are the raw operand strings (see splitOperands). Expressions are
/// evaluated through `eval`, which lets the caller resolve symbols and `$`.
EncodeResult encodeInstruction(std::string_view mnemonic, const std::vector<std::string>& operands,
                               Word pc, const ExpressionCallback& eval);

}  // namespace z80
