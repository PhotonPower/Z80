// Jump tables, data type guesses, references, constants and routine blocks (z80/analysis.hpp and the protocol commands built on it).

#include <random>
#include <set>

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
std::string h4(unsigned v) { return "0x" + hexDigits(v & 0xFFFF, 4); }

bool assemble(Session& s, const std::string& source) {
    const Json r = ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(source) + "}");
    if (!ok(r)) std::cerr << "asm failed: " << r.dump() << "\n" << source << "\n";
    return ok(r);
}

const Json* findBy(const Json& list, const char* key, const std::string& value) {
    for (const Json& e : list.items()) {
        if (str(e, key) == value) return &e;
    }
    return nullptr;
}

const char* kProgram =
    "        ORG &4000\n"
    "start:  LD A,1\n"
    "        CALL dispatch\n"
    "        LD A,1\n"
    "        CALL jrow\n"
    "        LD HL,(counter)\n"
    "        INC HL\n"
    "        LD (counter),HL\n"
    "        LD HL,msgs\n"
    "        CALL print_tab\n"
    "        LD HL,squares\n"
    "        CALL sum_words\n"
    "numline: LD BC,squares\n"      // stands for a number that happens to look like the address of squares
    "        JP finish\n"            // a tail call
    "dispatch:\n"
    "        ADD A,A\n"
    "        LD E,A\n"
    "        LD D,0\n"
    "        LD HL,table\n"
    "        ADD HL,DE\n"
    "        LD E,(HL)\n"
    "        INC HL\n"
    "        LD D,(HL)\n"
    "        EX DE,HL\n"
    "        JP (HL)\n"
    "table:  DW case0,case1,case2\n"
    "case0:  LD A,'0'\n"
    "        JP emit\n"
    "case1:  LD A,'1'\n"
    "        JP emit\n"
    "case2:  LD A,'2'\n"
    "emit:   CALL &BB5A\n"
    "        RET\n"
    "jrow:   LD L,A\n"
    "        ADD A,A\n"
    "        ADD A,L\n"
    "        LD L,A\n"
    "        LD H,0\n"
    "        LD DE,jtab\n"
    "        ADD HL,DE\n"
    "        JP (HL)\n"
    "jtab:   JP r0\n"
    "        JP r1\n"
    "r0:     XOR A\n"
    "        RET\n"
    "r1:     LD A,1\n"
    "        CALL finish\n"
    "        RET\n"
    "print_tab:\n"
    "        LD B,3\n"
    "pt1:    LD E,(HL)\n"
    "        INC HL\n"
    "        LD D,(HL)\n"
    "        INC HL\n"
    "        PUSH HL\n"
    "        EX DE,HL\n"
    "pt2:    LD A,(HL)\n"
    "        OR A\n"
    "        JR Z,pt3\n"
    "        CALL &BB5A\n"
    "        INC HL\n"
    "        JR pt2\n"
    "pt3:    POP HL\n"
    "        DJNZ pt1\n"
    "        RET\n"
    "sum_words:\n"
    "        LD B,4\n"
    "        LD IX,0\n"
    "sw1:    LD E,(HL)\n"
    "        INC HL\n"
    "        LD D,(HL)\n"
    "        INC HL\n"
    "        ADD IX,DE\n"
    "        DJNZ sw1\n"
    "        RET\n"
    "finish: LD A,(flag)\n"
    "        RET\n"
    "msgs:   DW m1,m2,m3\n"
    "m1:     DB \"Hello\",0\n"
    "m2:     DB \"No\",0\n"
    "m3:     DB \"Worl\",'d'+&80\n"
    "squares: DW 1,4,9,16,25\n"
    "counter: DW 0\n"
    "flag:   DB 0\n";

}  // namespace

