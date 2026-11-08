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
#include "z80/dsk.hpp"
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
    // ---- a virtual disc drive for the firmware stubs (insert_disk): AMSDOS file reading and the READ SECTOR command ----
    std::unique_ptr<DskImage> disk_;
    struct InFile {
        bool open = false;
        std::string name;
        std::vector<Byte> data;  ///< without the AMSDOS header
        std::size_t pos = 0;
        Word load = 0, entry = 0;
        Byte type = 0x16;
    } inFile_;
    struct DiskEvent {
        std::string what;   ///< "load NAME.EXT", "sector T/&ID" ...
        Word address = 0;   ///< where the data went
        std::size_t bytes = 0;
        Word pc = 0;        ///< the CALL that asked for it
    };
    std::deque<DiskEvent> diskLog_;
    /// What KL_FIND_COMMAND returns for the disc BIOS commands &81-&89: kBiosEntry + (command - &81); handled by the stubs.
    static constexpr Word kBiosEntry = 0xBDF0;
    static constexpr Word kReadSectorEntry = kBiosEntry + 3;
    /// RST &18 (FAR CALL) is routed here: the stubs install JP kFarCallEntry at &0018 when a disk is inserted.
    static constexpr Word kFarCallEntry = 0xBDE0;
    bool biosCommand(Cpu& cpu, unsigned command);
    // ---- a little firmware kernel for the stubs: interrupts with frame flyback events, joystick and keys ----
    static constexpr Word kInterruptEntry = 0xB941;  ///< where the firmware's IM 1 handler is (&0038 jumps here)
    std::optional<Word> frameFlyRoutine_;           ///< registered with KL_NEW_FRAME_FLY: called once per frame
    std::vector<Registers> eventSaves_;
    /// The firmware's ink table (SCR_SET_INK / SCR_SET_BORDER, hardware colours): like the real screen pack, the stubs write it to
    /// the hardware at every frame flyback, so a program can change the inks during a frame (MC_SET_INKS) and get them back.
    /// Empty until a program sets an ink through the firmware.
    std::optional<std::array<Byte, 17>> firmwareInks_;
    /// The jump block entries are "RST &08 + address" as on a real CPC; their addresses point here (entry - &B900 + kFakeRom), into the
    /// lower ROM. A program that reads an address and calls the ROM directly (with the lower ROM enabled) still reaches the stubs.
    static constexpr Word kFakeRom = 0x3A00;
    bool stubLowerRom_ = false;  ///< the program enabled the lower ROM (KL_L_ROM_ENABLE): calls into kFakeRom.. are stubbed
    void setStubLowerRom(bool on);
    std::vector<Byte> coverage_;  ///< 64 KiB: 1 where an instruction started (command coverage); empty while off
    // ---- raster splits: mode and ink changes with the display line they happened on, for the last complete frame ----
    cpc::VideoState rasterStart_, lastRasterStart_;
    std::vector<cpc::RasterChange> rasterChanges_, lastRasterChanges_;
    std::uint64_t rasterFrame_ = ~std::uint64_t{0};
    void noteVideoChange(const cpc::VideoState& before);
    /// The state at the top of the last complete frame and the changes during it (none: the picture has one mode and palette).
    [[nodiscard]] std::pair<cpc::VideoState, std::vector<cpc::RasterChange>> lastFrameRaster() const;             ///< the registers of the interrupted program while an event routine runs
    Byte joystick_ = 0;                             ///< bits 0 up, 1 down, 2 left, 3 right, 4 fire 2, 5 fire 1 (joystick 0)
    Byte joystick2_ = 0;
    std::set<Byte> keysDown_;                       ///< key numbers 0-79 (row * 8 + bit)
    Byte psgLatch_ = 0, psgRegister_ = 0, ppiC_ = 0;  ///< what the program wrote to PPI ports A and C (PSG access, keyboard row)
    [[nodiscard]] bool keyDown(unsigned key) const;
    [[nodiscard]] Byte keyboardRow(unsigned row) const;
    bool diskStub(Cpu& cpu, const std::string& name);
    void enableCpc();
    void applyRomState();
    void installFirmwareStubs();
    bool firmwareStub(Cpu& cpu);
    friend struct Impl;
};

}  // namespace z80
