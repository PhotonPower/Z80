// Session commands for reverse engineering: CPC machine, analysis, annotations, listing, screen.

#include <algorithm>
#include <set>

#include "session_impl.hpp"

namespace z80 {

// ---- CPC machine and firmware stubs ----------------------------------------------------------

void Session::applyRomState() { memory_.enableRoms(video_.lowerRomOn, video_.upperRomOn); }

void Session::enableCpc() {
    cpc_ = true;
    if (!memory_.hasLowRom()) installFirmwareStubs();
    applyRomState();
}

void Session::installFirmwareStubs() {
    // The firmware jump block (&B900-&BDFF) lives in RAM and jumps into the lower ROM. Without that ROM
    // the calls are intercepted: they are logged, a few are emulated, and the routine "returns".
    cpu_.setExecHook(0xB900, 0xBDFF, [this](Cpu& cpu) { return firmwareStub(cpu); });
    // As on a real CPC, the restarts in RAM lead into the firmware: RST &38 (the IM 1 interrupt) to its handler, RST &18 (FAR CALL) to
    // the stubs. Only written where the RAM is still empty (a snapshot or a program may have put its own code there).
    auto install = [this](Word at, Word target) {
        if (memory_.read(at) == 0 && memory_.read(static_cast<Word>(at + 1)) == 0 && memory_.read(static_cast<Word>(at + 2)) == 0) {
            memory_.write(at, 0xC3);
            memory_.write(static_cast<Word>(at + 1), static_cast<Byte>(target));
            memory_.write(static_cast<Word>(at + 2), static_cast<Byte>(target >> 8));
        }
    };
    install(0x0038, kInterruptEntry);
    install(0x0018, kFarCallEntry);
    // the small instructions of the restart block that programs call directly: PCBC (&000E), PCDE (&0016), PCHL (&001E)
    auto put = [this](Word at, std::initializer_list<Byte> bytes) {
        bool empty = true;
        for (std::size_t i = 0; i < bytes.size(); ++i) empty = empty && memory_.read(static_cast<Word>(at + i)) == 0;
        if (!empty) return;
        Word a = at;
        for (Byte b : bytes) memory_.write(a++, b);
    };
    put(0x000E, {0xC5, 0xC9});  // PUSH BC : RET
    put(0x0016, {0xD5, 0xC9});  // PUSH DE : RET
    put(0x001E, {0xE9});        // JP (HL)
    put(0x003B, {0xC9});        // EXT INTERRUPT: RET
    // the jump block itself, as programs read it: B900-BAFF (high kernel) JP to itself, BB00- RST &08 (LOW JUMP) + an address in the
    // lower ROM (bit 15: upper ROM off)
    bool blockEmpty = true;
    for (unsigned a = 0xB900; a <= 0xBDFF; ++a) blockEmpty = blockEmpty && memory_.read(static_cast<Word>(a)) == 0;
    if (blockEmpty) {
        for (unsigned a = 0xB900; a + 2 <= 0xBAFF; a += 3) {
            memory_.write(static_cast<Word>(a), 0xC3);
            memory_.write(static_cast<Word>(a + 1), static_cast<Byte>(a));
            memory_.write(static_cast<Word>(a + 2), static_cast<Byte>(a >> 8));
        }
        for (unsigned a = 0xBB00; a + 2 <= 0xBDFF; a += 3) {
            const auto rom = static_cast<Word>(0x8000 | (kFakeRom + (a - 0xB900)));
            memory_.write(static_cast<Word>(a), 0xCF);
            memory_.write(static_cast<Word>(a + 1), static_cast<Byte>(rom));
            memory_.write(static_cast<Word>(a + 2), static_cast<Byte>(rom >> 8));
        }
    }
}

void Session::setStubLowerRom(bool on) {
    // while the lower ROM is "on", the hook also covers the fake ROM addresses (and costs a call per instruction)
    stubLowerRom_ = on;
    cpu_.setExecHookRange(on ? 0x0000 : 0xB900, 0xBDFF);
}

bool Session::firmwareStub(Cpu& cpu) {
    Registers& r = cpu.regs();
    if (r.pc < 0xB900) {
        // a direct call into the (fake) lower ROM: handled like the jump block entry it belongs to
        if (!stubLowerRom_ || r.pc < kFakeRom || r.pc > kFakeRom + 0x4FF) return false;
        const Word rom = r.pc;
        r.pc = static_cast<Word>(0xB900 + (rom - kFakeRom));
        if (firmwareStub(cpu)) return true;
        r.pc = rom;
        return false;
    }
    if (r.pc == kInterruptEntry || r.pc == kInterruptEntry + 1) {
        // the firmware's interrupt handler: once per frame (the interrupt during the vertical sync) the frame flyback event
        // runs; it returns to the second entry, which returns from the interrupt with EI
        if (r.pc == kInterruptEntry && firmwareInks_ && cpu.cycles() % 79872 < 13312 && video_.inks != *firmwareInks_) {
            // the screen pack's part of the frame flyback: the inks of its table go to the hardware again
            const cpc::VideoState before = video_;
            video_.inks = *firmwareInks_;
            noteVideoChange(before);
        }
        if (r.pc == kInterruptEntry && frameFlyRoutine_ && cpu.cycles() % 79872 < 13312 && eventSaves_.size() < 4) {
            eventSaves_.push_back(r);  // like the firmware, the event runs with the interrupted program's registers saved
            r.sp = static_cast<Word>(r.sp - 2);
            memory_.write(r.sp, static_cast<Byte>(kInterruptEntry + 1));
            memory_.write(static_cast<Word>(r.sp + 1), static_cast<Byte>((kInterruptEntry + 1) >> 8));
            r.pc = *frameFlyRoutine_;
            return true;
        }
        if (r.pc == kInterruptEntry + 1 && !eventSaves_.empty()) {
            const Registers saved = eventSaves_.back();
            eventSaves_.pop_back();
            r.main = saved.main;
            r.alt = saved.alt;
            r.ix = saved.ix;
            r.iy = saved.iy;
        }
        r.iff1 = r.iff2 = true;
        const Word lo = memory_.read(r.sp);
        const Word hi = memory_.read(static_cast<Word>(r.sp + 1));
        r.sp = static_cast<Word>(r.sp + 2);
        r.pc = static_cast<Word>(lo | (hi << 8));
        return true;
    }
    if (disk_ && r.pc >= kBiosEntry && r.pc < kBiosEntry + 9) return biosCommand(cpu, 0x81u + (r.pc - kBiosEntry));
    if (disk_ && r.pc == kFarCallEntry) {
        // RST &18: the inline word behind it points to a far address (address, ROM). A disc BIOS command is emulated, anything else is
        // called like a near routine; either way execution continues behind the inline word.
        const Word ret = static_cast<Word>(memory_.read(r.sp) | (memory_.read(static_cast<Word>(r.sp + 1)) << 8));
        const Word ptr = static_cast<Word>(memory_.read(ret) | (memory_.read(static_cast<Word>(ret + 1)) << 8));
        const Word far = static_cast<Word>(memory_.read(ptr) | (memory_.read(static_cast<Word>(ptr + 1)) << 8));
        const auto behind = static_cast<Word>(ret + 2);
        memory_.write(r.sp, static_cast<Byte>(behind));
        memory_.write(static_cast<Word>(r.sp + 1), static_cast<Byte>(behind >> 8));
        r.pc = far;  // a stub address is handled on the next step, with the corrected return address on the stack
        return true;
    }
    const auto name = cpc::firmwareName(r.pc);
    if (!name) return false;
    std::optional<Word> callSite;
    {
        // the return address on the stack points behind the CALL: its operand must be this entry
        const Word ret = static_cast<Word>(memory_.read(r.sp) | (memory_.read(static_cast<Word>(r.sp + 1)) << 8));
        const Word site = static_cast<Word>(ret - 3);
        const Byte op = memory_.read(site);
        const bool isCall = op == 0xCD || (op & 0xC7) == 0xC4;
        if (isCall && memory_.read16(static_cast<Word>(site + 1)) == r.pc) callSite = site;
    }
    firmwareLog_.push_back({*name, r.pc, r.a(), r.bc(), r.de(), r.hl(), callSite});
    while (firmwareLog_.size() > 4096) firmwareLog_.pop_front();

    auto clearScreen = [this] {
        const Word base = video_.screenBase();
        for (unsigned i = 0; i < 0x4000; ++i) memory_.write(static_cast<Word>(base + i), 0);
    };
    const Byte a = r.a();
    const cpc::VideoState videoBefore = video_;
    if (*name == "TXT_OUTPUT") {
        if (a >= 0x20 && a < 0x7F) console_ += static_cast<char>(a);
        else if (a == 0x0D || a == 0x0A) console_ += '\n';
        else console_ += "<&" + hexDigits(a, 2) + ">";
    } else if (*name == "SCR_SET_MODE") {
        video_.mode = a & 3;
        clearScreen();
    } else if (*name == "MC_SET_MODE") {
        video_.mode = a & 3;
    } else if (*name == "SCR_CLEAR") {
        clearScreen();
    } else if (*name == "SCR_SET_INK") {
        const Byte ink = r.main[0] <= 26 ? r.main[0] : Byte{0};  // B = first ink
        video_.inks[a & 15] = cpc::kInkToHardware[ink];
        if (!firmwareInks_) firmwareInks_ = video_.inks;
        (*firmwareInks_)[a & 15] = cpc::kInkToHardware[ink];
    } else if (*name == "SCR_SET_BORDER") {
        const Byte ink = r.main[0] <= 26 ? r.main[0] : Byte{0};
        video_.inks[16] = cpc::kInkToHardware[ink];
        if (!firmwareInks_) firmwareInks_ = video_.inks;
        (*firmwareInks_)[16] = cpc::kInkToHardware[ink];
    } else if (*name == "MC_SET_INKS") {
        // DE = the border colour, then the 16 pens (hardware colour numbers)
        video_.inks[16] = memory_.read(r.de()) & 0x1F;
        for (Word i = 0; i < 16; ++i) video_.inks[i] = memory_.read(static_cast<Word>(r.de() + 1 + i)) & 0x1F;
    }
    if (videoBefore.mode != video_.mode || videoBefore.inks != video_.inks) noteVideoChange(videoBefore);
    if (*name == "KL_NEW_FRAME_FLY" || *name == "KL_ADD_FRAME_FLY") {
        frameFlyRoutine_ = r.de();  // HL = the event block, B = class, C = ROM, DE = routine
    } else if (*name == "KL_DEL_FRAME_FLY") {
        frameFlyRoutine_.reset();
    } else if (*name == "KM_GET_JOYSTICK") {
        r.main[4] = joystick_;   // H
        r.main[5] = joystick2_;  // L
        r.main[7] = joystick_;
    } else if (*name == "KM_TEST_KEY") {
        const bool down = keyDown(a & 0x7F);
        r.main[6] = static_cast<Byte>((r.f() & ~0x40) | (down ? 0 : 0x40));  // NZ = pressed
        r.main[1] = 0;  // C = shift/control state
    } else if (*name == "SCR_GET_MODE") {
        r.main[7] = video_.mode;
        r.main[6] = static_cast<Byte>((r.f() & ~0x41) | (video_.mode == 0 ? 0x01 : 0) | (video_.mode == 1 ? 0x40 : 0));
    } else if (*name == "SCR_GET_INK") {
        Byte ink = 0;  // the firmware colour number of the pen's hardware colour
        for (Byte i = 0; i < 27; ++i) {
            if (cpc::kInkToHardware[i] == (video_.inks[a & 15] & 0x1F)) ink = i;
        }
        r.main[0] = r.main[1] = ink;
    }
    if (name->rfind("KL_U_ROM_", 0) == 0 || name->rfind("KL_L_ROM_", 0) == 0 || *name == "KL_ROM_RESTORE") {
        r.iff1 = r.iff2 = true;  // the high kernel switches the ROMs with interrupts off and ends with EI
    }
    if (*name == "KL_L_ROM_ENABLE") {
        r.main[7] = stubLowerRom_ ? 0x00 : 0x04;  // A = the previous ROM state
        setStubLowerRom(true);
    } else if (*name == "KL_L_ROM_DISABLE") {
        r.main[7] = stubLowerRom_ ? 0x00 : 0x04;
        setStubLowerRom(false);
    } else if (*name == "KL_ROM_RESTORE") {
        setStubLowerRom((a & 0x04) == 0);
    } else if (*name == "MC_WAIT_FLYBACK") {
        // returns once the vertical sync is on: the frame starts with it in this emulation
        const std::uint64_t phase = cpu.cycles() % 79872;
        if (phase >= 4096) {
            CpuState st = cpu.state();
            st.cycles += 79872 - phase;
            cpu.setState(st);
        }
    } else if (*name == "MC_START_PROGRAM") {
        // the program starts with a fresh stack and does not return
        r.sp = 0xC000;
        r.pc = r.hl();
        return true;
    } else if (disk_ && (name->rfind("CAS_IN_", 0) == 0 || *name == "KL_FIND_COMMAND")) {
        if (diskStub(cpu, *name)) return true;
    }
    // emulate RET
    const Word lo = memory_.read(r.sp);
    const Word hi = memory_.read(static_cast<Word>(r.sp + 1));
    r.sp = static_cast<Word>(r.sp + 2);
    r.pc = static_cast<Word>(lo | (hi << 8));
    return true;
}

// ---- the virtual disc drive ------------------------------------------------------------------------

namespace {
constexpr Byte kCarryFlag = 0x01, kZeroFlag = 0x40;
void setCarryZero(Registers& r, bool carry, bool zero) {
    auto f = static_cast<Byte>(r.f() & ~(kCarryFlag | kZeroFlag));
    if (carry) f |= kCarryFlag;
    if (zero) f |= kZeroFlag;
    r.main[6] = f;
}
}  // namespace

bool Session::diskStub(Cpu& cpu, const std::string& name) {
    Registers& r = cpu.regs();
    const Word ret = static_cast<Word>(memory_.read(r.sp) | (memory_.read(static_cast<Word>(r.sp + 1)) << 8));
    const auto site = static_cast<Word>(ret - 3);
    auto finish = [&] {  // RET
        r.sp = static_cast<Word>(r.sp + 2);
        r.pc = ret;
        return true;
    };
    auto log = [&](std::string what, Word address, std::size_t bytes) {
        diskLog_.push_back({std::move(what), address, bytes, site});
        while (diskLog_.size() > 256) diskLog_.pop_front();
    };
    if (name == "KL_FIND_COMMAND") {
        // the disc BIOS commands &81-&89 (one character with bit 7) are known; the caller reaches them through the address returned
        const Byte first = memory_.read(r.hl());
        if (first >= 0x81 && first <= 0x89) {
            r.setHl(static_cast<Word>(kBiosEntry + (first - 0x81)));
            r.main[1] = 7;  // C = ROM number of AMSDOS
            setCarryZero(r, true, false);
            // from now on RST &18 (FAR CALL) reaches the stubs
            memory_.write(0x0018, 0xC3);
            memory_.write(0x0019, static_cast<Byte>(kFarCallEntry));
            memory_.write(0x001A, static_cast<Byte>(kFarCallEntry >> 8));
        } else {
            setCarryZero(r, false, false);
        }
        return finish();
    }
    if (name == "READ_SECTOR") {
        // HL = buffer, E = drive, D = track, C = sector ID
        const Byte track = r.main[2], id = r.main[1];
        const auto* sec = disk_->sector(track, id);
        if (!sec) {
            log("sector " + std::to_string(track) + "/&" + hexDigits(id, 2) + " missing", r.hl(), 0);
            r.main[7] = 0x20;  // A = an FDC error
            setCarryZero(r, false, false);
            return finish();
        }
        for (std::size_t i = 0; i < sec->data.size(); ++i) memory_.write(static_cast<Word>(r.hl() + i), sec->data[i]);
        log("sector " + std::to_string(track) + "/&" + hexDigits(id, 2), r.hl(), sec->data.size());
        setCarryZero(r, true, false);
        return finish();
    }
    if (name == "CAS_IN_OPEN") {
        // B = name length, HL = name, DE = 2 KiB buffer
        std::string fname;
        for (unsigned i = 0; i < r.main[0]; ++i) {
            fname += static_cast<char>(std::toupper(memory_.read(static_cast<Word>(r.hl() + i)) & 0x7F));
        }
        while (!fname.empty() && fname.back() == ' ') fname.pop_back();
        std::vector<std::string> tries{fname};
        if (fname.find('.') == std::string::npos) tries = {fname, fname + ".BAS", fname + ".BIN"};  // what AMSDOS tries without an extension
        for (const std::string& t : tries) {
            std::vector<Byte> data;
            try {
                data = disk_->readFile(t);
            } catch (const DskError&) {
                continue;
            }
            inFile_ = InFile{};
            inFile_.open = true;
            inFile_.name = t;
            std::vector<Byte> header(64, 0);
            if (const auto h = parseAmsdosHeader(data)) {
                std::copy(data.begin(), data.begin() + 64, header.begin());
                const std::size_t n = std::min<std::size_t>(h->length, data.size() - 128);
                inFile_.data.assign(data.begin() + 128, data.begin() + 128 + static_cast<std::ptrdiff_t>(n));
                inFile_.load = h->load;
                inFile_.entry = h->exec;
                inFile_.type = h->type;
            } else {
                inFile_.data = data;
                while (!inFile_.data.empty() && inFile_.data.back() == 0x1A) inFile_.data.pop_back();  // CP/M end of file
                for (std::size_t i = 0; i < t.size() && i < 16; ++i) header[1 + i] = static_cast<Byte>(t[i]);
                header[18] = 0x16;  // ASCII file
                header[24] = static_cast<Byte>(inFile_.data.size());
                header[25] = static_cast<Byte>(inFile_.data.size() >> 8);
            }
            for (std::size_t i = 0; i < 64; ++i) memory_.write(static_cast<Word>(r.de() + i), header[i]);
            r.setHl(r.de());  // the header lies in the buffer
            r.setDe(inFile_.load);
            r.setBc(static_cast<Word>(inFile_.data.size()));
            r.main[7] = inFile_.type;
            setCarryZero(r, true, false);
            log("open " + t, inFile_.load, inFile_.data.size());
            return finish();
        }
        log("open " + fname + ": not found", 0, 0);
        r.main[7] = 0x92;  // file not found
        setCarryZero(r, false, false);
        return finish();
    }
    if (name == "CAS_IN_DIRECT") {
        if (!inFile_.open) {
            setCarryZero(r, false, false);
            return finish();
        }
        for (std::size_t i = 0; i < inFile_.data.size(); ++i) memory_.write(static_cast<Word>(r.hl() + i), inFile_.data[i]);
        log("load " + inFile_.name, r.hl(), inFile_.data.size());
        inFile_.pos = inFile_.data.size();
        r.setHl(inFile_.entry);
        setCarryZero(r, true, false);
        return finish();
    }
    if (name == "CAS_IN_CHAR") {
        if (!inFile_.open || inFile_.pos >= inFile_.data.size()) {
            r.main[7] = 0x1A;  // end of file
            setCarryZero(r, false, false);
        } else {
            r.main[7] = inFile_.data[inFile_.pos++];
            setCarryZero(r, true, false);
        }
        return finish();
    }
    if (name == "WRITE_SECTOR") {
        // HL = buffer, E = drive, D = track, C = sector ID: written into the inserted image (save_disk writes it to a file)
        const Byte track = r.main[2], id = r.main[1];
        const auto* sec = disk_->sector(track, id);
        if (!sec) {
            log("write sector " + std::to_string(track) + "/&" + hexDigits(id, 2) + " missing", r.hl(), 0);
            r.main[7] = 0x20;
            setCarryZero(r, false, false);
            return finish();
        }
        std::vector<Byte> data(sec->data.size());
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = memory_.read(static_cast<Word>(r.hl() + i));
        disk_->writeSector(track, id, data);
        log("write sector " + std::to_string(track) + "/&" + hexDigits(id, 2), r.hl(), data.size());
        setCarryZero(r, true, false);
        return finish();
    }
    if (name == "CAS_IN_CLOSE" || name == "CAS_IN_ABANDON") {
        inFile_.open = false;
        setCarryZero(r, true, false);
        return finish();
    }
    return false;
}

bool Session::biosCommand(Cpu& cpu, unsigned command) {
    Registers& r = cpu.regs();
    switch (command) {
        case 0x84: return diskStub(cpu, "READ_SECTOR");
        case 0x85: return diskStub(cpu, "WRITE_SECTOR");
        default: break;
    }
    // &81 set message, &82 setup disc, &83 select format, &86 format track, &87 move track, &88 drive status, &89 retry count:
    // nothing to do for an image; success
    const Word ret = static_cast<Word>(memory_.read(r.sp) | (memory_.read(static_cast<Word>(r.sp + 1)) << 8));
    diskLog_.push_back({"bios &" + hexDigits(command, 2), r.hl(), 0, static_cast<Word>(ret - 3)});
    while (diskLog_.size() > 256) diskLog_.pop_front();
    if (command == 0x88) r.main[7] = 0x20;  // drive status: ready
    r.main[6] = static_cast<Byte>(r.f() | 0x01);
    r.sp = static_cast<Word>(r.sp + 2);
    r.pc = ret;
    return true;
}

// ---- commands -------------------------------------------------------------------------------------

Json Session::Impl::cmdMachine() {
    const std::string type = req.find("type") && req.find("type")->isString() ? req.find("type")->asString() : "";
    if (type != "cpc") throw SessionError("only type \"cpc\" is supported");
    s.enableCpc();
    if (s.cpu_.regs().sp == 0xFFFF) s.cpu_.regs().sp = 0xBFFF;  // the power-on SP would lie inside the screen RAM
    Json j = Json::object();
    j.set("machine", "cpc").set("firmware_stubs", !s.memory_.hasLowRom());
    return j;
}

Json Session::Impl::cmdVideo() {
    if (!s.cpc_) throw SessionError("not a CPC machine (use machine or load_sna first)");
    const cpc::VideoState& v = s.video_;
    Json j = Json::object();
    j.set("mode", v.mode).set("screen_base", hex16(v.screenBase())).set("lower_rom", v.lowerRomOn).set("upper_rom", v.upperRomOn);
    Json pens = Json::array();
    for (std::size_t i = 0; i < 16; ++i) {
        Json e = Json::object();
        e.set("pen", i).set("hw", v.inks[i]).set("colour", cpc::kHardwareColourNames[v.inks[i] & 0x1F]);
        pens.push(std::move(e));
    }
    j.set("pens", pens).set("border", cpc::kHardwareColourNames[v.inks[16] & 0x1F]);
    Json crtc = Json::object();
    for (int reg : {1, 2, 6, 7, 9, 12, 13}) crtc.set("R" + std::to_string(reg), v.crtc[static_cast<std::size_t>(reg)]);
    j.set("crtc", crtc);
    return j;
}

Json Session::Impl::cmdLoadSna() {
    requireFiles();
    const auto data = readFile(text("file"));
    const auto header = cpc::parseSnaHeader(data);
    if (!header) throw SessionError("not a CPC snapshot (.sna)");
    s.memory_.load(0, loadSna(data).bytes());
    s.programRange_.reset();
    if (flag("state", true)) {
        s.cpu_.setState(header->cpu);
        s.video_ = header->video;
    }
    s.enableCpc();
    Json j = Json::object();
    j.set("regs", regs()).set("snapshot_version", header->version).set("video", cmdVideo());
    if (header->memoryKiB > 64) j.set("note", "only the first 64 KiB of RAM were loaded");
    return j;
}

Json Session::Impl::cmdLoadRom() {
    requireFiles();
    const auto data = readFile(text("file"));
    const std::string which = req.find("which") && req.find("which")->isString() ? req.find("which")->asString() : "";
    if (data.size() < 0x4000) throw SessionError("a ROM image has at least 16 KiB");
    const std::span<const Byte> all(data);
    if (which == "lower") {
        s.memory_.setLowRom(all.subspan(0, 0x4000));
    } else if (which == "upper") {
        s.memory_.setHighRom(all.subspan(0, 0x4000));
    } else if (which == "both") {
        if (data.size() < 0x8000) throw SessionError("\"both\" needs a 32 KiB image (lower ROM followed by upper ROM)");
        s.memory_.setLowRom(all.subspan(0, 0x4000));
        s.memory_.setHighRom(all.subspan(0x4000, 0x4000));
    } else {
        throw SessionError("'which' must be lower, upper or both");
    }
    if (s.memory_.hasLowRom()) s.cpu_.clearExecHook();  // the real firmware replaces the stubs
    s.applyRomState();
    Json j = Json::object();
    j.set("lower_rom", s.memory_.hasLowRom()).set("upper_rom", s.memory_.hasHighRom()).set("firmware_stubs", !s.memory_.hasLowRom() && s.cpc_);
    return j;
}

AnalysisContext Session::Impl::analysisContext() const {
    AnalysisContext ctx;
    if (s.programRange_) {
        ctx.first = s.programRange_->first;
        ctx.last = s.programRange_->second;
    } else if (s.dis_ && s.dis_->hasResult()) {
        ctx.first = s.dis_->lowestAddress();
        ctx.last = s.dis_->highestAddress();
        // annotated data right behind the code belongs to the program
        for (const auto& [a, region] : s.ann_.dataRegions()) {
            if (a > ctx.last && a <= ctx.last + 0x40u) ctx.last = std::min<unsigned>(0xFFFF, static_cast<unsigned>(a) + region.length - 1);
        }
    }
    const Annotations* ann = &s.ann_;
    ctx.isData = [ann](Word a) { return ann->dataCovering(a) != nullptr; };
    ctx.claimed = [ann](Word a) { return ann->dataCovering(a) != nullptr || ann->expressionFor(a).has_value(); };
    for (const auto& [a, region] : s.ann_.dataRegions()) {
        if (region.kind == DataKind::Pointers) ctx.pointerRegions.emplace_back(a, region.length);
    }
    return ctx;
}

Json Session::Impl::cmdAnalyze() {
    RamView ram(s);
    if (const Json* from = req.find("from")) {
        const Json* to = req.find("to");
        if (!to) throw SessionError("'from' needs 'to' (the last byte of the program)");
        const Word f = address(*from, "from"), t = address(*to, "to");
        if (t < f) throw SessionError("'to' must not be below 'from'");
        s.programRange_ = std::make_pair(f, t);
    }
    if (flag("reset", false) || !s.dis_) {
        s.dis_ = std::make_unique<Disassembler>(s.memory_);
        s.analysisEntries_.clear();
        s.jumpTables_.clear();
        std::fill(s.tableBytes_.begin(), s.tableBytes_.end(), 0);
    }
    {
        const Annotations* ann = &s.ann_;
        const std::vector<std::uint8_t>* tables = &s.tableBytes_;
        // on a CPC the firmware jump block is not part of the program: calls into it are not followed
        const bool cpc = s.cpc_;
        s.dis_->setDataBarrier([ann, tables, cpc](Word a) {
            return (*tables)[a] != 0 || ann->dataCovering(a) != nullptr || (cpc && a >= 0xB900 && a <= 0xBDFF);
        });
    }
    if (const Json* inl = req.find("inline_words"); inl && inl->isArray()) {
        // CALL/RST targets followed by one inline address, e.g. "0x18": RST &18 is the CPC firmware's FAR CALL
        for (const Json& e : inl->items()) s.dis_->addInlineData(address(e, "inline_words"), InlineDataKind::Word);
    }
    if (const Json* args = req.find("inline_control_args"); args && args->isObject()) {
        // {"0x1F": 2, "0x1E": 1}: control codes in inline strings and the number of argument bytes behind them
        std::map<Byte, unsigned> m;
        for (const auto& [k, v] : args->members()) {
            const Json key(k);
            const Word code = address(key, "inline_control_args");
            if (code > 0xFF || !v.isNumber() || v.asInt() < 0 || v.asInt() > 8) throw SessionError("inline_control_args: {\"code\": 0-8}");
            m[static_cast<Byte>(code)] = static_cast<unsigned>(v.asInt());
        }
        s.dis_->setInlineControlArguments(std::move(m));
    }
    if (const Json* inl = req.find("inline_strings"); inl && inl->isArray()) {
        const std::string kind = req.find("inline_kind") && req.find("inline_kind")->isString() ? req.find("inline_kind")->asString() : "bit7";
        for (const Json& e : inl->items()) s.dis_->addInlineData(address(e, "inline_strings"), kind == "zero" ? InlineDataKind::StringZero : InlineDataKind::StringBit7);
    }
    std::vector<Word> entries;
    if (const Json* e = req.find("entries"); e && e->isArray()) {
        for (const Json& item : e->items()) entries.push_back(address(item, "entries"));
    }
    if (const Json* range = req.find("linear"); range && range->isObject()) {
        const Json* from = range->find("from");
        const Json* to = range->find("to");
        if (!from || !to) throw SessionError("'linear' needs from and to");
        s.dis_->analyzeRange(address(*from, "from"), address(*to, "to"));
    } else {
        if (entries.empty() && s.analysisEntries_.empty()) entries.push_back(s.cpu_.regs().pc);
        for (Word e : entries) s.analysisEntries_.insert(e);
        for (Word e : s.analysisEntries_) s.dis_->addEntryPoint(e);
        s.dis_->analyzeFlow(entries.empty() ? *s.analysisEntries_.begin() : entries[0]);
    }
    // jump tables (and interrupt handlers the program installs): follow their targets until nothing new turns up
    const bool followTables = flag("tables", true);
    for (int round = 0; round < 16; ++round) {
        s.jumpTables_ = findJumpTables(s.memory_, *s.dis_, analysisContext());
        bool grown = false;
        for (Word handler : findInterruptHandlers(s.memory_, *s.dis_)) {
            if (s.dis_->isInstructionStart(handler)) continue;
            s.analysisEntries_.insert(handler);  // a routine of its own: nobody calls it, the interrupt does
            s.dis_->addEntryPoint(handler);
            s.dis_->analyzeFlow(handler);
            grown = true;
        }
        for (const JumpTable& t : s.jumpTables_) {
            if (!t.followed || !followTables) continue;
            if (t.kind == JumpTable::Kind::Pointers) {
                for (unsigned i = 0; i < t.bytes && t.table + i < kAddressSpace; ++i) s.tableBytes_[t.table + i] = 1;
            }
            std::vector<Word> starts = t.targets;
            if (t.kind == JumpTable::Kind::JumpRow) starts.insert(starts.end(), t.slots.begin(), t.slots.end());
            for (Word target : starts) {
                if (s.dis_->isInstructionStart(target)) continue;
                s.dis_->addEntryPoint(target);
                s.dis_->analyzeFlow(target);
                grown = true;
            }
        }
        if (!grown) break;
    }
    Json j = Json::object();
    j.set("instructions", s.dis_->instructionCount()).set("call_targets", s.dis_->callers().size());
    j.set("jump_tables", s.jumpTables_.size());
    if (s.dis_->hasResult()) j.set("from", hex16(s.dis_->lowestAddress())).set("to", hex16(s.dis_->highestAddress()));
    if (s.programRange_) {
        Json pr = Json::object();
        pr.set("from", hex16(s.programRange_->first)).set("to", hex16(s.programRange_->second));
        j.set("program", pr);
    }
    return j;
}

std::set<Word> Session::Impl::routineEntries() const {
    std::set<Word> entries;
    const Disassembler& dis = *s.dis_;
    for (const auto& [a, refs] : dis.callers()) {
        (void)refs;
        if (dis.isInstructionStart(a)) entries.insert(a);
    }
    for (const auto& [a, doc] : s.ann_.routines()) {
        (void)doc;
        if (dis.isInstructionStart(a)) entries.insert(a);
    }
    for (Word a : s.analysisEntries_) {
        if (dis.isInstructionStart(a)) entries.insert(a);  // the program entry is a routine of its own
    }
    for (const JumpTable& t : s.jumpTables_) {  // a case of a jump table is code that nobody calls directly: a routine of its own
        if (!t.followed) continue;
        for (Word target : t.targets) {
            if (dis.isInstructionStart(target)) entries.insert(target);
        }
    }
    return entries;
}

Json Session::Impl::cmdRoutines() {
    if (!s.dis_ || !s.dis_->hasResult()) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    RamView ram(s);
    const Disassembler& dis = *s.dis_;

    Json external = Json::array();
    for (const auto& [a, refs] : dis.callers()) {
        if (dis.isInstructionStart(a)) continue;
        // call into the firmware or into code that was not analysed
        Json e = Json::object();
        e.set("addr", hex16(a)).set("caller_count", refs.size());
        if (auto fw = cpc::firmwareName(a)) e.set("firmware", *fw);
        external.push(std::move(e));
    }
    std::vector<RoutineInfo> infos = computeRoutines(s.memory_, dis, routineEntries(), s.jumpTables_);

    const std::string how = req.find("order") && req.find("order")->isString() ? req.find("order")->asString() : "bottom_up";
    if (how != "bottom_up" && how != "address") throw SessionError("order must be bottom_up or address");
    if (how == "bottom_up") {
        std::stable_sort(infos.begin(), infos.end(), [](const RoutineInfo& x, const RoutineInfo& y) { return x.depth < y.depth; });
    }
    const bool onlyOpen = flag("undocumented_only", false);

    Json list = Json::array();
    for (const RoutineInfo& info : infos) {
        const Word a = info.entry;
        const RoutineDoc* doc = s.ann_.routineAt(a);
        const bool documented = doc != nullptr;
        if (onlyOpen && documented) continue;
        Json e = Json::object();
        e.set("addr", hex16(a));
        if (auto n = s.ann_.nameAt(a)) e.set("name", *n);
        else e.set("name", "sub_" + hexDigits(a, 4));
        e.set("documented", documented).set("size", info.end - info.start).set("ends_with", info.endsWith).set("depth", info.depth);
        e.set("start", hex16(info.start)).set("end", hex16(info.end & 0xFFFF)).set("instructions", info.instructions).set("bytes", info.bytes);
        Json blocks = Json::array();
        for (const auto& [bs, be] : info.blocks) {
            Json b = Json::array();
            b.push(hex16(bs));
            b.push(hex16(be & 0xFFFF));
            blocks.push(std::move(b));
        }
        e.set("blocks", blocks);
        if (documented) {
            e.set("confidence", doc->confidence).set("summary", doc->summary).set("inputs", doc->inputs).set("outputs", doc->outputs).set("clobbers", doc->clobbers);
        }
        if (info.viaTable && info.callers.empty()) e.set("via_table", hex16(*info.viaTable));
        Json callers = Json::array();
        int n = 0;
        for (Word c : info.callers) {
            if (n++ == 8) break;
            callers.push(hex16(c));
        }
        e.set("caller_count", info.callers.size()).set("callers", callers);
        Json callees = Json::array();
        for (Word c : info.callees) {
            Json ce = Json::object();
            ce.set("addr", hex16(c));
            if (auto nm = s.ann_.nameAt(c)) ce.set("name", *nm);
            else ce.set("name", "sub_" + hexDigits(c, 4));
            ce.set("documented", s.ann_.routineAt(c) != nullptr);
            callees.push(std::move(ce));
        }
        for (Word c : info.external) {
            Json ce = Json::object();
            ce.set("addr", hex16(c));
            if (auto nm = s.ann_.nameAt(c)) ce.set("name", *nm);
            else if (auto fw = cpc::firmwareName(c)) ce.set("firmware", *fw);
            callees.push(std::move(ce));
        }
        e.set("calls", callees);
        list.push(std::move(e));
    }
    Json j = Json::object();
    j.set("routines", list).set("external_calls", external);
    return j;
}

Json Session::Impl::cmdJumpTables() {
    if (!s.dis_ || !s.dis_->hasResult()) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    const auto maxEntries = static_cast<std::size_t>(integer("max_entries", 64, 1, 4096));
    Json arr = Json::array();
    for (const JumpTable& t : s.jumpTables_) {
        Json e = Json::object();
        e.set("kind", t.kind == JumpTable::Kind::JumpRow ? "jump_row" : "pointers");
        if (t.jump) e.set("jump", hex16(*t.jump));
        e.set("table", hex16(t.table)).set("bytes", t.bytes).set("followed", t.followed).set("how", t.how);
        e.set("entry_count", t.slots.size());
        Json entries = Json::array();
        for (std::size_t i = 0; i < t.slots.size() && i < maxEntries; ++i) {
            Json x = Json::object();
            x.set("slot", hex16(t.slots[i])).set("target", hex16(t.targets[i]));
            if (auto n = s.ann_.nameAt(t.targets[i])) x.set("name", *n);
            entries.push(std::move(x));
        }
        e.set("entries", entries);
        if (!t.followed) e.set("hint", "looks like a table of code addresses; mark it with ann_data kind=pointers and analyze again to follow it");
        arr.push(std::move(e));
    }
    Json j = Json::object();
    j.set("tables", arr);
    return j;
}

Json Session::Impl::cmdDataRegions() {
    if (!s.dis_ || !s.dis_->hasResult()) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    RamView ram(s);
    const bool apply = flag("apply", false);
    const std::vector<DataGuess> guesses = guessData(s.memory_, *s.dis_, s.jumpTables_, analysisContext());
    Json arr = Json::array();
    unsigned applied = 0;
    for (const DataGuess& g : guesses) {
        Json e = Json::object();
        const char* kind = g.kind == DataGuess::Kind::Text ? "text" : g.kind == DataGuess::Kind::Pointers ? "pointers"
                           : g.kind == DataGuess::Kind::Words ? "words" : "zeros";
        e.set("addr", hex16(g.start)).set("length", g.length).set("kind", kind).set("reason", g.reason);
        if (!g.preview.empty()) e.set("preview", g.preview);
        if (apply && g.kind != DataGuess::Kind::Zeros) {  // zero runs need no annotation: the listing writes them as DS
            DataRegion region;
            region.address = g.start;
            region.length = g.length;
            region.kind = g.kind == DataGuess::Kind::Text ? DataKind::Text : g.kind == DataGuess::Kind::Words ? DataKind::Words : DataKind::Pointers;
            const bool done = !s.ann_.setData(region).has_value();
            e.set("applied", done);
            if (done) ++applied;
        }
        arr.push(std::move(e));
    }
    Json j = Json::object();
    j.set("regions", arr);
    if (apply) j.set("applied", applied);
    return j;
}

Json Session::Impl::cmdReferences() {
    // what a stretch of code refers to: the targets of its 16-bit data operands, with name, type and a look at the contents
    if (!s.dis_ || !s.dis_->hasResult()) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    RamView ram(s);
    const Disassembler& dis = *s.dis_;
    const Annotations& ann = s.ann_;
    const unsigned from = addr("from");
    const unsigned to = req.find("to") ? static_cast<unsigned>(addr("to")) + 1 : 0x10000u;  // 'to' is the last byte (inclusive)
    std::map<Word, std::vector<Word>> targets;
    const AnalysisContext ctx = analysisContext();
    for (unsigned a = from; a < to && a < kAddressSpace; ++a) {
        if (!dis.isInstructionStart(static_cast<Word>(a)) || ann.isConstant(static_cast<Word>(a))) continue;
        const auto d = dataOperandAt(s.memory_, static_cast<Word>(a));
        if (!d) continue;
        if (!d->memory && d->value < 0x0100) continue;  // small numbers are rarely addresses
        // an immediate outside the program is a number (LD BC,&0506) unless it names the screen or the firmware
        if (!d->memory && (d->value < ctx.first || d->value > ctx.last) && !(s.cpc_ && (d->value >= 0xC000 || cpc::firmwareName(d->value)))) continue;
        if (dis.isInstructionStart(d->value) && d->value >= from && d->value < to) continue;  // a label inside the stretch: the listing shows it
        targets[d->value].push_back(static_cast<Word>(a));
    }
    auto printable = [](Byte b) { return b >= 0x20 && b < 0x7F; };
    auto rd = [this](unsigned a) { return s.memory_.read(static_cast<Word>(a)); };
    Json arr = Json::array();
    for (const auto& [t, users] : targets) {
        std::string kind = "unknown", name, preview;
        unsigned length = 0;
        const DataRegion* region = ann.dataCovering(t);
        if (dis.isInstructionStart(t)) {
            kind = "code";
            name = ann.nameAt(t) ? *ann.nameAt(t) : "sub_" + hexDigits(t, 4);
        } else if (auto fw = cpc::firmwareName(t); fw && s.cpc_) {
            kind = "firmware";
            name = *fw;
        } else if (s.cpc_ && t >= 0xC000 && !region) {
            kind = "screen";
            name = "screen";
            preview = cpc::describeScreenAddress(t);
        } else if (region) {
            kind = toString(region->kind);
            length = region->length;
            name = ann.nameAt(region->address) ? *ann.nameAt(region->address) : "data_" + hexDigits(region->address, 4);
            if (t != region->address) name += "+" + std::to_string(t - region->address);
            const unsigned end = static_cast<unsigned>(region->address) + region->length;
            if (region->kind == DataKind::Text) {
                for (unsigned p = t; p < end && preview.size() < 60; ++p) {
                    const Byte b = rd(p);
                    if (b == 0) break;
                    const Byte c = b & 0x7F;
                    preview += c == 13 ? std::string("\\r") : c == 10 ? std::string("\\n") : printable(c) ? std::string(1, static_cast<char>(c)) : "\\x" + hexDigits(b, 2);
                    if (b >= 0x80) break;  // the last character has bit 7 set
                }
            } else if (region->kind == DataKind::Words || region->kind == DataKind::Pointers) {
                unsigned p = t;
                for (; p + 2 <= end && preview.size() < 70; p += 2) {
                    const Word v = s.memory_.read16(static_cast<Word>(p));
                    std::string item = hexDigits(v, 4);
                    if (region->kind == DataKind::Pointers) {
                        if (auto n = ann.expressionFor(v)) item = *n;
                    }
                    preview += (preview.empty() ? "" : " ") + item;
                }
                if (p + 2 <= end) preview += " ...";
            } else {
                for (unsigned p = t; p < end && p < t + 16u; ++p) preview += (preview.empty() ? "" : " ") + hexDigits(rd(p), 2);
            }
        } else {
            if (auto n = ann.expressionFor(t)) {
                kind = "variable";
                name = *n;
            } else {
                name = "data_" + hexDigits(t, 4);
            }
            // bytes that no region covers but read like a short string: shown as such, marked with a question mark
            unsigned n = 0;
            while (n < 40 && t + n < kAddressSpace && printable(rd(t + n))) ++n;
            if (n >= 2 && t + n < kAddressSpace && (rd(t + n) == 0 || (rd(t + n) >= 0xA0 && printable(rd(t + n) & 0x7F))) && !dis.isCovered(t)) {
                kind = "text?";
                length = n + 1;
                for (unsigned i = 0; i <= n; ++i) {
                    if (rd(t + i) != 0) preview += static_cast<char>(rd(t + i) & 0x7F);
                }
            }
            if (preview.empty()) {
                for (unsigned p = t; p < t + 8u && p < kAddressSpace; ++p) preview += (preview.empty() ? "" : " ") + hexDigits(rd(p), 2);
            }
        }
        Json e = Json::object();
        e.set("addr", hex16(t)).set("name", name).set("kind", kind).set("length", length);
        if (!preview.empty()) e.set("preview", preview);
        Json usedAt = Json::array();
        for (std::size_t i = 0; i < users.size() && i < 4; ++i) usedAt.push(hex16(users[i]));
        e.set("used_at", usedAt);
        arr.push(std::move(e));
    }
    Json j = Json::object();
    j.set("references", arr);
    return j;
}

Json Session::Impl::cmdListing() {
    if (!s.dis_) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    ListingOptions options;
    if (const Json* v = req.find("from")) options.from = address(*v, "from");
    if (const Json* v = req.find("to")) options.to = address(*v, "to");
    options.verify = flag("verify", true);
    options.showXrefs = flag("xrefs", true);
    options.addresses = flag("addresses", false);
    options.equates = flag("equates", true);
    {
        const AnalysisContext ctx = analysisContext();
        options.program = std::make_pair(static_cast<Word>(ctx.first), static_cast<Word>(ctx.last));
        // the whole listing reaches to the end of the program: data behind the last instruction belongs to it
        if (!options.to && s.programRange_ && s.dis_->hasResult() && s.programRange_->second > s.dis_->highestAddress() &&
            (!options.from || *options.from <= s.programRange_->second)) {
            options.to = s.programRange_->second;
        }
    }
    RamView ram(s);
    // followed address tables are data: shown as DW rows with the names of their targets
    Annotations shown = s.ann_;
    for (const JumpTable& t : s.jumpTables_) {
        if (!t.followed || t.kind != JumpTable::Kind::Pointers || t.bytes < 2) continue;
        bool free = true;
        for (unsigned i = 0; i < t.bytes && free; ++i) free = shown.dataCovering(static_cast<Word>(t.table + i)) == nullptr;
        if (!free) continue;
        DataRegion region;
        region.address = t.table;
        region.length = t.bytes & ~1u;
        region.kind = DataKind::Pointers;
        (void)shown.setData(region);
    }
    options.entries = routineEntries();
    for (const JumpTable& t : s.jumpTables_) {
        if (!t.followed) continue;
        if (t.kind == JumpTable::Kind::JumpRow) options.targets.insert(t.table);  // an address table gets a data_ label from its user
        options.notes[t.table] = "jump table (" + std::to_string(t.slots.size()) + " entries): " + t.how;
    }
    const ListingResult r = renderListing(s.memory_, *s.dis_, shown, options);
    const auto maxChars = static_cast<std::size_t>(integer("max_chars", 60000, 1000, 10'000'000));
    Json j = Json::object();
    if (r.text.size() > maxChars) {
        j.set("text", r.text.substr(0, maxChars)).set("truncated", true);
    } else {
        j.set("text", r.text).set("truncated", false);
    }
    j.set("verified", r.verified).set("verify_detail", r.verifyDetail);
    j.set("from", hex16(r.from)).set("to", hex16(r.to)).set("instructions", r.instructions);
    j.set("named_labels", r.namedLabels).set("auto_labels", r.autoLabels).set("user_comments", r.userComments);
    return j;
}

Json Session::Impl::cmdAnnotate(const std::string& cmd) {
    auto check = [](const std::optional<std::string>& err) {
        if (err) throw SessionError(*err);
    };
    auto optText = [&](const char* key) -> std::string {
        const Json* v = req.find(key);
        return v && v->isString() ? v->asString() : std::string();
    };
    Json j = Json::object();
    if (cmd == "ann_label") {
        const Word a = addr("addr");
        check(s.ann_.setLabel(a, text("name")));
    } else if (cmd == "ann_routine") {
        const Word a = addr("addr");
        check(s.ann_.setRoutine(a, {text("name"), optText("summary"), optText("inputs"), optText("outputs"), optText("clobbers"), optText("confidence")}));
    } else if (cmd == "ann_variable") {
        VariableDoc v;
        v.address = addr("addr");
        v.name = text("name");
        v.size = static_cast<unsigned>(integer("size", 1, 1, 0x4000));
        v.comment = optText("comment");
        check(s.ann_.setVariable(v));
    } else if (cmd == "ann_data") {
        DataRegion r;
        r.address = addr("addr");
        r.length = static_cast<unsigned>(integer("length", 0, 1, 0x10000));
        if (s.dis_) {
            for (unsigned k = r.address; k < r.address + r.length && k <= 0xFFFF; ++k) {
                if (s.dis_->isInstructionStart(static_cast<Word>(k))) {
                    throw SessionError("the range contains code (an instruction starts at &" + hexDigits(k, 4) +
                                       "); mark_data is for data only. For a single variable that lives inside the code use name_variable.");
                }
            }
        }
        const auto kind = dataKindFromString(optText("kind").empty() ? "bytes" : optText("kind"));
        if (!kind) throw SessionError("kind must be bytes, words, text or pointers");
        r.kind = *kind;
        r.name = optText("name");
        r.comment = optText("comment");
        check(s.ann_.setData(r));
    } else if (cmd == "ann_comment") {
        const Word a = addr("addr");
        if (s.dis_ && s.dis_->isCovered(a) && !s.dis_->isInstructionStart(a)) {
            // such a comment would never appear in the listing: say where the instruction really starts
            RamView ram(s);
            for (unsigned back = 1; back <= 3; ++back) {
                const Word start = static_cast<Word>(a - back);
                if (!s.dis_->isInstructionStart(start)) continue;
                const Instruction ins = decode(s.memory_, start, {});
                if (start + ins.length > a) {
                    throw SessionError("address &" + hexDigits(a, 4) + " is in the middle of the instruction at &" + hexDigits(start, 4) + " (" +
                                       ins.mnemonic + (ins.operands.empty() ? "" : " " + ins.operands) + "), where no comment can be shown. Comment the instruction start &" +
                                       hexDigits(start, 4) + " instead. The listing has the address of every instruction in its left column.");
                }
            }
        }
        const std::string t = text("text");
        if (flag("block", false)) s.ann_.setBlockComment(a, t);
        else s.ann_.setComment(a, t);
    } else if (cmd == "ann_constant") {
        const Word a = addr("addr");
        if (s.dis_ && !s.dis_->isInstructionStart(a)) {
            throw SessionError("address &" + hexDigits(a, 4) + " is not the first byte of an analysed instruction; a constant is marked on the instruction that holds the 16-bit value");
        }
        {
            RamView ram(s);
            if (!dataOperandAt(s.memory_, a)) {
                throw SessionError("the instruction at &" + hexDigits(a, 4) + " has no 16-bit operand that could be an address (only LD rr,nn and LD (nn) forms)");
            }
        }
        s.ann_.setConstant(a, !flag("remove", false));
    } else if (cmd == "ann_remove") {
        const Word a = addr("addr");
        const bool removedName = s.ann_.removeLabel(a);
        const bool removedComment = s.ann_.removeComment(a);
        const bool removed = removedName || removedComment;
        j.set("removed", removed);
    } else if (cmd == "ann_title") {
        s.ann_.setTitle(text("text"));
    } else if (cmd == "ann_list") {
        j.set("annotations", s.ann_.toJson());
    } else if (cmd == "ann_save") {
        requireFiles();
        const std::string dump = s.ann_.toJson().dump();
        writeFile(text("file"), std::vector<Byte>(dump.begin(), dump.end()));
    } else if (cmd == "ann_load") {
        requireFiles();
        const auto data = readFile(text("file"));
        s.ann_ = Annotations::fromJson(Json::parse(std::string(data.begin(), data.end())));
        j.set("labels", s.ann_.labels().size());
    }
    return j;
}

Json Session::Impl::cmdScreen(const std::string& cmd) {
    if (cmd == "screens") {
        Json names = Json::array();
        for (const auto& [name, img] : s.screens_) {
            Json e = Json::object();
            e.set("name", name).set("width", img.width).set("height", img.height);
            names.push(std::move(e));
        }
        Json j = Json::object();
        j.set("screens", names);
        return j;
    }
    if (cmd == "screen_drop") {
        s.screens_.erase(text("name"));
        return Json::object();
    }
    if (!s.cpc_) throw SessionError("not a CPC machine (use machine or load_sna first)");
    cpc::RenderOptions options;
    options.border = flag("border", false);
    // with mode or ink changes during the last frame (raster splits) every line shows the state it was displayed with
    const auto [rasterStart, rasterChanges] = s.lastFrameRaster();
    const bool raster = flag("raster", true) && !rasterChanges.empty();
    const IndexedImage current = raster ? cpc::renderScreenRaster(s.memory_, rasterStart, rasterChanges, options)
                                        : cpc::renderScreen(s.memory_, s.video_, options);

    if (cmd == "screen") {
        const std::string name = req.find("name") && req.find("name")->isString() ? req.find("name")->asString() : "last";
        s.screens_[name] = current;  // the full screen is stored; crop/zoom only affect what is returned
        IndexedImage shown = current;
        Json j = Json::object();
        if (const Json* crop = req.find("crop"); crop && crop->isObject()) {
            // a box in text cells of an 80x25 screen: every cell is 8 px wide and 16 px tall in the image
            auto cell = [&](const char* key) {
                const Json* v = crop->find(key);
                if (!v || !v->isNumber() || v->asInt() < 0 || v->asInt() > 400) throw SessionError(std::string("crop needs ") + key + " (0..)");
                return static_cast<int>(v->asInt());
            };
            const int col0 = cell("col0"), row0 = cell("row0"), col1 = cell("col1"), row1 = cell("row1");
            if (col1 < col0 || row1 < row0) throw SessionError("crop: end is before start");
            const int px = (col1 - col0 + 1) * 8;
            const int zoom = static_cast<int>(integer("zoom", std::clamp(320 / std::max(px, 1), 1, 16), 1, 16));
            shown = cpc::cropAndZoom(current, col0 * 8, row0 * 16, col1 * 8 + 7, row1 * 16 + 15, zoom);
            j.set("crop", true).set("zoom", zoom);
        }
        j.set("name", name).set("mode", s.video_.mode).set("width", shown.width).set("height", shown.height);
        if (raster) {
            Json splits = Json::array();
            for (const auto& c : rasterChanges) {
                Json e = Json::object();
                e.set("line", c.line).set("mode", c.video.mode);
                splits.push(std::move(e));
            }
            j.set("raster_splits", splits).set("top_mode", rasterStart.mode);
        }
        j.set("png_base64", base64Encode(encodePng(shown)));
        return j;
    }
    // screen_diff
    const std::string a = text("a");
    auto it = s.screens_.find(a);
    if (it == s.screens_.end()) throw SessionError("unknown screen '" + a + "'");
    IndexedImage other = current;
    if (const Json* b = req.find("b"); b && b->isString()) {
        auto jt = s.screens_.find(b->asString());
        if (jt == s.screens_.end()) throw SessionError("unknown screen '" + b->asString() + "'");
        other = jt->second;
    }
    const cpc::ImageDiff d = cpc::diffImages(it->second, other);
    Json j = Json::object();
    j.set("same_size", d.sameSize).set("changed_pixels", d.changedPixels);
    if (d.sameSize && d.changedPixels > 0) {
        Json box = Json::object();
        box.set("x0", d.x0).set("y0", d.y0).set("x1", d.x1).set("y1", d.y1);
        j.set("bbox", box);
        // the same region in text cells of an 80x25 screen (every cell is 8 px wide and 16 px tall here)
        Json cells = Json::object();
        cells.set("col0", d.x0 / 8).set("row0", d.y0 / 16).set("col1", d.x1 / 8).set("row1", d.y1 / 16);
        j.set("text_cells", cells);
        j.set("changed_percent", 100.0 * static_cast<double>(d.changedPixels) / (static_cast<double>(other.width) * other.height));
    }
    return j;
}

bool Session::Impl::analysisCommand(const std::string& cmd, Json& result) {
    if (cmd == "info") {
        // protocol 3: analysis, annotations, listing with idiom comments and "user_comments", screen, CPC machine
        // protocol 4: jump_tables, data_regions, references, ann_constant, routine blocks, unpacking, firmware call sites
        result = Json::object();
        result.set("name", "z80tool").set("protocol", 4);
        return true;
    }
    if (cmd == "machine") result = cmdMachine();
    else if (cmd == "video") result = cmdVideo();
    else if (cmd == "load_sna") result = cmdLoadSna();
    else if (cmd == "load_rom") result = cmdLoadRom();
    else if (cmd == "analyze") result = cmdAnalyze();
    else if (cmd == "routines") result = cmdRoutines();
    else if (cmd == "jump_tables") result = cmdJumpTables();
    else if (cmd == "data_regions") result = cmdDataRegions();
    else if (cmd == "references") result = cmdReferences();
    else if (cmd == "listing") result = cmdListing();
    else if (cmd.rfind("ann_", 0) == 0) {
        static const std::set<std::string> known = {"ann_label", "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_constant", "ann_remove",
                                                   "ann_title", "ann_list", "ann_save", "ann_load"};
        if (!known.count(cmd)) return false;
        result = cmdAnnotate(cmd);
    } else if (cmd == "screen" || cmd == "screen_diff" || cmd == "screens" || cmd == "screen_drop") result = cmdScreen(cmd);
    else if (cmd == "firmware_log") {
        Json list = Json::array();
        for (const auto& c : s.firmwareLog_) {
            Json e = Json::object();
            e.set("name", c.name).set("addr", hex16(c.address)).set("A", hex8(c.a)).set("BC", hex16(c.bc)).set("DE", hex16(c.de)).set("HL", hex16(c.hl));
            if (c.callSite) e.set("call_site", hex16(*c.callSite));
            list.push(std::move(e));
        }
        result = Json::object();
        result.set("calls", list);
        if (flag("clear", false)) s.firmwareLog_.clear();
    } else if (cmd == "insert_disk") {
        // a .dsk for the firmware stubs: CAS_IN_* read its files, KL_FIND_COMMAND &84 / READ SECTOR its sectors
        requireFiles();
        if (!s.cpc_) throw SessionError("not a CPC machine (use machine first)");
        try {
            s.disk_ = std::make_unique<DskImage>(DskImage::parse(readFile(text("file"))));
        } catch (const DskError& e) {
            throw SessionError(e.what());
        }
        s.inFile_ = {};
        result = Json::object();
        result.set("format", s.disk_->format()).set("files", s.disk_->files().size());
    } else if (cmd == "save_disk") {
        requireFiles();
        if (!s.disk_) throw SessionError("no disk inserted");
        writeFile(text("file"), s.disk_->serialize());
        result = Json::object();
    } else if (cmd == "coverage") {
        // the start addresses of executed instructions: they tell code from data, also code the analysis cannot reach
        if (req.find("on")) {
            if (flag("on", true)) {
                if (s.coverage_.empty()) s.coverage_.assign(0x10000, 0);
            } else {
                s.coverage_.clear();
            }
        }
        Json ranges = Json::array();
        std::size_t count = 0;
        if (!s.coverage_.empty()) {
            const unsigned from = static_cast<unsigned>(integer("from", 0, 0, 0xFFFF));
            const unsigned to = static_cast<unsigned>(integer("to", 0xFFFF, 0, 0xFFFF));
            // runs of instructions with gaps of at most 3 bytes (the rest of an instruction) form a range
            std::optional<unsigned> start;
            unsigned last = 0;
            auto close = [&] {
                if (!start) return;
                Json e = Json::array();
                e.push(hex16(static_cast<Word>(*start)));
                e.push(hex16(static_cast<Word>(last)));
                ranges.push(std::move(e));
                start.reset();
            };
            for (unsigned a = from; a <= to; ++a) {
                if (!s.coverage_[a]) continue;
                ++count;
                if (start && a - last > 4) close();
                if (!start) start = a;
                last = a;
            }
            close();
        }
        result = Json::object();
        result.set("on", !s.coverage_.empty()).set("instructions", count).set("ranges", ranges);
        if (flag("addresses", false) && !s.coverage_.empty()) {
            // every executed instruction address (good entry points for analyze: also code reached only through vectors)
            Json all = Json::array();
            const unsigned from = static_cast<unsigned>(integer("from", 0, 0, 0xFFFF));
            const unsigned to = static_cast<unsigned>(integer("to", 0xFFFF, 0, 0xFFFF));
            for (unsigned a = from; a <= to; ++a) {
                if (s.coverage_[a]) all.push(hex16(static_cast<Word>(a)));
            }
            result.set("addresses", all);
        }
        if (flag("clear", false) && !s.coverage_.empty()) std::fill(s.coverage_.begin(), s.coverage_.end(), Byte{0});
    } else if (cmd == "controls") {
        // joystick and keys as the program sees them (firmware KM_* and the keyboard matrix behind PSG register 14)
        if (req.find("joystick")) s.joystick_ = static_cast<Byte>(integer("joystick", 0, 0, 255));
        if (req.find("joystick2")) s.joystick2_ = static_cast<Byte>(integer("joystick2", 0, 0, 255));
        if (const Json* k = req.find("keys"); k && k->isArray()) {
            s.keysDown_.clear();
            for (const Json& key : k->items()) {
                const std::int64_t n = value(key, "keys");
                if (n < 0 || n > 79) throw SessionError("key numbers are 0-79");
                s.keysDown_.insert(static_cast<Byte>(n));
            }
        }
        result = Json::object();
        Json keys = Json::array();
        for (Byte k : s.keysDown_) keys.push(static_cast<std::int64_t>(k));
        result.set("joystick", s.joystick_).set("joystick2", s.joystick2_).set("keys", keys);
    } else if (cmd == "disk_log") {
        Json list = Json::array();
        for (const auto& e : s.diskLog_) {
            Json j = Json::object();
            j.set("what", e.what).set("addr", hex16(e.address)).set("bytes", e.bytes).set("call_site", hex16(e.pc));
            list.push(std::move(j));
        }
        result = Json::object();
        result.set("events", list);
        if (flag("clear", false)) s.diskLog_.clear();
    } else if (cmd == "console") {
        result = Json::object();
        result.set("text", s.console_);
        if (flag("clear", false)) s.console_.clear();
    } else {
        return false;
    }
    return true;
}

}  // namespace z80
