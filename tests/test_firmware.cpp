// The firmware stubs as a small kernel: the virtual disc drive (insert_disk: AMSDOS files and READ SECTOR through KL_FIND_COMMAND
// and RST &18), interrupts with the frame flyback event and the ink refresh, joystick and keys (controls), HALT between
// interrupts, the jump block with its ROM addresses (MC WAIT FLYBACK called directly), raster splits in the screen, coverage,
// and inline data the analysis understands (RST &18 + address, print codes with arguments).

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "test_util.hpp"
#include "z80/dsk.hpp"
#include "z80/json.hpp"
#include "z80/session.hpp"

using namespace z80;

namespace {

Json ask(Session& s, const std::string& request) { return Json::parse(s.handleLine(request)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
std::string str(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : "<missing>";
}
std::string quote(const std::string& text) { return Json(text).dump(); }
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool assemble(Session& s, const std::string& source) {
    const Json r = ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(source) + "}");
    if (!ok(r)) std::cerr << "asm failed: " << r.dump() << "\n";
    return ok(r);
}

std::vector<Byte> readMem(Session& s, unsigned addr, unsigned len) {
    const Json r = ask(s, "{\"cmd\":\"read\",\"addr\":" + std::to_string(addr) + ",\"len\":" + std::to_string(len) + "}");
    std::vector<Byte> out;
    const std::string hex = str(r, "hex");
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<Byte>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

SessionOptions withFiles() {
    SessionOptions o;
    o.allowFiles = true;
    return o;
}

void makeCpc(Session& s) {
    if (!ok(ask(s, R"({"cmd":"machine","type":"cpc"})"))) std::cerr << "machine cpc failed\n";
}

std::string runTo(Session& s, const char* label, const char* extra = "") {
    return ask(s, std::string("{\"cmd\":\"run\",\"until\":\"") + label + "\",\"max_instructions\":2000000" + extra + "}").dump();
}

}  // namespace

int main() {
    // a disc with a binary (AMSDOS header) and a plain text file; one sector gets a known pattern
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "z80tool_test_firmware";
    std::filesystem::create_directories(dir);
    const std::string disc = (dir / "test.dsk").string();
    {
        DskImage d = DskImage::createFormatted("data");
        std::vector<Byte> game{0x11, 0x22, 0x33, 0x44, 0x55};
        std::vector<Byte> file = makeAmsdosHeader("GAME.BIN", 0x1234, 0x4321, static_cast<std::uint32_t>(game.size()));
        file.insert(file.end(), game.begin(), game.end());
        d.writeFile("GAME.BIN", file);
        const std::string text = "HI!";
        d.writeFile("NOTE.TXT", std::vector<Byte>(text.begin(), text.end()));
        std::vector<Byte> pattern(512);
        for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<Byte>(i * 7);
        CHECK(d.writeSector(39, 0xC9, pattern));
        const std::vector<Byte> bytes = d.serialize();
        std::ofstream(disc, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // ---- AMSDOS files: CAS_IN_OPEN (tries .BIN), CAS_IN_DIRECT, CAS_IN_CHAR, CAS_IN_CLOSE ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(ok(ask(s, "{\"cmd\":\"insert_disk\",\"file\":" + quote(disc) + "}")));
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  LD B,4\n"
                       "        LD HL,name\n"
                       "        LD DE,&8000\n"
                       "        CALL &BC77\n"          // CAS_IN_OPEN "GAME" -> GAME.BIN
                       "        LD (len),BC\n"
                       "        LD (load),DE\n"
                       "        LD HL,&5000\n"
                       "        CALL &BC83\n"          // CAS_IN_DIRECT
                       "        LD (entry),HL\n"
                       "        CALL &BC7A\n"          // CAS_IN_CLOSE
                       "        LD B,8\n"
                       "        LD HL,name2\n"
                       "        LD DE,&8000\n"
                       "        CALL &BC77\n"          // NOTE.TXT: no header
                       "        CALL &BC80\n"          // CAS_IN_CHAR
                       "        LD (ch),A\n"
                       "        LD B,4\n"
                       "        LD HL,name3\n"
                       "        CALL &BC77\n"          // not there
                       "        LD (err),A\n"
                       "done:   JR done\n"
                       "name:   DB \"GAME\"\n"
                       "name2:  DB \"NOTE.TXT\"\n"
                       "name3:  DB \"NONE\"\n"
                       "len:    DW 0\n"
                       "load:   DW 0\n"
                       "entry:  DW 0\n"
                       "ch:     DB 0\n"
                       "err:    DB 0\n"));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        runTo(s, "done");
        CHECK(readMem(s, 0x5000, 5) == (std::vector<Byte>{0x11, 0x22, 0x33, 0x44, 0x55}));
        const auto vars = readMem(s, 0x4000, 0x100);
        const Json sym = *ask(s, R"({"cmd":"symbols"})").find("symbols");
        auto at = [&](const char* n) { return static_cast<unsigned>(std::stoul(str(sym, n), nullptr, 16)) - 0x4000; };
        CHECK_EQ(vars[at("LEN")] | (vars[at("LEN") + 1] << 8), 5);
        CHECK_EQ(vars[at("LOAD")] | (vars[at("LOAD") + 1] << 8), 0x1234);
        CHECK_EQ(vars[at("ENTRY")] | (vars[at("ENTRY") + 1] << 8), 0x4321);
        CHECK_EQ(vars[at("CH")], Byte{'H'});
        CHECK_EQ(vars[at("ERR")], Byte{0x92});
        const Json log = ask(s, R"({"cmd":"disk_log"})");
        CHECK(has(log.dump(), "load GAME.BIN"));
        CHECK(has(log.dump(), "NONE: not found"));
    }

    // ---- READ SECTOR: KL_FIND_COMMAND &84, called directly and through RST &18 (FAR CALL) ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(ok(ask(s, "{\"cmd\":\"insert_disk\",\"file\":" + quote(disc) + "}")));
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  LD HL,cmd84\n"
                       "        CALL &BCD4\n"          // KL_FIND_COMMAND
                       "        LD (far),HL\n"
                       "        LD A,C\n"
                       "        LD (far+2),A\n"
                       "        LD HL,&6000\n"
                       "        LD DE,&2700\n"         // track 39, drive 0
                       "        LD C,&C9\n"
                       "        RST &18\n"
                       "        DW far\n"
                       "done:   JR done\n"
                       "cmd84:  DB &84\n"
                       "far:    DS 3\n"));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        runTo(s, "done");
        const auto sector = readMem(s, 0x6000, 512);
        CHECK_EQ(sector[1], Byte{7});
        CHECK_EQ(sector[511], static_cast<Byte>(511 * 7));
        CHECK(has(ask(s, R"({"cmd":"disk_log"})").dump(), "sector 39/&C9"));
    }

    // ---- interrupts: HALT waits for them, the frame flyback event runs with saved registers, the inks come back ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  LD HL,block\n"
                       "        LD B,&81\n"
                       "        LD C,0\n"
                       "        LD DE,event\n"
                       "        CALL &BCD7\n"          // KL_NEW_FRAME_FLY
                       "        LD A,1\n"
                       "        LD B,6\n"
                       "        LD C,6\n"
                       "        CALL &BC32\n"          // SCR_SET_INK pen 1 = bright red (hardware &0C)
                       "        LD DE,inks\n"
                       "        CALL &BD25\n"          // MC_SET_INKS: pen 1 = &14 for now
                       "        LD BC,&1234\n"
                       "        EI\n"
                       "wait:   HALT\n"
                       "        LD A,(count)\n"
                       "        CP 3\n"
                       "        JR C,wait\n"
                       "done:   JR done\n"
                       "event:  LD HL,count\n"
                       "        INC (HL)\n"
                       "        LD BC,0\n"             // the interrupted program must not see this
                       "        RET\n"
                       "count:  DB 0\n"
                       "block:  DS 9\n"
                       "inks:   DB &14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14,&14\n"));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        const Json r = ask(s, R"({"cmd":"run","until":"done","max_instructions":200000,"irq_every_cycles":13312})");
        CHECK_EQ(str(r, "stop"), std::string("address"));
        CHECK_EQ(str(*r.find("regs"), "BC"), std::string("0x1234"));
        const Json v = ask(s, R"({"cmd":"video"})");
        CHECK(has(v.dump(), "\"pen\":1,\"hw\":12"));  // the firmware's ink table won at the flyback
    }

    // ---- joystick and keys ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  CALL &BB24\n"          // KM_GET_JOYSTICK
                       "        LD (joy),A\n"
                       "        LD A,47\n"
                       "        CALL &BB1E\n"          // KM_TEST_KEY space
                       "        LD A,0\n"
                       "        JR Z,up\n"
                       "        LD A,1\n"
                       "up:     LD (key),A\n"
                       "done:   JR done\n"
                       "joy:    DB 0\n"
                       "key:    DB 0\n"));
        CHECK(ok(ask(s, R"({"cmd":"controls","joystick":16,"keys":[47]})")));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        runTo(s, "done");
        const Json sym = *ask(s, R"({"cmd":"symbols"})").find("symbols");
        const unsigned joy = static_cast<unsigned>(std::stoul(str(sym, "JOY"), nullptr, 16));
        const auto vals = readMem(s, joy, 2);
        CHECK_EQ(vals[0], Byte{16});
        CHECK_EQ(vals[1], Byte{1});
    }

    // ---- the jump block holds ROM addresses: MC WAIT FLYBACK called directly in the lower ROM; PCHL at &001E ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  CALL &B906\n"          // KL_L_ROM_ENABLE
                       "        DI\n"
                       "        LD HL,(&BD1A)\n"       // the address in the entry of MC WAIT FLYBACK
                       "        RES 7,H\n"
                       "        RES 6,H\n"
                       "        CALL &001E\n"          // JP (HL)
                       "        CALL &B909\n"          // KL_L_ROM_DISABLE (ends with EI)
                       "done:   JR done\n"));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        const Json r = ask(s, R"({"cmd":"run","until":"done","max_instructions":1000})");
        CHECK_EQ(str(r, "stop"), std::string("address"));
        CHECK(r.find("regs")->find("IFF1")->asBool());
        CHECK(has(ask(s, R"({"cmd":"firmware_log"})").dump(), "MC_WAIT_FLYBACK"));
    }

    // ---- raster splits: mode 0 at the top, mode 1 further down; coverage of what ran ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  LD BC,&7F00\n"
                       "        EI\n"
                       "loop:   HALT\n"                // the interrupt in the vertical sync
                       "        LD C,&8C\n"
                       "        OUT (C),C\n"           // mode 0
                       "        HALT\n"
                       "        HALT\n"
                       "        HALT\n"
                       "        LD C,&8D\n"
                       "        OUT (C),C\n"           // mode 1, about 100 lines further down
                       "        HALT\n"
                       "        HALT\n"
                       "        JR loop\n"
                       "never:  NOP\n"));
        CHECK(ok(ask(s, R"({"cmd":"coverage","on":true})")));
        CHECK(ok(ask(s, R"({"cmd":"set","regs":{"PC":"start","SP":"0xBF00"}})")));
        CHECK(ok(ask(s, R"({"cmd":"run","max_instructions":3000,"irq_every_cycles":13312})")));
        const Json scr = ask(s, R"({"cmd":"screen","name":"x"})");
        CHECK(ok(scr));
        const Json* splits = scr.find("raster_splits");
        CHECK(splits && splits->isArray() && splits->items().size() >= 2);
        const Json cov = ask(s, R"({"cmd":"coverage","addresses":true,"from":"0x4000","to":"0x4020"})");
        const std::string c = cov.dump();
        CHECK(has(c, "\"0x4000\""));
        const Json sym = *ask(s, R"({"cmd":"symbols"})").find("symbols");
        CHECK(!has(c, "\"" + str(sym, "NEVER") + "\""));
    }

    // ---- the analysis: RST &18 + an address, print codes with argument bytes in inline texts ----
    {
        Session s(withFiles());
        makeCpc(s);
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  RST &18\n"
                       "        DW far\n"
                       "        CALL print\n"
                       "        DB &1F,&80,&10,\"HI\",'!'+&80\n"   // &80 is an argument of &1F, not the end
                       "        LD A,1\n"
                       "        RET\n"
                       "print:  EX (SP),HL\n"
                       "        EX (SP),HL\n"
                       "        RET\n"
                       "far:    DW &1234\n"
                       "        DB 7\n"));
        const Json an = ask(s, R"({"cmd":"analyze","entries":["start"],"inline_words":["0x18"],"inline_strings":["print"],"inline_control_args":{"0x1F":2},"from":"0x4000","to":"0x4017"})");
        CHECK(ok(an));
        const Json l = ask(s, R"({"cmd":"listing","verify":true})");
        CHECK(l.find("verified")->asBool());
        const std::string text = str(l, "text");
        CHECK(has(text, "RST &18"));
        CHECK(has(text, "DW data_4012") || has(text, "DW far"));
        CHECK(has(text, "LD A,&01"));  // the code behind both inline blocks was found
    }

    std::filesystem::remove_all(dir);
    return finish("firmware");
}
