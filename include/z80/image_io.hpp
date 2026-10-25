#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "z80/assembler.hpp"
#include "z80/memory.hpp"

namespace z80 {

/// Size of the header in front of the RAM dump in an Amstrad CPC .sna snapshot.
inline constexpr std::size_t kSnaHeaderSize = 0x100;

/// Reads a whole file. Throws std::runtime_error if it cannot be read.
std::vector<Byte> readFile(const std::filesystem::path& path);

/// Writes a whole file. Throws std::runtime_error on failure.
void writeFile(const std::filesystem::path& path, std::span<const Byte> bytes);

/// True if `file` starts with the "MV - SNA" magic of a CPC snapshot.
bool looksLikeSna(std::span<const Byte> file);

/// Loads the first 64 KiB of RAM from a CPC snapshot (the data after the 0x100 byte header).
Memory loadSna(std::span<const Byte> file);

/// Loads a raw binary at `origin`.
Memory loadRaw(std::span<const Byte> file, Word origin);

/// Writes assembled segments into a CPC snapshot's RAM image in place.
/// Throws std::runtime_error if a segment lies beyond the end of the file.
void patchSna(std::vector<Byte>& file, const std::vector<Segment>& segments);

/// Intel HEX text (16 data bytes per record) for the given segments.
std::string toIntelHex(const std::vector<Segment>& segments);

}  // namespace z80
