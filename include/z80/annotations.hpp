#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "z80/json.hpp"
#include "z80/types.hpp"

namespace z80 {

/// Documentation of a subroutine (shown as a header block above its label).
struct RoutineDoc {
    std::string name;
    std::string summary;   ///< what it does
    std::string inputs;    ///< e.g. "HL = source, DE = screen address"
    std::string outputs;   ///< e.g. "A = number of pixels drawn"
    std::string clobbers;  ///< e.g. "AF BC"
    std::string confidence;  ///< "", "high" (verified by a test), "medium" or "low" (a guess, to be checked)
};

/// A named memory location (a variable, or a table).
struct VariableDoc {
    std::string name;
    Word address = 0;
    unsigned size = 1;       ///< bytes
    std::string comment;
};

enum class DataKind { Bytes, Words, Text, Pointers };
const char* toString(DataKind kind);
std::optional<DataKind> dataKindFromString(const std::string& text);

/// A range that is data, not code, and how to show it.
struct DataRegion {
    Word address = 0;
    unsigned length = 0;
    DataKind kind = DataKind::Bytes;
    std::string name;     ///< optional label at the start
    std::string comment;
};

/// Everything a human or an LLM has learned about a program: names and comments. It contains no
/// code; the listing is always generated from the real bytes, so annotations cannot change what the
/// program does (see renderListing's verification).
class Annotations {
public:
    /// All setters return an error message, or nullopt on success.
    std::optional<std::string> setLabel(Word address, const std::string& name);
    std::optional<std::string> setRoutine(Word address, const RoutineDoc& doc);
    std::optional<std::string> setVariable(const VariableDoc& variable);
    std::optional<std::string> setData(const DataRegion& region);
    void setComment(Word address, const std::string& text) { comments_[address] = text; }
    void setBlockComment(Word address, const std::string& text) { blockComments_[address] = text; }
    void setTitle(const std::string& title) { title_ = title; }

    bool removeLabel(Word address);       ///< also removes a routine/variable name at that address
    bool removeComment(Word address);
    void clear();

    /// Checks a name without registering it: valid identifier, not reserved, not a platform symbol.
    [[nodiscard]] static std::optional<std::string> checkName(const std::string& name);

    // ---- lookups ----
    /// The name given to exactly this address (label, routine or variable), or nullopt.
    [[nodiscard]] std::optional<std::string> nameAt(Word address) const;
    /// "name" or "name+k" if `address` lies inside a named variable/data region of size > 1.
    [[nodiscard]] std::optional<std::string> expressionFor(Word address) const;
    [[nodiscard]] std::optional<Word> addressOf(const std::string& name) const;
    [[nodiscard]] const RoutineDoc* routineAt(Word address) const;
    [[nodiscard]] const std::string* commentAt(Word address) const;
    [[nodiscard]] const std::string* blockCommentAt(Word address) const;
    [[nodiscard]] const DataRegion* dataAt(Word address) const;  ///< region *starting* at address

    [[nodiscard]] const std::map<Word, std::string>& labels() const { return labels_; }
    [[nodiscard]] const std::map<Word, RoutineDoc>& routines() const { return routines_; }
    [[nodiscard]] const std::vector<VariableDoc>& variables() const { return variables_; }
    [[nodiscard]] const std::map<Word, DataRegion>& dataRegions() const { return data_; }
    [[nodiscard]] const std::map<Word, std::string>& comments() const { return comments_; }
    [[nodiscard]] const std::map<Word, std::string>& blockComments() const { return blockComments_; }
    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] Json toJson() const;
    /// Throws JsonError on malformed input or rule violations (duplicate names, ...).
    static Annotations fromJson(const Json& json);

private:
    std::optional<std::string> claimName(Word address, const std::string& name);

    std::map<Word, std::string> labels_;       ///< address -> name (also routine/variable/data names)
    std::map<std::string, Word> byName_;       ///< upper-case name -> address
    std::map<Word, RoutineDoc> routines_;
    std::vector<VariableDoc> variables_;
    std::map<Word, DataRegion> data_;
    std::map<Word, std::string> comments_;
    std::map<Word, std::string> blockComments_;
    std::string title_;
};

}  // namespace z80
