#include <filesystem>
#include <fstream>

#include "test_util.hpp"
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

const char* kDemo =
    "SCR_SET_MODE EQU &BC0E\n"
    "SCR_SET_INK  EQU &BC32\n"
    "TXT_OUTPUT   EQU &BB5A\n"
    "        ORG &4000\n"
    "start:  LD A,1\n"
    "        CALL SCR_SET_MODE\n"
    "        LD A,1\n"
    "        LD B,26\n"
    "        LD C,26\n"
    "        CALL SCR_SET_INK\n"
    "        LD HL,msg\n"
    "        CALL print\n"
    "        LD DE,&C78A\n"
    "        LD HL,sprite\n"
    "        CALL draw8\n"
    "        LD A,(counter)\n"
    "        INC A\n"
    "        LD (counter),A\n"
    "        RET\n"
    "print:  LD A,(HL)\n"
    "        OR A\n"
    "        RET Z\n"
    "        CALL TXT_OUTPUT\n"
    "        INC HL\n"
    "        JR print\n"
    "draw8:  LD B,8\n"
    "d1:     LD A,(HL)\n"
    "        LD (DE),A\n"
    "        INC HL\n"
    "        LD A,D\n"
    "        ADD A,8\n"
    "        LD D,A\n"
    "        DJNZ d1\n"
    "        RET\n"
    "msg:    DB \"HELLO\",0\n"
    "sprite: DB &FF,&81,&BD,&A5,&A5,&BD,&81,&FF\n"
    "        ORG &A000\n"
    "counter: DB 0\n";

}  // namespace

