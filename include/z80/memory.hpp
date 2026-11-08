#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <utility>
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

    void write(Word address, Byte value) {
        data_[address] = value;
        if (!written_.empty()) markWritten(address);
    }

    /// Copies `bytes` to `address`; data that would pass 0xFFFF is truncated.
    void load(Word address, std::span<const Byte> bytes) {
        for (std::size_t i = 0; i < bytes.size() && address + i < kAddressSpace; ++i) {
            data_[address + i] = bytes[i];
        }
    }

    void clear() {
        data_.fill(0);
        if (!written_.empty()) clearWritten();
    }

    // ---- which bytes were written (to find the code a self-extracting program has unpacked) -----------
    /// Starts (or stops) remembering every byte the program writes from now on (one bit per byte). `load` does not count.
    /// Starting clears the record.
    void trackWrites(bool on) {
        if (on) written_.assign(kAddressSpace / 8, 0);
        else written_.clear();
        writtenBytes_ = 0;
    }
    [[nodiscard]] bool tracking() const { return !written_.empty(); }
    void clearWritten() {
        std::fill(written_.begin(), written_.end(), Byte{0});
        writtenBytes_ = 0;
    }
    [[nodiscard]] bool written(Word address) const { return !written_.empty() && ((written_[address >> 3] >> (address & 7U)) & 1U) != 0; }
    /// Number of different bytes written since tracking started.
    [[nodiscard]] std::size_t writtenBytes() const { return writtenBytes_; }
    /// The run of consecutive written bytes around `address` as [first, end), looking at most `cap` bytes each way. {a, a} if not written.
    [[nodiscard]] std::pair<unsigned, unsigned> writtenRunAround(Word address, std::size_t cap) const {
        if (!written(address)) return {address, address};
        unsigned first = address, end = static_cast<unsigned>(address) + 1;
        while (address - first < cap && first > 0 && written(static_cast<Word>(first - 1))) --first;
        while (end - address <= cap && end < kAddressSpace && written(static_cast<Word>(end))) ++end;
        return {first, end};
    }
    /// The written bytes as ranges [first, end) of at least `minLength` consecutive bytes, in address order.
    [[nodiscard]] std::vector<std::pair<unsigned, unsigned>> writtenRanges(std::size_t minLength = 1) const {
        std::vector<std::pair<unsigned, unsigned>> out;
        if (written_.empty()) return out;
        unsigned start = 0;
        bool inside = false;
        for (unsigned a = 0; a <= kAddressSpace; ++a) {
            const bool w = a < kAddressSpace && written(static_cast<Word>(a));
            if (w && !inside) start = a, inside = true;
            if (!w && inside) {
                if (a - start >= minLength) out.emplace_back(start, a);
                inside = false;
            }
        }
        return out;
    }

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
    void markWritten(Word address) {
        Byte& b = written_[address >> 3];
        const auto bit = static_cast<Byte>(1U << (address & 7U));
        if (!(b & bit)) {
            b |= bit;
            ++writtenBytes_;
        }
    }
    void update() { romActive_ = (lowRomOn_ && !lowRom_.empty()) || (highRomOn_ && !highRom_.empty()); }

    std::array<Byte, kAddressSpace> data_;
    std::vector<Byte> lowRom_;
    std::vector<Byte> highRom_;
    bool lowRomOn_ = false;
    bool highRomOn_ = false;
    bool romActive_ = false;
    std::vector<Byte> written_;  // empty = not tracking
    std::size_t writtenBytes_ = 0;
};

}  // namespace z80