int main() {
    // ---- jump tables ----
    {
        Session s;
        CHECK(ok(ask(s, R"({"cmd":"machine","type":"cpc"})")));
        CHECK(assemble(s, kProgram));
        const Json an = ask(s, R"({"cmd":"analyze","entries":["start"]})");
        CHECK(ok(an));
        CHECK_EQ(an.find("jump_tables")->asInt(), 2);
        const Json jt = ask(s, R"({"cmd":"jump_tables"})");
        CHECK(ok(jt));
        const Json& tables = *jt.find("tables");
        CHECK_EQ(tables.items().size(), std::size_t{2});
        const Json* ptr = findBy(tables, "kind", "pointers");
        const Json* row = findBy(tables, "kind", "jump_row");
        CHECK(ptr && row);
        if (ptr) {
            CHECK_EQ(ptr->find("entry_count")->asInt(), 3);
            CHECK(ptr->find("followed")->asBool());
            CHECK_EQ(ptr->find("bytes")->asInt(), 6);
        }
        if (row) CHECK_EQ(row->find("entry_count")->asInt(), 2);

        // the cases are routines of their own, reached through the table
        const Json r = ask(s, R"({"cmd":"routines","order":"address"})");
        CHECK(ok(r));
        int viaTable = 0;
        for (const Json& e : r.find("routines")->items()) {
            if (e.find("via_table")) ++viaTable;
        }
        CHECK_EQ(viaTable, 5);

        // the tail call: finish is a callee of start, not part of it; start is a caller of finish
        const Json* start = findBy(*r.find("routines"), "addr", "0x4000");
        CHECK(start != nullptr);
        if (start) {
            const std::string finishAddr = str(*ask(s, R"({"cmd":"symbols"})").find("symbols"), "FINISH");
            bool callsFinish = false;
            for (const Json& c : start->find("calls")->items()) callsFinish = callsFinish || str(c, "addr") == finishAddr;
            CHECK(callsFinish);
            const Json* fin = findBy(*r.find("routines"), "addr", finishAddr);
            CHECK(fin != nullptr);
            if (fin) CHECK(fin->find("caller_count")->asInt() >= 1);  // the tail call counts as a caller
            const Json& blocks = *start->find("blocks");
            CHECK_EQ(blocks.items().size(), std::size_t{1});
            CHECK_EQ(start->find("end")->asString(), "0x4023");  // ends with the JP finish, does not take finish in
        }

        // the listing names the cases and stays verified
        const Json l = ask(s, R"({"cmd":"listing"})");
        CHECK(ok(l));
        CHECK(l.find("verified")->asBool());
        const std::string text = str(l, "text");
        CHECK(has(text, "jump table (3 entries)"));
        CHECK(has(text, "DW sub_"));
        CHECK(has(text, "JP sub_"));
    }

    // ---- data regions, references, constants ----
    {
        Session s;
        CHECK(ok(ask(s, R"({"cmd":"machine","type":"cpc"})")));
        CHECK(assemble(s, kProgram));
        CHECK(ok(ask(s, R"({"cmd":"analyze","entries":["start"]})")));
        const Json d = ask(s, R"({"cmd":"data_regions"})");
        CHECK(ok(d));
        std::set<std::string> kinds;
        bool sawHello = false, sawWorld = false, sawSquares = false, sawCounter = false, sawPtrs = false;
        for (const Json& e : d.find("regions")->items()) {
            kinds.insert(str(e, "kind"));
            if (str(e, "preview") == "Hello") sawHello = true;
            if (str(e, "preview") == "World") sawWorld = has(str(e, "reason"), "bit 7") || has(str(e, "reason"), "string pointers");
            if (str(e, "kind") == "words" && e.find("length")->asInt() == 10) sawSquares = true;
            if (str(e, "kind") == "words" && e.find("length")->asInt() == 2) sawCounter = true;
            if (str(e, "kind") == "pointers" && e.find("length")->asInt() == 6) sawPtrs = true;
        }
        CHECK(sawHello);
        CHECK(sawWorld);
        CHECK(sawSquares);
        CHECK(sawCounter);
        CHECK(sawPtrs);
        // the byte variable `flag` gets no guess (bytes say nothing)
        const Json applied = ask(s, R"({"cmd":"data_regions","apply":true})");
        CHECK(ok(applied));
        CHECK(applied.find("applied")->asInt() >= 6);

        // references of the main routine: the pointer table, the number table and the variable, with contents
        const Json refs = ask(s, R"({"cmd":"references","from":"0x4000","to":"0x4022"})");
        CHECK(ok(refs));
        const Json* table = findBy(*refs.find("references"), "kind", "pointers");
        CHECK(table != nullptr);
        if (table) CHECK(str(*table, "preview").size() == 14);  // three addresses
        bool sawWords = false;
        for (const Json& e : refs.find("references")->items()) sawWords = sawWords || str(e, "kind") == "words";
        CHECK(sawWords);

        // LD BC,squares stands for a number that equals the address of squares: before marking it the listing calls it by the table's name
        const Json syms = *ask(s, R"({"cmd":"symbols"})").find("symbols");
        const std::string sq = str(syms, "SQUARES").substr(2), numline = str(syms, "NUMLINE");
        Json l = ask(s, R"({"cmd":"listing"})");
        CHECK(l.find("verified")->asBool());
        CHECK(has(str(l, "text"), "LD BC,data_" + sq));
        CHECK(!ok(ask(s, R"({"cmd":"ann_constant","addr":"0x4001"})")));  // the middle of an instruction
        CHECK(!ok(ask(s, R"({"cmd":"ann_constant","addr":"0x4000"})")));  // LD A,1 has no 16-bit operand
        CHECK(ok(ask(s, "{\"cmd\":\"ann_constant\",\"addr\":\"" + numline + "\"}")));
        l = ask(s, R"({"cmd":"listing"})");
        CHECK(l.find("verified")->asBool());
        CHECK(has(str(l, "text"), "LD BC,&" + sq));
        CHECK(has(str(l, "text"), "LD HL,data_" + sq));  // the real use keeps its label
        // constants survive save/load (JSON) and are undone by ann_remove
        const Json list = ask(s, R"({"cmd":"ann_list"})");
        CHECK_EQ(list.find("annotations")->find("constants")->items().size(), std::size_t{1});
        CHECK(ask(s, "{\"cmd\":\"ann_remove\",\"addr\":\"" + numline + "\"}").find("removed")->asBool());
        l = ask(s, R"({"cmd":"listing"})");
        CHECK(has(str(l, "text"), "LD BC,data_" + sq));

        // ann_remove takes back a wrong data region (not only its name)
        CHECK(ok(ask(s, "{\"cmd\":\"ann_variable\",\"addr\":\"0x" + sq + "\",\"name\":\"squares\",\"size\":10}")));
        CHECK(ask(s, "{\"cmd\":\"ann_remove\",\"addr\":\"0x" + sq + "\"}").find("removed")->asBool());
        const Json after = ask(s, R"({"cmd":"ann_list"})");
        bool still = false;
        for (const Json& e : after.find("annotations")->find("data")->items()) still = still || str(e, "address") == "0x" + sq;
        CHECK(!still);
    }

    // ---- name+offset: hexadecimal from 10 on, and it re-assembles ----
    {
        Session s;
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "        LD A,(&4102)\n"
                       "        LD (&4126),A\n"
                       "        RET\n"
                       "        ORG &4100\n"
                       "        DS 64,0\n"));
        CHECK(ok(ask(s, R"({"cmd":"analyze","entries":["0x4000"]})")));
        CHECK(ok(ask(s, R"({"cmd":"ann_variable","addr":"0x4100","name":"player","size":64})")));
        const Json l = ask(s, R"({"cmd":"listing","to":"0x4005"})");
        CHECK(l.find("verified")->asBool());
        CHECK(has(str(l, "text"), "LD A,(player+2)"));
        CHECK(has(str(l, "text"), "LD (player+&26),A"));
    }

    // ---- annotated pointer region: its targets are followed as code ----
    {
        Session s;
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "start:  LD HL,vectors\n"   // only stored somewhere: no dispatch the analysis could see
                       "        LD (&5000),HL\n"
                       "        RET\n"
                       "vectors: DW h1,h2,h3\n"
                       "h1:     LD A,1\n"
                       "        RET\n"
                       "h2:     LD A,2\n"
                       "        RET\n"
                       "h3:     LD A,3\n"
                       "        RET\n"));
        CHECK(ok(ask(s, R"({"cmd":"analyze","entries":["start"]})")));
        Json jt = ask(s, R"({"cmd":"jump_tables"})");
        CHECK_EQ(jt.find("tables")->items().size(), std::size_t{1});
        if (!jt.find("tables")->items().empty()) {
            const Json& t = jt.find("tables")->items()[0];
            CHECK(!t.find("followed")->asBool());  // only a suggestion
            CHECK(t.find("hint") != nullptr);
        }
        CHECK(ok(ask(s, R"({"cmd":"ann_data","addr":"vectors","length":6,"kind":"pointers"})")));
        const Json an = ask(s, R"({"cmd":"analyze"})");
        CHECK(ok(an));
        jt = ask(s, R"({"cmd":"jump_tables"})");
        bool followed = false;
        for (const Json& t : jt.find("tables")->items()) followed = followed || t.find("followed")->asBool();
        CHECK(followed);
        const Json l = ask(s, R"({"cmd":"listing"})");
        CHECK(l.find("verified")->asBool());
        CHECK(has(str(l, "text"), "DW sub_400D,sub_4010,sub_4013"));
    }

    // ---- PUSH HL / RET dispatch ----
    {
        Session s;
        CHECK(assemble(s,
                       "        ORG &4000\n"
                       "go:     ADD A,A\n"
                       "        LD L,A\n"
                       "        LD H,0\n"
                       "        LD DE,tab\n"
                       "        ADD HL,DE\n"
                       "        LD A,(HL)\n"
                       "        INC HL\n"
                       "        LD H,(HL)\n"
                       "        LD L,A\n"
                       "        PUSH HL\n"
                       "        RET\n"
                       "tab:    DW a1,a2\n"
                       "a1:     LD B,1\n"
                       "        RET\n"
                       "a2:     LD B,2\n"
                       "        RET\n"));
        const Json an = ask(s, R"({"cmd":"analyze","entries":["go"]})");
        CHECK_EQ(an.find("jump_tables")->asInt(), 1);
        const Json l = ask(s, R"({"cmd":"listing"})");
        CHECK(l.find("verified")->asBool());
        CHECK(has(str(l, "text"), "DW sub_4012,sub_4015"));
    }

    // ---- random dispatch programs: every case is found, the listing re-assembles ----
    {
        std::mt19937 rng(1234);
        int found = 0, total = 0;
        for (int round = 0; round < 200; ++round) {
            const int cases = 2 + static_cast<int>(rng() % 10);
            const bool row = rng() % 2 == 0;
            std::string src = "        ORG &" + hexDigits(0x1000 + (rng() % 0x80) * 0x100, 4) + "\n";
            src += "main:   LD A," + std::to_string(rng() % cases) + "\n        CALL disp\n        RET\n";
            if (row) {
                src += "disp:   LD L,A\n        ADD A,A\n        ADD A,L\n        LD L,A\n        LD H,0\n        LD DE,tab\n        ADD HL,DE\n        JP (HL)\n";
                src += "tab:\n";
                for (int c = 0; c < cases; ++c) src += "        JP c" + std::to_string(c) + "\n";
            } else {
                src += "disp:   ADD A,A\n        LD E,A\n        LD D,0\n        LD HL,tab\n        ADD HL,DE\n        LD E,(HL)\n        INC HL\n        LD D,(HL)\n        EX DE,HL\n        JP (HL)\n";
                src += "tab:    DW ";
                for (int c = 0; c < cases; ++c) src += (c ? ",c" : "c") + std::to_string(c);
                src += "\n";
            }
            for (int c = 0; c < cases; ++c) {
                src += "c" + std::to_string(c) + ":";
                const int n = 1 + static_cast<int>(rng() % 4);
                for (int k = 0; k < n; ++k) src += "  LD B," + std::to_string(rng() % 256) + "\n";
                src += "        RET\n";
            }
            Session s;
            if (!assemble(s, src)) continue;
            CHECK(ok(ask(s, R"({"cmd":"analyze","entries":["main"]})")));
            const Json jt = ask(s, R"({"cmd":"jump_tables"})");
            total += cases;
            for (const Json& t : jt.find("tables")->items()) {
                if (t.find("followed")->asBool()) found += static_cast<int>(t.find("entry_count")->asInt());
            }
            const Json l = ask(s, R"({"cmd":"listing"})");
            CHECK(l.find("verified")->asBool());
        }
        CHECK_EQ(found, total);
    }

    // ---- random memory images: guesses never break the listing ----
    {
        std::mt19937 rng(99);
        for (int round = 0; round < 150; ++round) {
            Session s;
            std::string hex;
            const int size = 64 + static_cast<int>(rng() % 400);
            for (int i = 0; i < size; ++i) {
                unsigned b = rng() % 256;
                if (rng() % 3 == 0) b = 'A' + rng() % 26;  // plenty of text-like bytes
                if (rng() % 7 == 0) b = 0;
                hex += hexDigits(b, 2);
            }
            CHECK(ok(ask(s, "{\"cmd\":\"write\",\"addr\":\"0x4000\",\"hex\":\"" + hex + "\"}")));
            CHECK(ok(ask(s, "{\"cmd\":\"analyze\",\"entries\":[\"0x4000\"],\"from\":\"0x4000\",\"to\":\"" + h4(0x4000 + size - 1) + "\"}")));
            CHECK(ok(ask(s, R"({"cmd":"jump_tables"})")));
            CHECK(ok(ask(s, R"({"cmd":"data_regions","apply":true})")));
            CHECK(ok(ask(s, R"({"cmd":"routines"})")));
            CHECK(ok(ask(s, R"({"cmd":"references","from":"0x4000","to":"0x4100"})")));
            const Json l = ask(s, "{\"cmd\":\"listing\",\"from\":\"0x4000\",\"to\":\"" + h4(0x4000 + size - 1) + "\"}");
            CHECK(ok(l));
            if (!l.find("verified")->asBool()) std::cerr << str(l, "verify_detail") << "\n";
            CHECK(l.find("verified")->asBool());
        }
    }

    return finish("analysis");
}
