#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "z80/cpu.hpp"
#include "z80/json.hpp"

namespace z80 {

/// A complete machine state: processor state plus the 64 KiB address space.
/// Cheap enough (≈64 KiB) to keep many of them as checkpoints.
struct Snapshot {
    CpuState state;
    std::vector<Byte> memory;  ///< kAddressSpace bytes

    bool operator==(const Snapshot&) const = default;
};

Snapshot captureSnapshot(const Cpu& cpu);
/// Restores processor state and memory (the Cpu's Memory object is overwritten).
void restoreSnapshot(Cpu& cpu, const Snapshot& snapshot);

/// Binary file format "Z80EMUS1": header, little-endian register block, 64 KiB memory.
std::vector<Byte> serializeSnapshot(const Snapshot& snapshot);
/// Throws std::runtime_error on malformed input.
Snapshot deserializeSnapshot(std::span<const Byte> bytes);

// ---- registers by name ----------------------------------------------------------------

/// Names: A F B C D E H L, AF BC DE HL, AF' BC' DE' HL' (also AF2 …), IX IY SP PC, IXH IXL IYH IYL,
/// I R IM IFF1 IFF2 HALTED, WZ. Case-insensitive. Returns nullopt for unknown names.
std::optional<std::uint32_t> getRegister(const Registers& regs, std::string name);
/// Returns false for unknown names or values that do not fit the register.
bool setRegister(Registers& regs, std::string name, std::uint32_t value);

/// "SZYHXPNC" with '.' for cleared bits, e.g. "S.Y.X.NC".
std::string flagsToString(Byte f);

// ---- JSON -----------------------------------------------------------------------------

/// All registers; 16-bit values as "0x1234" strings, 8-bit as "0x12", flags as a string.
Json registersToJson(const Registers& regs);

/// Complete CpuState (registers, counters, pending interrupts). Round-trips with stateFromJson.
Json stateToJson(const CpuState& state);
/// Accepts numbers or numeric strings ("0x10", "&10", "$10", "16"). Unknown members are ignored,
/// missing ones keep their default. Throws JsonError on bad values.
CpuState stateFromJson(const Json& json);

}  // namespace z80
