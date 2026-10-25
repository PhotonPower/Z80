#pragma once

#include <deque>
#include <map>
#include <string>
#include <string_view>

#include "z80/assembler.hpp"
#include "z80/cpu.hpp"
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
    Memory memory_;
    Cpu cpu_;
    std::map<std::string, Symbol> symbols_;
    std::map<std::string, Snapshot> checkpoints_;
    std::deque<std::pair<Word, Byte>> ioLog_;
    std::deque<Byte> input_;
    friend struct Impl;
};

}  // namespace z80
