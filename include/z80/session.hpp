#pragma once

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "z80/analysis.hpp"
#include "z80/annotations.hpp"
#include "z80/assembler.hpp"
#include "z80/cpc.hpp"
#include "z80/cpu.hpp"
#include "z80/disassembler.hpp"
#include "z80/json.hpp"
#include "z80/memory.hpp"
#include "z80/snapshot.hpp"

namespace z80 {

struct SessionOptions {
    /// Allow commands that read/write files on the host (load_file, save_state, ...).
    /// Off by default: an agent should not get file access unless the operator opts in.
    bool allowFiles = false;
    std::size_t maxIoLog = 4096;  ///< newest OUT operations kept for the "io" command
};

/// One emulated machine plus the state an agent needs to work with it: symbols from the last
/// assembly, named checkpoints, an I/O log, and a JSON command interface.
///
/// Protocol: a request is a JSON object {"cmd": "...", ...}; the reply is a JSON object with
/// "ok": true plus results, or "ok": false and "error". `handle` never throws.
/// See docs/agent-protocol.md for the command list.
class Session {
public:
    explicit Session(SessionOptions options = {});
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Json handle(const Json& request);
    /// Parses one line of JSON, handles it and returns the reply as a single line.
    std::string handleLine(std::string_view line);

    [[nodiscard]] Cpu& cpu() { return cpu_; }
    [[nodiscard]] Memory& memory() { return memory_; }

private:
    struct Impl;

    SessionOptions options_;
    std::unique_ptr<Memory> memoryStore_;  // heap: a Session is ~64 KiB, keep it off small stacks
    Memory& memory_;
    Cpu cpu_;
    std::map<std::string, Symbol> symbols_;
    struct Checkpoint {
        Snapshot machine;      ///< CPU state and RAM
        cpc::VideoState video;  ///< screen mode, palette, CRTC (restored on rollback)
    };
    std::map<std::string, Checkpoint> checkpoints_;
    struct IoWrite {
        Word port = 0;
        Byte value = 0;
        Word pc = 0;  ///< address of the OUT instruction that wrote
    };
    std::deque<IoWrite> ioLog_;
    std::deque<Byte> input_;
    std::vector<std::pair<unsigned, unsigned>> unpackIgnore_;      ///< stages of an unpacker run_until_unpacked has seen already
    std::optional<std::pair<unsigned, unsigned>> lastUnpackHit_;   ///< written range of the last stop

    // ---- reverse-engineering state (see session_analysis.cpp) ----
    struct FirmwareCall {
        std::string name;
        Word address = 0;
        Byte a = 0;
        Word bc = 0, de = 0, hl = 0;
        std::optional<Word> callSite;  ///< the CALL instruction that led here (if the return address on the stack says so)
    };
    std::unique_ptr<Disassembler> dis_;   ///< code analysis (created by "analyze")
    std::set<Word> analysisEntries_;      ///< entry points given to "analyze"
    std::vector<JumpTable> jumpTables_;   ///< found by the last "analyze"
    std::vector<std::uint8_t> tableBytes_ = std::vector<std::uint8_t>(kAddressSpace, 0);  ///< bytes of followed address tables (data, never code)
    /// The program: set by asm, load_file and analyze {from, to}; jump table targets and data guesses stay inside it.
    std::optional<std::pair<Word, Word>> programRange_;
    Annotations ann_;                     ///< names and comments given by the user / LLM
    bool cpc_ = false;                    ///< Amstrad CPC mode: video hardware, firmware stubs
    cpc::VideoState video_;
    std::map<std::string, IndexedImage> screens_;
    std::deque<FirmwareCall> firmwareLog_;
    std::string console_;                 ///< characters passed to TXT_OUTPUT while stubbed
    void enableCpc();
    void applyRomState();
    void installFirmwareStubs();
    bool firmwareStub(Cpu& cpu);
    friend struct Impl;
};

}  // namespace z80
