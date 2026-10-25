#pragma once

#include <optional>
#include <string>

#include "z80/annotations.hpp"
#include "z80/disassembler.hpp"
#include "z80/memory.hpp"

namespace z80 {

struct ListingOptions {
    std::optional<Word> from;  ///< default: lowest analysed address
    std::optional<Word> to;    ///< default: highest analysed address
    bool verify = true;        ///< re-assemble the listing and compare with the memory image
    bool showXrefs = true;     ///< "called from" lists
    bool hardwareComments = true;  ///< explain OUT (C),r to Gate Array / CRTC / PPI
    bool platformSymbols = true;   ///< show CPC firmware calls and hardware ports by name
};

struct ListingResult {
    std::string text;            ///< assembler source with labels, comments and EQUs
    bool verified = false;       ///< re-assembled bytes equal the memory image
    std::string verifyDetail;    ///< why verification failed (empty if verified or not requested)
    Word from = 0;
    Word to = 0;
    std::size_t instructions = 0;
    std::size_t namedLabels = 0;     ///< labels given by the user/LLM
    std::size_t autoLabels = 0;      ///< sub_xxxx / loc_xxxx still to be named
    std::size_t commentedLines = 0;
    std::size_t userComments = 0;    ///< line and block comments that come from the annotations (not automatic ones)
};

/// Builds the annotated, re-assemblable listing of [from, to].
///
/// Code comes from the analysis in `dis`; names, comments and data types come from `ann`; Amstrad
/// CPC firmware entries and hardware ports are shown by name. The memory image is read as plain RAM
/// (ROM overlays are ignored). With `verify`, the text is assembled again and compared byte by byte
/// with the image, so a listing that claims `verified` is guaranteed to describe the same program.
ListingResult renderListing(const Memory& memory, const Disassembler& dis, const Annotations& ann,
                            const ListingOptions& options = {});

}  // namespace z80
