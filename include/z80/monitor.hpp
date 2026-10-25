#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "z80/cpu.hpp"
#include "z80/memory.hpp"
#include "z80/snapshot.hpp"

namespace z80 {

struct MonitorOptions {
    bool allowFiles = true;                           ///< load/save/asm of host files
    std::uint64_t maxInstructions = 10'000'000;       ///< per run command (`limit` changes it)
};

/// Interactive machine-code monitor: memory, disassembler, assembler, and the emulator's
/// debugger functions behind a classic one-line command interface.
///
/// `execute` takes one input line and returns the text to print; it never throws and has no
/// I/O of its own, so the same class serves the terminal front end (`z80tool monitor`) and tests.
/// Numbers in arguments are **hexadecimal** (`4000`, `FF`); other bases and expressions are
/// available through prefixes/suffixes (`&FF`, `0x4000`, `100.` = decimal 100, `%1010`), symbols
/// (`start+3`) and registers (`@HL`, `@PC`). Lines typed in assemble mode use assembler syntax.
class Monitor {
public:
    explicit Monitor(MonitorOptions options = {});
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    /// Executes one command line. Returns the output (empty or ending in a newline).
    std::string execute(std::string_view line);

    /// Prompt to show before reading the next line ("-" normally, "4000:" in assemble mode).
    [[nodiscard]] std::string prompt() const;
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] static std::string banner();

    [[nodiscard]] Cpu& cpu() { return cpu_; }
    [[nodiscard]] Memory& memory() { return memory_; }

private:
    struct Sym {
        std::int64_t value = 0;
        bool isConstant = false;  ///< defined with EQU / `sym`, as opposed to a code label
    };
    struct Impl;
    friend struct Impl;

    MonitorOptions options_;
    std::unique_ptr<Memory> memoryStore_;
    Memory& memory_;
    Cpu cpu_;
    std::map<std::string, Sym> symbols_;
    std::map<std::string, Snapshot> checkpoints_;
    std::deque<std::pair<Word, Byte>> ioLog_;
    std::deque<Byte> input_;
    Word nextDisassembly_ = 0;
    Word nextDump_ = 0;
    std::optional<Word> assembleAt_;
    std::uint64_t limit_;
    bool finished_ = false;
};

}  // namespace z80
