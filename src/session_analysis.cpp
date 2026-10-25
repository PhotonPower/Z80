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
    firmwareLog_.push_back({*name, r.pc, r.a(), r.bc(), r.de(), r.hl()});
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

Json Session::Impl::cmdAnalyze() {
    RamView ram(s);
    if (flag("reset", false) || !s.dis_) {
        s.dis_ = std::make_unique<Disassembler>(s.memory_);
        s.analysisEntries_.clear();
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
        if (entries.empty()) entries.push_back(s.cpu_.regs().pc);
        for (std::size_t i = 1; i < entries.size(); ++i) s.dis_->addEntryPoint(entries[i]);
        s.dis_->analyzeFlow(entries[0]);
        for (Word e : entries) s.analysisEntries_.insert(e);
    }
    Json j = Json::object();
    j.set("instructions", s.dis_->instructionCount()).set("call_targets", s.dis_->callers().size());
    if (s.dis_->hasResult()) j.set("from", hex16(s.dis_->lowestAddress())).set("to", hex16(s.dis_->highestAddress()));
    return j;
}

Json Session::Impl::cmdRoutines() {
    if (!s.dis_ || !s.dis_->hasResult()) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    RamView ram(s);
    const Disassembler& dis = *s.dis_;

    struct Info {
        Word address;
        std::size_t size = 0;
        std::set<Word> callees;
        std::string end = "unknown";
    };
    std::set<Word> entries;
    Json external = Json::array();
    for (const auto& [a, refs] : dis.callers()) {
        if (dis.isInstructionStart(a)) {
            entries.insert(a);
        } else {  // call into the firmware or into code that was not analysed
            Json e = Json::object();
            e.set("addr", hex16(a)).set("caller_count", refs.size());
            if (auto fw = cpc::firmwareName(a)) e.set("firmware", *fw);
            external.push(std::move(e));
        }
    }
    for (const auto& [a, doc] : s.ann_.routines()) {
        (void)doc;
        if (dis.isInstructionStart(a)) entries.insert(a);
    }
    for (Word a : s.analysisEntries_) {
        if (dis.isInstructionStart(a)) entries.insert(a);  // the program entry is a routine of its own
    }

    std::map<Word, Info> infos;
    for (Word entry : entries) {
        Info info;
        info.address = entry;
        Word a = entry;
        for (int guard = 0; guard < 4096 && dis.isInstructionStart(a); ++guard) {
            const Instruction ins = decode(s.memory_, a, {});
            info.size += ins.length;
            if ((ins.flow == FlowKind::Call || ins.flow == FlowKind::ConditionalCall) && ins.target) info.callees.insert(*ins.target);
            if (ins.flow == FlowKind::Return) { info.end = "ret"; break; }
            if (ins.flow == FlowKind::Jump) { info.end = "jump"; break; }
            if (ins.flow == FlowKind::JumpIndirect) { info.end = "jump_indirect"; break; }
            a = ins.next();
        }
        infos[entry] = std::move(info);
    }

    // depth in the call graph: leaves (no known callees) come first
    std::map<Word, int> depth;
    std::function<int(Word, int)> visit = [&](Word a, int budget) -> int {
        if (auto it = depth.find(a); it != depth.end()) return it->second;
        if (budget == 0 || infos.count(a) == 0) return 0;
        depth[a] = 0;  // cycle guard
        int d = 0;
        for (Word c : infos[a].callees) {
            if (infos.count(c)) d = std::max(d, 1 + visit(c, budget - 1));
        }
        return depth[a] = d;
    };
    for (const auto& [a, info] : infos) { (void)info; visit(a, 64); }

    std::vector<Word> order;
    for (const auto& [a, info] : infos) { (void)info; order.push_back(a); }
    const std::string how = req.find("order") && req.find("order")->isString() ? req.find("order")->asString() : "bottom_up";
    if (how == "bottom_up") {
        std::stable_sort(order.begin(), order.end(), [&](Word x, Word y) { return depth[x] < depth[y]; });
    }
    const bool onlyOpen = flag("undocumented_only", false);

    Json list = Json::array();
    for (Word a : order) {
        const Info& info = infos[a];
        const bool documented = s.ann_.routineAt(a) != nullptr;
        if (onlyOpen && documented) continue;
        Json e = Json::object();
        e.set("addr", hex16(a));
        if (auto n = s.ann_.nameAt(a)) e.set("name", *n);
        else e.set("name", "sub_" + hexDigits(a, 4));
        e.set("documented", documented).set("size", info.size).set("ends_with", info.end).set("depth", depth[a]);
        Json callers = Json::array();
        if (auto it = dis.callers().find(a); it != dis.callers().end()) {
            int n = 0;
            for (Word c : it->second) {
                if (n++ == 8) break;
                callers.push(hex16(c));
            }
            e.set("caller_count", it->second.size());
        } else {
            e.set("caller_count", 0);
        }
        e.set("callers", callers);
        Json callees = Json::array();
        for (Word c : info.callees) {
            Json ce = Json::object();
            ce.set("addr", hex16(c));
            if (auto n = s.ann_.nameAt(c)) ce.set("name", *n);
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

Json Session::Impl::cmdListing() {
    if (!s.dis_) throw SessionError("nothing analysed yet (run \"analyze\" first)");
    ListingOptions options;
    if (const Json* v = req.find("from")) options.from = address(*v, "from");
    if (const Json* v = req.find("to")) options.to = address(*v, "to");
    options.verify = flag("verify", true);
    options.showXrefs = flag("xrefs", true);
    RamView ram(s);
    const ListingResult r = renderListing(s.memory_, *s.dis_, s.ann_, options);
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
        check(s.ann_.setRoutine(a, {text("name"), optText("summary"), optText("inputs"), optText("outputs"), optText("clobbers")}));
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
        const auto kind = dataKindFromString(optText("kind").empty() ? "bytes" : optText("kind"));
        if (!kind) throw SessionError("kind must be bytes, words, text or pointers");
        r.kind = *kind;
        r.name = optText("name");
        r.comment = optText("comment");
        check(s.ann_.setData(r));
    } else if (cmd == "ann_comment") {
        const Word a = addr("addr");
        const std::string t = text("text");
        if (flag("block", false)) s.ann_.setBlockComment(a, t);
        else s.ann_.setComment(a, t);
    } else if (cmd == "ann_remove") {
        const Word a = addr("addr");
        const bool removed = s.ann_.removeLabel(a) | s.ann_.removeComment(a);
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
        result = Json::object();
        result.set("name", "z80tool").set("protocol", 3);
        return true;
    }
    if (cmd == "machine") result = cmdMachine();
    else if (cmd == "video") result = cmdVideo();
    else if (cmd == "load_sna") result = cmdLoadSna();
    else if (cmd == "load_rom") result = cmdLoadRom();
    else if (cmd == "analyze") result = cmdAnalyze();
    else if (cmd == "routines") result = cmdRoutines();
    else if (cmd == "listing") result = cmdListing();
    else if (cmd.rfind("ann_", 0) == 0) {
        static const std::set<std::string> known = {"ann_label", "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_remove",
                                                   "ann_title", "ann_list", "ann_save", "ann_load"};
        if (!known.count(cmd)) return false;
        result = cmdAnnotate(cmd);
    } else if (cmd == "screen" || cmd == "screen_diff" || cmd == "screens" || cmd == "screen_drop") result = cmdScreen(cmd);
    else if (cmd == "firmware_log") {
        Json list = Json::array();
        for (const auto& c : s.firmwareLog_) {
            Json e = Json::object();
            e.set("name", c.name).set("addr", hex16(c.address)).set("A", hex8(c.a)).set("BC", hex16(c.bc)).set("DE", hex16(c.de)).set("HL", hex16(c.hl));
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
