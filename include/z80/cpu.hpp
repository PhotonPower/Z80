#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "z80/memory.hpp"
#include "z80/types.hpp"

namespace z80 {

/// Bit masks of the F register.
namespace flag {
inline constexpr Byte C = 0x01;   ///< carry
inline constexpr Byte N = 0x02;   ///< subtract
inline constexpr Byte PV = 0x04;  ///< parity / overflow
inline constexpr Byte X = 0x08;   ///< undocumented (bit 3)
inline constexpr Byte H = 0x10;   ///< half carry
inline constexpr Byte Y = 0x20;   ///< undocumented (bit 5)
inline constexpr Byte Z = 0x40;   ///< zero
inline constexpr Byte S = 0x80;   ///< sign
}  // namespace flag

/// The complete programmer-visible state of the processor (plus MEMPTR/WZ).
struct Registers {
    // Index order: B C D E H L F A  (so that index 6 is "F" instead of "(HL)")
    std::array<Byte, 8> main{};
    std::array<Byte, 8> alt{};  ///< shadow registers B' C' D' E' H' L' F' A'
    Word ix = 0;
    Word iy = 0;
    Word sp = 0xFFFF;
    Word pc = 0;
    Word wz = 0;  ///< internal MEMPTR register (affects flags 3/5 of BIT n,(HL))
    Byte i = 0;
    Byte r = 0;
    Byte im = 0;
    bool iff1 = false;
    bool iff2 = false;
    bool halted = false;

    [[nodiscard]] Byte a() const { return main[7]; }
    [[nodiscard]] Byte f() const { return main[6]; }
    [[nodiscard]] Word af() const { return pair(7, 6); }
    [[nodiscard]] Word bc() const { return pair(0, 1); }
    [[nodiscard]] Word de() const { return pair(2, 3); }
    [[nodiscard]] Word hl() const { return pair(4, 5); }
    [[nodiscard]] Word af2() const { return static_cast<Word>(alt[7] << 8 | alt[6]); }
    [[nodiscard]] Word bc2() const { return static_cast<Word>(alt[0] << 8 | alt[1]); }
    [[nodiscard]] Word de2() const { return static_cast<Word>(alt[2] << 8 | alt[3]); }
    [[nodiscard]] Word hl2() const { return static_cast<Word>(alt[4] << 8 | alt[5]); }

    void setAf(Word v) { setPair(7, 6, v); }
    void setBc(Word v) { setPair(0, 1, v); }
    void setDe(Word v) { setPair(2, 3, v); }
    void setHl(Word v) { setPair(4, 5, v); }
    void setAf2(Word v) { alt[7] = static_cast<Byte>(v >> 8); alt[6] = static_cast<Byte>(v); }
    void setBc2(Word v) { alt[0] = static_cast<Byte>(v >> 8); alt[1] = static_cast<Byte>(v); }
    void setDe2(Word v) { alt[2] = static_cast<Byte>(v >> 8); alt[3] = static_cast<Byte>(v); }
    void setHl2(Word v) { alt[4] = static_cast<Byte>(v >> 8); alt[5] = static_cast<Byte>(v); }

    bool operator==(const Registers&) const = default;

private:
    [[nodiscard]] Word pair(int hi, int lo) const {
        return static_cast<Word>(main[static_cast<std::size_t>(hi)] << 8 |
                                 main[static_cast<std::size_t>(lo)]);
    }
    void setPair(int hi, int lo, Word v) {
        main[static_cast<std::size_t>(hi)] = static_cast<Byte>(v >> 8);
        main[static_cast<std::size_t>(lo)] = static_cast<Byte>(v);
    }
};

/// Everything needed to resume execution exactly (apart from memory).
struct CpuState {
    Registers regs;
    std::uint64_t cycles = 0;        ///< T-states since reset
    std::uint64_t instructions = 0;  ///< executed instructions since reset
    bool eiDelay = false;            ///< interrupts still blocked for one instruction after EI
    bool irqPending = false;
    Byte irqData = 0xFF;             ///< byte on the data bus during interrupt acknowledge
    bool nmiPending = false;

    bool operator==(const CpuState&) const = default;
};

enum class StopReason {
    Stepped,           ///< the requested single step / step-over finished
    Breakpoint,        ///< PC reached a breakpoint (not yet executed)
    Address,           ///< PC reached the "run until" address (not yet executed)
    Returned,          ///< the current routine returned (RET/RETI/RETN)
    Halted,            ///< HALT with no interrupt that could wake the CPU
    InstructionLimit,
    CycleLimit,
    Watchpoint         ///< a watched memory location was read/written
};

const char* toString(StopReason reason);

struct RunLimits {
    std::uint64_t maxInstructions = 1'000'000;  ///< 0 = unlimited
    std::uint64_t maxCycles = 0;                ///< 0 = unlimited
};

struct RunResult {
    StopReason reason = StopReason::Stepped;
    std::uint64_t instructions = 0;  ///< executed during this call
    std::uint64_t cycles = 0;        ///< T-states during this call
    Word pc = 0;                     ///< PC when stopped
    std::optional<Word> watchAddress;  ///< for StopReason::Watchpoint
    bool watchWrite = false;
};

enum class WatchKind { Read, Write, Access };

/// Z80 processor core. All documented and the common undocumented instructions are implemented
/// (including the undocumented flag bits 3 and 5 and MEMPTR).
///
/// The Cpu does not own the memory; several Cpu objects may share or swap Memory objects.
class Cpu {
public:
    using PortIn = std::function<Byte(Word port)>;
    using PortOut = std::function<void(Word port, Byte value)>;
    using Trace = std::function<void(const Cpu&)>;  ///< called before every instruction

