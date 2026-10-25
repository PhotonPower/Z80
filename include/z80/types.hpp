#pragma once

#include <cstddef>
#include <cstdint>

namespace z80 {

using Byte = std::uint8_t;
using Word = std::uint16_t;

/// Size of the Z80 address space in bytes.
inline constexpr std::size_t kAddressSpace = 0x10000;

}  // namespace z80
