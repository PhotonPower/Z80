#pragma once

#include <string>

namespace z80 {

/// How numbers are written in disassembler output.
enum class NumberStyle {
    Ampersand,  ///< &1F   (Amstrad/BASIC style, default; matches the original tools)
    Dollar,     ///< $1F
    CPrefix,    ///< 0x1F
    HSuffix     ///< 1FH (a leading 0 is added if the first digit is a letter)
};

/// Upper-case hexadecimal digits without any prefix, zero-padded to `digits`.
std::string hexDigits(unsigned value, int digits);

/// Hexadecimal number in the requested style, zero-padded to `digits`.
std::string formatHex(unsigned value, int digits, NumberStyle style);

/// Parses "amp", "dollar", "c", "h" (case-insensitive). Returns false on unknown names.
bool parseNumberStyle(const std::string& name, NumberStyle& out);

}  // namespace z80