    explicit Cpu(Memory& memory) : memory_(&memory) { reset(); }

    // ---- state ----------------------------------------------------------------------
    /// Power-on state: PC = 0, SP = 0xFFFF, AF = 0xFFFF, interrupts disabled.
    void reset();

    [[nodiscard]] Registers& regs() { return state_.regs; }
    [[nodiscard]] const Registers& regs() const { return state_.regs; }
    [[nodiscard]] Memory& memory() { return *memory_; }
    [[nodiscard]] const Memory& memory() const { return *memory_; }
    void setMemory(Memory& memory) { memory_ = &memory; }

    [[nodiscard]] const CpuState& state() const { return state_; }
    void setState(const CpuState& state) { state_ = state; }

    [[nodiscard]] std::uint64_t cycles() const { return state_.cycles; }
    [[nodiscard]] std::uint64_t instructionCount() const { return state_.instructions; }

    // ---- execution ------------------------------------------------------------------
    /// Executes one instruction (or one idle cycle group while halted, or an interrupt
    /// acknowledge). Returns the T-states used. Breakpoints are not checked here.
    unsigned step();

    /// Executes exactly one instruction (honouring pending interrupts). Reports `Halted` instead
    /// of idling if the CPU is halted and nothing can wake it, and `Watchpoint` if one was hit.
    RunResult stepInstruction();

    /// Runs until a breakpoint/watchpoint/limit/halt. Execution resumes even if PC sits on a
    /// breakpoint.
    RunResult run(const RunLimits& limits = {});

    /// Runs until PC == address (the instruction at `address` is not executed).
    RunResult runUntil(Word address, const RunLimits& limits = {});

    /// Runs until the routine that is currently executing returns: stops after the first
    /// RET/RETI/RETN that pops a return address located above the current stack pointer.
    RunResult runUntilReturn(const RunLimits& limits = {});

    /// Executes one instruction; if it is a CALL/RST that is taken, runs until the callee has
    /// returned (breakpoints inside the callee still stop execution).
    RunResult stepOver(const RunLimits& limits = {});

    // ---- interrupts -----------------------------------------------------------------
    /// Raises the maskable interrupt line; it is accepted when IFF1 is set. `dataBus` is the
    /// byte placed on the bus (IM 0: opcode, IM 2: vector low byte).
    void requestInterrupt(Byte dataBus = 0xFF) {
        state_.irqPending = true;
        state_.irqData = dataBus;
    }
    void requestNmi() { state_.nmiPending = true; }

    // ---- debugging aids -------------------------------------------------------------
    void addBreakpoint(Word address) { breakpoints_.insert(address); }
    void removeBreakpoint(Word address) { breakpoints_.erase(address); }
    void clearBreakpoints() { breakpoints_.clear(); }
    [[nodiscard]] const std::set<Word>& breakpoints() const { return breakpoints_; }

    void addWatchpoint(Word address, WatchKind kind = WatchKind::Access) { watch_[address] = kind; }
    void removeWatchpoint(Word address) { watch_.erase(address); }
    void clearWatchpoints() { watch_.clear(); }
    [[nodiscard]] const std::map<Word, WatchKind>& watchpoints() const { return watch_; }

    void setTrace(Trace trace) { trace_ = std::move(trace); }

    void setPortHandlers(PortIn in, PortOut out) {
        portIn_ = std::move(in);
        portOut_ = std::move(out);
    }

private:
    struct RunSpec {
        RunLimits limits;
        std::optional<Word> untilPc;
        bool untilReturn = false;
        bool stepOver = false;
        bool single = false;
    };
    RunResult execute(const RunSpec& spec);

    // memory / bus
    Byte rd(Word address);
    void wr(Word address, Byte value);
    Word rd16(Word address);
    void wr16(Word address, Word value);
    Byte fetch8() { return memory_->read(state_.regs.pc++); }
    Word fetch16();
    Byte fetchOpcode();
    void push(Word value);
    Word pop();

    // instruction execution
    unsigned executeInstruction();
    unsigned execMain(Byte op, int mode);
    unsigned execCb();
    unsigned execIndexedCb(int mode);
    unsigned execEd();
    unsigned execBlock(int y, int z);
    unsigned acceptNmi();
    unsigned acceptInterrupt();

    // helpers
    Byte& flags() { return state_.regs.main[6]; }
    [[nodiscard]] Byte reg(int index, int mode) const;
    void setReg(int index, Byte value, int mode);
    [[nodiscard]] Word pairGet(int p, int mode) const;
    void pairSet(int p, int mode, Word value);
    Word& indexReg(int mode);
    Word indexAddress(int mode);
    [[nodiscard]] bool condition(int cc) const;
    void alu(int op, Byte v);
    Byte inc8(Byte v);
    Byte dec8(Byte v);
    Byte rotate(int y, Byte v);
    void daa();
    Word add16(Word a, Word b);
    void adc16(Word v);
    void sbc16(Word v);
    void ioBlockFlags(Byte value, unsigned k);

    Memory* memory_;
    CpuState state_;
    std::set<Word> breakpoints_;
    std::map<Word, WatchKind> watch_;
    Trace trace_;
    PortIn portIn_;
    PortOut portOut_;

    bool lastCall_ = false;
    bool lastReturn_ = false;
    std::optional<Word> watchHit_;
    bool watchHitWrite_ = false;
};

}  // namespace z80