int main() {
    // ---- protocol version: the orchestrator refuses an outdated build ----
    {
        Session s;
        const Json info = ask(s, R"({"cmd":"info"})");
        CHECK(ok(info));
        CHECK(info.find("protocol")->asInt() >= 3);
    }

    // ---- CPC machine: run, screen, console, firmware log ----
    {
        Session s;
        CHECK(!ok(ask(s, R"({"cmd":"screen"})")));  // not a CPC yet
        CHECK(ok(ask(s, R"({"cmd":"machine","type":"cpc"})")));
        CHECK(!ok(ask(s, R"({"cmd":"machine","type":"zx"})")));
        CHECK_EQ(s.cpu().regs().sp, Word{0xBFFF});  // not inside the screen RAM
        CHECK(ok(ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(kDemo) + "}")));

        const Json before = ask(s, R"({"cmd":"screen","name":"before"})");
        CHECK(ok(before));
        CHECK(has(str(before, "png_base64"), "iVBORw0KGgo"));  // base64 of the PNG signature
        CHECK_EQ(before.find("width")->asInt(), std::int64_t{640});

        const Json call = ask(s, R"({"cmd":"call","addr":"start","max_instructions":100000})");
        CHECK(ok(call));
        CHECK(call.find("returned")->asBool());
        CHECK_EQ(str(call, "stop"), std::string("address"));

        CHECK_EQ(str(ask(s, R"({"cmd":"console"})"), "text"), std::string("HELLO"));
        CHECK_EQ(str(ask(s, R"({"cmd":"console","clear":true})"), "text"), std::string("HELLO"));
        CHECK_EQ(str(ask(s, R"({"cmd":"console"})"), "text"), std::string(""));

        const Json log = ask(s, R"({"cmd":"firmware_log"})");
        const auto& calls = log.find("calls")->items();
        CHECK_EQ(calls.size(), std::size_t{7});  // SET_MODE, SET_INK, 5 x TXT_OUTPUT
        CHECK_EQ(str(calls[0], "name"), std::string("SCR_SET_MODE"));
        CHECK_EQ(str(calls[1], "name"), std::string("SCR_SET_INK"));
        CHECK_EQ(str(calls[2], "A"), std::string("0x48"));  // 'H'

        const Json diff = ask(s, R"({"cmd":"screen_diff","a":"before"})");
        CHECK(ok(diff));
        CHECK(diff.find("changed_pixels")->asInt() > 0);
        CHECK_EQ(diff.find("text_cells")->find("col0")->asInt(), std::int64_t{10});
        CHECK_EQ(diff.find("text_cells")->find("row0")->asInt(), std::int64_t{24});
        CHECK(!ok(ask(s, R"({"cmd":"screen_diff","a":"nope"})")));

        // zoomed crop of the changed cell: small and enlarged
        const Json crop = ask(s, R"({"cmd":"screen","name":"zoom","crop":{"col0":9,"row0":23,"col1":11,"row1":24},"zoom":4})");
        CHECK(ok(crop));
        CHECK_EQ(crop.find("width")->asInt(), std::int64_t{3 * 8 * 4});
        CHECK_EQ(crop.find("height")->asInt(), std::int64_t{2 * 16 * 4});
        CHECK(!ok(ask(s, R"({"cmd":"screen","crop":{"col0":5,"row0":5,"col1":2,"row1":5}})")));
        CHECK(!ok(ask(s, R"({"cmd":"screen","crop":{"col0":5}})")));

        // the palette change done by SCR_SET_INK is visible in the video state
        const Json video = ask(s, R"({"cmd":"video"})");
        CHECK_EQ(str(video.find("pens")->items()[1], "colour"), std::string("bright white"));
        CHECK_EQ(video.find("mode")->asInt(), std::int64_t{1});

        CHECK_EQ(ask(s, R"({"cmd":"screens"})").find("screens")->items().size(), std::size_t{3});
        CHECK(ok(ask(s, R"({"cmd":"screen_drop","name":"zoom"})")));
        CHECK_EQ(ask(s, R"({"cmd":"screens"})").find("screens")->items().size(), std::size_t{2});
    }

    // ---- checkpoints restore the video state, too ----
    {
        Session s;
        ask(s, R"({"cmd":"machine","type":"cpc"})");
        ask(s, R"({"cmd":"asm","source":"SCR_SET_MODE EQU &BC0E\nSCR_SET_INK EQU &BC32\nORG &4000\nfn: LD A,2\nCALL SCR_SET_MODE\nLD A,1\nLD B,0\nLD C,0\nCALL SCR_SET_INK\nRET\n"})");
        ask(s, R"({"cmd":"checkpoint","name":"base"})");
        CHECK_EQ(ask(s, R"({"cmd":"video"})").find("mode")->asInt(), std::int64_t{1});
        CHECK(ok(ask(s, R"({"cmd":"call","addr":"fn"})")));
        const Json changed = ask(s, R"({"cmd":"video"})");
        CHECK_EQ(changed.find("mode")->asInt(), std::int64_t{2});
        CHECK_EQ(str(changed.find("pens")->items()[1], "colour"), std::string("black"));
        CHECK(ok(ask(s, R"({"cmd":"rollback","name":"base"})")));
        const Json restored = ask(s, R"({"cmd":"video"})");
        CHECK_EQ(restored.find("mode")->asInt(), std::int64_t{1});
        CHECK_EQ(str(restored.find("pens")->items()[1], "colour"), std::string("bright yellow"));
    }

    // ---- analysis, routines, annotations, listing ----
    {
        Session s;
        ask(s, R"({"cmd":"machine","type":"cpc"})");
        ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(kDemo) + "}");
        CHECK(!ok(ask(s, R"({"cmd":"routines"})")));  // nothing analysed yet
        CHECK(!ok(ask(s, R"({"cmd":"listing"})")));

        const Json an = ask(s, R"({"cmd":"analyze","entries":["start"]})");
        CHECK(ok(an));
        CHECK_EQ(str(an, "from"), std::string("0x4000"));

        const Json routines = ask(s, R"({"cmd":"routines"})");
        const auto& list = routines.find("routines")->items();
        CHECK_EQ(list.size(), std::size_t{3});  // print, draw8 and the entry point; firmware calls are listed separately
        CHECK_EQ(str(list[2], "name"), std::string("sub_4000"));  // bottom-up: the entry calls the others, so it comes last
        CHECK_EQ(routines.find("external_calls")->items().size(), std::size_t{3});
        bool sawFirmware = false;
        for (const Json& e : routines.find("external_calls")->items()) {
            if (str(e, "firmware") == "TXT_OUTPUT") sawFirmware = true;
        }
        CHECK(sawFirmware);
        CHECK_EQ(str(list[0], "name"), std::string("sub_4025"));
        CHECK(!list[0].find("documented")->asBool());
        CHECK_EQ(list[0].find("calls")->items().size(), std::size_t{1});  // print calls TXT_OUTPUT

        auto annotate = [&](const std::string& json) { return ask(s, json); };
        CHECK(ok(annotate(R"({"cmd":"ann_routine","addr":"print","name":"print_string","summary":"Prints the string at HL.","inputs":"HL = zero terminated text","clobbers":"AF HL"})")));
        CHECK(!ok(annotate(R"({"cmd":"ann_routine","addr":"draw8","name":"draw_sprite_8x8","summary":"x","confidence":"certain"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_routine","addr":"draw8","name":"draw_sprite_8x8","summary":"x","confidence":"low"})")));
        CHECK_EQ(str(ask(s, R"({"cmd":"routines"})").find("routines")->items()[1], "confidence"), std::string("low"));
        CHECK(ok(annotate(R"({"cmd":"ann_remove","addr":"draw8"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_label","addr":"draw8","name":"draw_sprite_8x8"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_variable","addr":"counter","name":"run_counter","size":1,"comment":"number of runs"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_data","addr":"msg","length":6,"kind":"text","name":"greeting"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_data","addr":"sprite","length":8,"kind":"bytes","name":"face_sprite"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_comment","addr":"start","text":"entry point"})")));
        CHECK(ok(annotate(R"({"cmd":"ann_title","text":"Hello demo"})")));

        // wrong names are refused with an explanation the model can act on
        const Json bad = annotate(R"({"cmd":"ann_label","addr":"0x4000","name":"LD"})");
        CHECK(!ok(bad));
        CHECK(has(str(bad, "error"), "reserved"));
        CHECK(has(str(annotate(R"({"cmd":"ann_label","addr":"0x4000","name":"TXT_OUTPUT"})"), "error"), "firmware"));
        CHECK(has(str(annotate(R"({"cmd":"ann_label","addr":"0x4000","name":"print_string"})"), "error"), "already used"));
        CHECK(!ok(annotate(R"({"cmd":"ann_data","addr":"msg","length":3,"kind":"words"})")));
        CHECK(!ok(annotate(R"({"cmd":"ann_data","addr":"msg","length":3,"kind":"sprites"})")));
        CHECK(!ok(annotate(R"({"cmd":"ann_variable","addr":"0xA100","name":"x","size":0})")));

        const Json routines2 = ask(s, R"({"cmd":"routines","undocumented_only":true})");
        CHECK_EQ(routines2.find("routines")->items().size(), std::size_t{2});  // draw_sprite_8x8 and the entry are still open
        CHECK_EQ(str(routines2.find("routines")->items()[0], "name"), std::string("draw_sprite_8x8"));

        const Json listing = ask(s, R"({"cmd":"listing"})");
        CHECK(ok(listing));
        CHECK(listing.find("verified")->asBool());
        const std::string text = str(listing, "text");
        CHECK(has(text, "; Hello demo"));
        CHECK(has(text, "CALL SCR_SET_MODE"));
        CHECK(has(text, "print_string:"));
        CHECK(has(text, "LD A,(run_counter)"));
        CHECK(has(text, "LD HL,greeting"));
        CHECK(has(text, "greeting:"));
        CHECK(has(text, "screen at &C000: char row 24, byte column 10, scan line 0"));
        CHECK(has(text, "entry point"));
        CHECK(!has(text, "sub_"));      // everything is named, except draw8's header (documented text mentions the name)
        CHECK_EQ(listing.find("auto_labels")->asInt(), std::int64_t{1});  // loc for the d1 loop

        const Json capped = ask(s, R"({"cmd":"listing","max_chars":1000})");
        CHECK(capped.find("truncated")->asBool());
        CHECK_EQ(str(capped, "text").size(), std::size_t{1000});

        const Json all = ask(s, R"({"cmd":"ann_list"})");
        CHECK_EQ(all.find("annotations")->find("routines")->items().size(), std::size_t{1});
        CHECK(ok(ask(s, R"({"cmd":"ann_remove","addr":"draw8"})")));
        CHECK(has(str(ask(s, R"({"cmd":"listing"})"), "text"), "sub_402E"));  // back to the automatic name
    }

    // ---- files: annotations, snapshot, ROMs ----
    {
        const auto dir = std::filesystem::temp_directory_path();
        const auto annFile = dir / "z80tools_reverse_test.json";
        const auto snaFile = dir / "z80tools_reverse_test.sna";
        const auto romFile = dir / "z80tools_reverse_test.rom";

        // a synthetic snapshot: registers, mode 2, one pixel row, RAM at &4000
        std::vector<Byte> sna(0x100 + 0x10000, 0);
        const char magic[] = "MV - SNA";
        std::copy(magic, magic + 8, sna.begin());
        sna[0x10] = 3;
        sna[0x23] = 0x00;
        sna[0x24] = 0x40;  // PC = 4000
        sna[0x21] = 0x00;
        sna[0x22] = 0xB0;  // SP = B000
        sna[0x40] = 0x8E;  // mode 2, lower and upper ROM off
        sna[0x6B] = 64;
        sna[0x43 + 1] = 40;
        sna[0x43 + 6] = 25;
        sna[0x43 + 9] = 7;
        sna[0x43 + 12] = 0x30;
        for (int i = 0; i < 16; ++i) sna[static_cast<std::size_t>(0x2F + i)] = 0x14;
        sna[0x30] = 0x0B;  // pen 1 = bright white
        sna[0x100 + 0x4000] = 0x3E;  // LD A,&42 ; RET at &4000
        sna[0x100 + 0x4001] = 0x42;
        sna[0x100 + 0x4002] = 0xC9;
        sna[0x100 + 0xC000] = 0xFF;  // 8 white pixels in the top left corner
        { std::ofstream f(snaFile, std::ios::binary); f.write(reinterpret_cast<const char*>(sna.data()), static_cast<std::streamsize>(sna.size())); }
        // 16 KiB lower ROM: "ROM" at address 0, a firmware routine at 0x1000
        std::vector<Byte> rom(0x4000, 0xC9);
        rom[0] = 'R';
        rom[1] = 'O';
        rom[2] = 'M';
        { std::ofstream f(romFile, std::ios::binary); f.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size())); }

        SessionOptions options;
        options.allowFiles = true;
        Session s(options);
        const Json loaded = ask(s, "{\"cmd\":\"load_sna\",\"file\":" + quote(snaFile.string()) + "}");
        CHECK(ok(loaded));
        CHECK_EQ(str(*loaded.find("regs"), "PC"), std::string("0x4000"));
        CHECK_EQ(loaded.find("video")->find("mode")->asInt(), std::int64_t{2});
        CHECK_EQ(str(loaded.find("video")->find("pens")->items()[1], "colour"), std::string("bright white"));
        CHECK(ok(ask(s, R"({"cmd":"step","n":2})")));
        CHECK_EQ(str(*ask(s, R"({"cmd":"regs"})").find("regs"), "A"), std::string("0x42"));

        const Json shot = ask(s, R"({"cmd":"screen"})");
        CHECK_EQ(shot.find("mode")->asInt(), std::int64_t{2});

        // snapshots without the right header are refused
        { std::ofstream f(annFile); f << "not a snapshot"; }
        CHECK(!ok(ask(s, "{\"cmd\":\"load_sna\",\"file\":" + quote(annFile.string()) + "}")));

        // lower ROM: visible to the CPU only while enabled; the Gate Array switches it
        CHECK(ok(ask(s, "{\"cmd\":\"load_rom\",\"file\":" + quote(romFile.string()) + ",\"which\":\"lower\"}")));
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":0,"len":3,"view":"cpu"})"), "hex"), std::string("000000"));  // ROM still off
        ask(s, R"({"cmd":"asm","source":"ORG &5000\nLD BC,&7F89\nOUT (C),C\nLD BC,&7F8D\nOUT (C),C\nRET\n","entry":20480})");
        ask(s, R"({"cmd":"step","n":2})");  // LD BC / OUT -> mode 1 with lower ROM ON (&89: bit 2 = 0)
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":0,"len":3,"view":"cpu"})"), "hex"), std::string("524F4D"));  // "ROM"
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":0,"len":3})"), "hex"), std::string("000000"));              // RAM view is the default
        ask(s, R"({"cmd":"step","n":2})");  // switch it off again (&8D)
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":0,"len":3,"view":"cpu"})"), "hex"), std::string("000000"));
        CHECK(!ok(ask(s, "{\"cmd\":\"load_rom\",\"file\":" + quote(romFile.string()) + ",\"which\":\"sideways\"}")));
        CHECK(!ok(ask(s, "{\"cmd\":\"load_rom\",\"file\":" + quote(romFile.string()) + ",\"which\":\"both\"}")));  // needs 32 KiB

        // annotations survive a save/load round trip
        ask(s, R"({"cmd":"ann_label","addr":"0x4000","name":"entry"})");
        ask(s, R"({"cmd":"ann_comment","addr":"0x4000","text":"sets A"})");
        CHECK(ok(ask(s, "{\"cmd\":\"ann_save\",\"file\":" + quote(annFile.string()) + "}")));
        Session t(options);
        CHECK(ok(ask(t, "{\"cmd\":\"ann_load\",\"file\":" + quote(annFile.string()) + "}")));
        CHECK_EQ(str(*ask(t, R"({"cmd":"ann_list"})").find("annotations")->find("labels"), "0x4000"), std::string("entry"));

        // corrupt project files are refused, not trusted
        { std::ofstream f(annFile); f << R"({"labels":{"0x4000":"a","0x4001":"a"}})"; }
        CHECK(!ok(ask(t, "{\"cmd\":\"ann_load\",\"file\":" + quote(annFile.string()) + "}")));
        { std::ofstream f(annFile); f << "garbage"; }
        CHECK(!ok(ask(t, "{\"cmd\":\"ann_load\",\"file\":" + quote(annFile.string()) + "}")));

        // without --allow-files none of this is possible
        Session locked;
        for (const char* cmd : {"load_sna", "load_rom", "ann_save", "ann_load"}) {
            const Json r = ask(locked, std::string("{\"cmd\":\"") + cmd + "\",\"file\":\"/etc/passwd\",\"which\":\"lower\"}");
            CHECK(!ok(r));
            CHECK(has(str(r, "error"), "disabled"));
        }
        for (const auto& p : {annFile, snaFile, romFile}) std::filesystem::remove(p);
    }

    // ---- periodic interrupts while running ----
    {
        Session s;
        ask(s, R"({"cmd":"asm","source":"ORG &100\nIM 1\nEI\nloop: JR loop\nORG &38\nINC B\nEI\nRETI\n","entry":256})");
        ask(s, R"({"cmd":"set","regs":{"SP":"0x9000"}})");
        const Json r = ask(s, R"({"cmd":"run","irq_every_cycles":1000,"max_cycles":20000,"max_instructions":0})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("cycle_limit"));
        const int b = std::stoi(str(*r.find("regs"), "BC").substr(2, 2), nullptr, 16);
        CHECK(b >= 15 && b <= 25);  // roughly one interrupt per 1000 T-states
        // without the option nothing interrupts
        ask(s, R"({"cmd":"set","regs":{"BC":0,"PC":256}})");
        const Json quiet = ask(s, R"({"cmd":"run","max_instructions":1000})");
        CHECK_EQ(str(*quiet.find("regs"), "BC"), std::string("0x0000"));
    }

    return finish("reverse");
}
