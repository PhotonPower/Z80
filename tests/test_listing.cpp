#include "test_util.hpp"
#include "z80/annotations.hpp"
#include "z80/assembler.hpp"
#include "z80/disassembler.hpp"
#include "z80/listing.hpp"

using namespace z80;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void expectIn(const std::string& text, const std::string& part, int line) {
    ++g_checks;
    if (!has(text, part)) {
        ++g_failures;
        std::cerr << "test_listing.cpp:" << line << ": expected \"" << part << "\" in:\n" << text << "\n";
    }
}
void expectNotIn(const std::string& text, const std::string& part, int line) {
    ++g_checks;
    if (has(text, part)) {
        ++g_failures;
        std::cerr << "test_listing.cpp:" << line << ": unexpected \"" << part << "\" in:\n" << text << "\n";
    }
}
#define EXPECT_IN(text, part) expectIn((text), (part), __LINE__)
#define EXPECT_NOT_IN(text, part) expectNotIn((text), (part), __LINE__)

// A small CPC-style program: set mode and a pen, print a message through the firmware, count calls.
const char* kProgram =
    "        ORG &4000\n"
    "        LD BC,&7F8D        ; mode 1, ROMs off\n"
    "        OUT (C),C\n"
    "        LD B,&7F\n"
    "        LD A,&10\n"
    "        OUT (C),A\n"
    "        LD BC,&BC0C\n"
    "        OUT (C),C\n"
    "        LD HL,&4080\n"
    "        CALL &4040\n"
    "        LD A,(&A010)\n"
    "        INC A\n"
    "        LD (&A010),A\n"
    "        LD HL,(&A020)\n"
    "        LD DE,&A020\n"
    "        JP &4030\n"
    "        ORG &4030\n"
    "wait:   DEC A\n"
    "        JR NZ,wait\n"
    "        RET\n"
    "        ORG &4040\n"
    "print:  LD A,(HL)\n"
    "        INC HL\n"
    "        OR A\n"
    "        RET Z\n"
    "        CALL &BB5A\n"
    "        JR print\n"
    "        ORG &4080\n"
    "        DB \"Hello\",0\n"
    "        DB 1,2,3,4\n"
    "        DW &4040,&4030\n";

struct Fixture {
    Memory memory;
    Disassembler dis{memory};
    Annotations ann;
    Fixture() {
        const auto r = Assembler().assemble(kProgram);
        if (!r.ok()) std::cerr << r.formatDiagnostics("program");
        r.applyTo(memory);
        dis.analyzeFlow(0x4000);
    }
};

}  // namespace

