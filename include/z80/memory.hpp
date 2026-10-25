#pragma once

#include <array>
#include <span>
#include <vector>

#include "z80/types.hpp"

namespace z80 {

/// Flat 64 KiB Z80 address space. All accesses wrap around at 0xFFFF.
class Memory {
public:
    Memory() { data_.fill(0); }

    /// What the CPU sees: RAM, or an enabled ROM overlay on top of it (see setLowRom/setHighRom).
    [[nodiscard]] Byte read(Word address) const {
        if (romActive_) {
            if (lowRomOn_ && address < 0x4000) return lowRom_[address];
            if (highRomOn_ && address >= 0xC000) return highRom_[address - 0xC000];
        }
        return data_[address];
    }
    /// Always the RAM underneath, ignoring ROM overlays.
    [[nodiscard]] Byte readRam(Word address) const { return data_[address]; }
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

    // ---- optional ROM overlays (writes always go to the RAM below) -------------------------
    /// 16 KiB ROM visible at 0x0000-0x3FFF while enabled (e.g. the CPC lower ROM).
    void setLowRom(std::span<const Byte> rom) { lowRom_.assign(rom.begin(), rom.end()); lowRom_.resize(0x4000); update(); }
    /// 16 KiB ROM visible at 0xC000-0xFFFF while enabled (e.g. the CPC upper ROM).
    void setHighRom(std::span<const Byte> rom) { highRom_.assign(rom.begin(), rom.end()); highRom_.resize(0x4000); update(); }
    void enableRoms(bool low, bool high) {
        lowRomOn_ = low;
        highRomOn_ = high;
        update();
    }
    [[nodiscard]] bool hasLowRom() const { return !lowRom_.empty(); }
    [[nodiscard]] bool hasHighRom() const { return !highRom_.empty(); }
    [[nodiscard]] bool lowRomEnabled() const { return lowRomOn_ && hasLowRom(); }
    [[nodiscard]] bool highRomEnabled() const { return highRomOn_ && hasHighRom(); }

    [[nodiscard]] std::span<const Byte> bytes() const { return data_; }

private:
    void update() { romActive_ = (lowRomOn_ && !lowRom_.empty()) || (highRomOn_ && !highRom_.empty()); }

    std::array<Byte, kAddressSpace> data_;
    std::vector<Byte> lowRom_;
    std::vector<Byte> highRom_;
    bool lowRomOn_ = false;
    bool highRomOn_ = false;
    bool romActive_ = false;
};

}  // namespace z80
