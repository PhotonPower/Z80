#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace z80 {

struct ExprResult {
    std::int64_t value = 0;
    bool ok = true;                           ///< false on syntax errors
    std::string error;                        ///< set if !ok
    std::vector<std::string> undefinedNames;  ///< symbols the lookup could not resolve (value 0 used)
};

/// Evaluates assembler expressions.
///
/// Literals:   123  &1F  &H1F  $1F  #1F  0x1F  1Fh (leading digit)  %1010  0b1010  1010b  'A'
/// Operators:  + - * / %  << >>  &  ^  |  ~  unary + -   and  ( )   HIGH(x)  LOW(x)
/// `$` on its own is the current program counter.
/// Symbol names are upper-cased before they are passed to the lookup.
class ExpressionEvaluator {
public:
    using Lookup = std::function<std::optional<std::int64_t>(const std::string& upperName)>;

    /// `hexDefault`: numbers without prefix or suffix are hexadecimal (monitor convention), and a
    /// name that is not known to `lookup` but consists only of hex digits (e.g. "BEEF") is a number.
    /// Known symbols always win over such an interpretation.
    explicit ExpressionEvaluator(Lookup lookup = {}, std::int64_t pc = 0, bool hexDefault = false)
        : lookup_(std::move(lookup)), pc_(pc), hexDefault_(hexDefault) {}

    [[nodiscard]] ExprResult evaluate(std::string_view text) const;

private:
    Lookup lookup_;
    std::int64_t pc_;
    bool hexDefault_;
};

/// Convenience: evaluates a constant expression without symbols.
std::optional<std::int64_t> parseConstant(std::string_view text);

}  // namespace z80