int main() {
    // ---- annotations: names are validated ----
    {
        Annotations a;
        CHECK(!a.setLabel(0x4000, "main_loop").has_value());
        CHECK_EQ(*a.nameAt(0x4000), std::string("main_loop"));
        CHECK(a.setLabel(0x4001, "main_loop").has_value());         // duplicate name, other address
        CHECK(a.setLabel(0x4001, "MAIN_LOOP").has_value());         // case-insensitive
        CHECK(!a.setLabel(0x4000, "renamed").has_value());          // renaming the same address is fine
        CHECK(!a.nameAt(0x4000)->empty() && *a.nameAt(0x4000) == "renamed");
        CHECK(!a.setLabel(0x4001, "main_loop").has_value());        // the old name is free again
        for (const char* bad : {"", "1abc", "has space", "a-b", "LD", "hl", "NZ", "DB", "high", "TXT_OUTPUT", "gate_array", "sub_4010", "L_ABCD",
                                "loc_00ff", "ünicode"}) {
            ++g_checks;
            if (!a.setLabel(0x5000, bad).has_value()) {
                ++g_failures;
                std::cerr << "name should be rejected: '" << bad << "'\n";
            }
        }
        CHECK(!Annotations::checkName("draw_sprite_2").has_value());
        CHECK(a.setLabel(0x5000, std::string(60, 'a')).has_value());

        CHECK(!a.setVariable({"score", 0xA000, 2, "player score"}).has_value());
        CHECK_EQ(*a.expressionFor(0xA000), std::string("score"));
        CHECK_EQ(*a.expressionFor(0xA001), std::string("score+1"));
        CHECK(!a.expressionFor(0xA002).has_value());
        CHECK(a.setVariable({"zero", 0xA100, 0, ""}).has_value());
        CHECK(a.setVariable({"huge", 0xFFF0, 100, ""}).has_value());
        CHECK(a.setData({0x6000, 3, DataKind::Words, "", ""}).has_value());  // odd length for words
        CHECK(!a.setData({0x6000, 4, DataKind::Words, "vectors", "jump table"}).has_value());
        CHECK_EQ(*a.expressionFor(0x6002), std::string("vectors+2"));

        const RoutineDoc* none = a.routineAt(0x4000);
        CHECK(none == nullptr);
        CHECK(!a.setRoutine(0x4100, {"clear_screen", "Clears the screen", "A = pen", "", "AF", "medium"}).has_value());
        CHECK_EQ(a.routineAt(0x4100)->summary, std::string("Clears the screen"));
        CHECK_EQ(a.routineAt(0x4100)->confidence, std::string("medium"));
        CHECK(a.setRoutine(0x4200, {"other_routine", "x", "", "", "", "certain"}).has_value());  // only high/medium/low

        // JSON round trip
        a.setComment(0x4000, "entry");
        a.setBlockComment(0x4100, "section: screen");
        a.setTitle("Test");
        const Annotations b = Annotations::fromJson(Json::parse(a.toJson().dump()));
        CHECK_EQ(b.toJson().dump(), a.toJson().dump());
        CHECK_EQ(*b.addressOf("SCORE"), Word{0xA000});
        CHECK(*b.commentAt(0x4000) == "entry");

        // removing
        CHECK(a.removeLabel(0xA000));
        CHECK(!a.nameAt(0xA000).has_value());
        CHECK(!a.addressOf("score").has_value());
        CHECK(a.variables().empty());
        CHECK(!a.removeLabel(0xA000));
        CHECK(a.removeComment(0x4000));

        bool threw = false;
        try { Annotations::fromJson(Json::parse(R"({"labels":{"0x4000":"a","0x4001":"a"}})")); } catch (const JsonError&) { threw = true; }
        CHECK(threw);  // duplicate names in a project file are rejected
        threw = false;
        try { Annotations::fromJson(Json::parse(R"({"labels":{"0x4000":"LD"}})")); } catch (const JsonError&) { threw = true; }
        CHECK(threw);
    }

    // ---- listing without any annotations: still complete and verified ----
    {
        Fixture f;
        const ListingResult r = renderListing(f.memory, f.dis, f.ann);
        EXPECT_IN(r.text, "ORG &4000");
        EXPECT_IN(r.text, "sub_4040:");                  // call target gets an automatic name
        EXPECT_IN(r.text, "CALL sub_4040");
        EXPECT_IN(r.text, "CALL TXT_OUTPUT");            // firmware call by name
        EXPECT_IN(r.text, "TXT_OUTPUT");
        EXPECT_IN(r.text, "EQU &BB5A");
        EXPECT_IN(r.text, "LD BC,GATE_ARRAY+&8D");
        EXPECT_IN(r.text, "LD B,HIGH(GATE_ARRAY)");
        EXPECT_IN(r.text, "LD BC,CRTC_SELECT+&0C");
        EXPECT_IN(r.text, "GATE_ARRAY");
        EXPECT_IN(r.text, "Gate Array: screen mode 1, lower ROM off, upper ROM off");
        EXPECT_IN(r.text, "CRTC: select register 12 (display start address (high))");
        EXPECT_IN(r.text, "Gate Array: select border");  // LD A,&10 / OUT (C),A with the value tracked
        EXPECT_IN(r.text, "LD A,(&A010)");              // nothing named yet -> numeric
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.verifyDetail << "\n" << r.text << "\n";
        CHECK(r.autoLabels >= 2);
    }

    // ---- listing with annotations ----
    {
        Fixture f;
        CHECK(!f.ann.setRoutine(0x4040, {"print_string", "Prints the zero terminated string at HL with the firmware.",
                                         "HL = address of the string", "HL points behind the terminator", "AF HL", ""}).has_value());
        CHECK(!f.ann.setRoutine(0x4000, {"main", "Sets up the screen and prints the greeting.", "", "", "", "low"}).has_value());
        CHECK(!f.ann.setLabel(0x4030, "delay").has_value());
        CHECK(!f.ann.setVariable({"call_count", 0xA010, 1, "number of times main ran"}).has_value());
        CHECK(!f.ann.setVariable({"cursor_pos", 0xA020, 2, "text cursor position"}).has_value());
        CHECK(!f.ann.setData({0x4080, 6, DataKind::Text, "greeting", "zero terminated"}).has_value());
        CHECK(!f.ann.setData({0x4086, 4, DataKind::Bytes, "pen_table", ""}).has_value());
        CHECK(!f.ann.setData({0x408A, 4, DataKind::Pointers, "routine_table", "addresses of routines"}).has_value());
        f.ann.setComment(0x4000, "program entry");
        f.ann.setBlockComment(0x4030, "Busy-wait loop; A = number of iterations");
        f.ann.setTitle("Greeting demo");

        ListingOptions all;
        all.to = Word{0x408D};  // include the data behind the code
        const ListingResult r = renderListing(f.memory, f.dis, f.ann, all);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.verifyDetail << "\n" << r.text << "\n";
        EXPECT_IN(r.text, "; Greeting demo");
        EXPECT_IN(r.text, "main:");
        EXPECT_IN(r.text, "print_string:");
        EXPECT_IN(r.text, "; print_string");
        EXPECT_IN(r.text, ";   in:       HL = address of the string");
        EXPECT_IN(r.text, ";   out:      HL points behind the terminator");
        EXPECT_IN(r.text, ";   clobbers: AF HL");
        EXPECT_IN(r.text, "called from: 4013");
        EXPECT_IN(r.text, "CALL print_string");
        EXPECT_IN(r.text, ";   confidence: low - a guess, please verify");     // shown only for routines that are not "high"
        EXPECT_NOT_IN(r.text, "confidence: high");
        EXPECT_IN(r.text, "JP delay");
        EXPECT_IN(r.text, "; Busy-wait loop");
        EXPECT_IN(r.text, "LD A,(call_count)");
        EXPECT_IN(r.text, "LD (call_count),A");
        EXPECT_IN(r.text, "LD HL,(cursor_pos)");
        EXPECT_IN(r.text, "LD DE,cursor_pos");           // immediate that equals a named variable
        EXPECT_IN(r.text, "LD HL,greeting");              // ... and a named data label inside the listing
        EXPECT_IN(r.text, "call_count");
        EXPECT_IN(r.text, "EQU &A010");                   // variables outside the listing become EQUs
        EXPECT_IN(r.text, "number of times main ran");
        EXPECT_IN(r.text, "greeting:");
        EXPECT_IN(r.text, "DB \"Hello\",&00");
        EXPECT_IN(r.text, "pen_table:");
        EXPECT_IN(r.text, "DB &01,&02,&03,&04");
        EXPECT_IN(r.text, "routine_table:");
        EXPECT_IN(r.text, "DW print_string,delay");       // pointer table shows names
        EXPECT_IN(r.text, "; program entry");
        EXPECT_NOT_IN(r.text, "sub_4040");                // everything that is named no longer shows automatic names
        EXPECT_NOT_IN(r.text, "loc_4030");
        EXPECT_NOT_IN(r.text, "L_");

        // naming something changes the text but never the bytes
        CHECK(!f.ann.setLabel(0x4000, "start_here").has_value());  // rename a routine label
        const ListingResult again = renderListing(f.memory, f.dis, f.ann, all);
        CHECK(again.verified);
        EXPECT_IN(again.text, "start_here:");
        EXPECT_NOT_IN(again.text, "\nmain:");
    }

    // ---- sub-ranges and names inside instructions ----
    {
        Fixture f;
        ListingOptions options;
        options.from = Word{0x4040};
        options.to = Word{0x404B};
        const ListingResult r = renderListing(f.memory, f.dis, f.ann, options);
        CHECK(r.verified);
        EXPECT_IN(r.text, "ORG &4040");
        EXPECT_NOT_IN(r.text, "GATE_ARRAY");

        // a range that ends in the middle of an instruction shows the cut bytes as data
        ListingOptions cut;
        cut.from = Word{0x4000};
        cut.to = Word{0x4001};
        const ListingResult c = renderListing(f.memory, f.dis, f.ann, cut);
        CHECK(c.verified);
        EXPECT_IN(c.text, "DB &01,&8D");

        // a name that points into the middle of an instruction becomes an EQU, not a broken label
        Fixture g;
        CHECK(!g.ann.setLabel(0x4001, "mid").has_value());
        const ListingResult m = renderListing(g.memory, g.dis, g.ann);
        CHECK(m.verified);
        EXPECT_IN(m.text, "mid");
        EXPECT_IN(m.text, "EQU &4001");
    }

    // ---- data regions and run compression ----
    {
        Memory memory;
        std::vector<Byte> code = {0xC9};  // RET at 0x1000
        memory.load(0x1000, code);
        for (int i = 0; i < 40; ++i) memory.write(static_cast<Word>(0x1001 + i), 0x55);
        memory.write(0x1029, 0x00);
        memory.write(0x102A, 'H' | 0x80);  // last letter with bit 7 set, as used by many CPC text routines
        Disassembler dis(memory);
        dis.analyzeFlow(0x1000);
        Annotations ann;
        CHECK(!ann.setData({0x102A, 1, DataKind::Text, "last_char", ""}).has_value());
        ListingOptions options;
        options.to = Word{0x102A};
        const ListingResult r = renderListing(memory, dis, ann, options);
        CHECK(r.verified);
        if (!r.verified) std::cerr << r.verifyDetail << "\n" << r.text << "\n";
        EXPECT_IN(r.text, "DS 40,&55");
        EXPECT_IN(r.text, "'H'+&80");
    }

    // ---- idioms that are easy to misread ----
    {
        Memory memory;
        const auto r = Assembler().assemble(
            "        ORG &0600\n"
            "        DI\n"
            "        LD SP,&C000\n"
            "        IM 1\n"
            "        LD A,&C3\n"
            "        LD (&0038),A\n"
            "        LD HL,&1D93\n"
            "        LD (&0039),HL\n"
            "        LD A,&05\n"
            "        LD (&0010),A\n"
            "        LD (&1C97),A\n"
            "        EI\n"
            "        HALT\n"
            "        RET\n");
        r.applyTo(memory);
        Disassembler dis(memory);
        dis.analyzeFlow(0x0600);
        const ListingResult l = renderListing(memory, dis, Annotations{});
        CHECK(l.verified);
        EXPECT_IN(l.text, "stack starts at &C000 and grows DOWNWARDS");
        EXPECT_IN(l.text, "interrupt mode 1: every interrupt jumps to &0038");
        EXPECT_IN(l.text, "stores the JP opcode (&C3) at &0038");
        EXPECT_IN(l.text, "the interrupt handler is at &1D93");
        EXPECT_IN(l.text, "writes into the RST / interrupt vector area");   // &0010 with A=5: not the JP idiom
        EXPECT_IN(l.text, "waits for the next interrupt");
        // user comments replace automatic ones and are counted separately
        Annotations ann;
        ann.setComment(0x0600, "my note");
        ann.setBlockComment(0x0601, "a section");
        const ListingResult m = renderListing(memory, dis, ann);
        CHECK_EQ(m.userComments, std::size_t{2});
        CHECK(m.commentedLines > m.userComments);  // plus the automatic comments
        // an unrelated HL load in between breaks the pairing
        Memory memory2;
        Assembler().assemble("        ORG &0700\n        LD A,&C3\n        LD (&0038),A\n        LD HL,&1111\n        INC HL\n        LD (&0039),HL\n        RET\n").applyTo(memory2);
        Disassembler dis2(memory2);
        dis2.analyzeFlow(0x0700);
        EXPECT_NOT_IN(renderListing(memory2, dis2, Annotations{}).text, "the interrupt handler is at");
    }

    // ---- nothing analysed ----
    {
        Memory memory;
        Disassembler dis(memory);
        const ListingResult r = renderListing(memory, dis, Annotations{});
        EXPECT_IN(r.text, "nothing analysed");
        CHECK(!r.verified);
    }

    return finish("listing");
}
