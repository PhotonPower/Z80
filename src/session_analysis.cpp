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
}

bool Session::firmwareStub(Cpu& cpu) {
    Registers& r = cpu.regs();
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
    } else if (*name == "SCR_SET_BORDER") {
        const Byte ink = r.main[0] <= 26 ? r.main[0] : Byte{0};
        video_.inks[16] = cpc::kInkToHardware[ink];
    } else if (*name == "MC_SET_INKS") {
        for (Word i = 0; i < 17; ++i) video_.inks[i] = memory_.read(static_cast<Word>(r.hl() + i)) & 0x1F;
    }
    // emulate RET
    const Word lo = memory_.read(r.sp);
    const Word hi = memory_.read(static_cast<Word>(r.sp + 1));
    r.sp = static_cast<Word>(r.sp + 2);
    r.pc = static_cast<Word>(lo | (hi << 8));
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
        s.dis_->setDataBarrier([ann, tables](Word a) { return (*tables)[a] != 0 || ann->dataCovering(a) != nullptr; });
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
    const IndexedImage current = cpc::renderScreen(s.memory_, s.video_, options);

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
