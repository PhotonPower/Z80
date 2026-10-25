#pragma once

#include <array>
#include <span>

#include "z80/types.hpp"

namespace z80 {

/// Flat 64 KiB Z80 address space. All accesses wrap around at 0xFFFF.
class Memory {
public:
    Memory() { data_.fill(0); }

    [[nodiscard]] Byte read(Word address) const { return data_[address]; }
    [[nodiscard]] Word read16(Word address) const {
        return static_cast<Word>(read(address) | (read(static_cast<Word>(address + 1)) << 8));
    }

    void write(Word address, Byte value) { data_[address] = value; }

    /// Copies `bytes` to `address`; data that would pass 0xFFFF is truncated.
    void load(Word address, std::span<const Byte> bytes) {
        for (std::size_t i = 0; i < bytes.size() && address + i < kAddressSpace; ++i) {
            data_[address + i] = bytes[i];
        }
    }

    void clear() { data_.fill(0); }

    [[nodiscard]] std::span<const Byte> bytes() const { return data_; }

private:
    std::array<Byte, kAddressSpace> data_;
};

}  // namespace z80
