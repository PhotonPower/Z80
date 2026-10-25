#include <filesystem>

#include "test_util.hpp"
#include "z80/json.hpp"
#include "z80/session.hpp"

using namespace z80;

namespace {

Json ask(Session& s, const std::string& request) { return Json::parse(s.handleLine(request)); }

bool ok(const Json& j) {
    const Json* v = j.find("ok");
    return v && v->isBool() && v->asBool();
}
std::string str(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : "<missing>";
}

const char* kProgram =
    "{\"cmd\":\"asm\",\"entry\":\"start\",\"source\":\""
    "        ORG &4000\\n"
    "start:  LD B,5\\n"
    "        XOR A\\n"
    "loop:   ADD A,B\\n"
    "        DJNZ loop\\n"
    "        LD (result),A\\n"
    "        CALL double\\n"
    "        HALT\\n"
    "double: ADD A,A\\n"
    "        RET\\n"
    "result: DB 0\\n\"}";

}  // namespace

int main() {
    // ---- JSON ----
    {
        const Json j = Json::parse(R"( {"a":1,"b":[true,null,"x\n\u0041"],"c":{"d":-2.5},"e":18446744073} )");
        CHECK(j.isObject());
        CHECK_EQ(j.find("a")->asInt(), std::int64_t{1});
        CHECK_EQ(j.find("b")->items().size(), std::size_t{3});
        CHECK_EQ(j.find("b")->items()[2].asString(), std::string("x\nA"));
        CHECK(j.find("c")->find("d")->asDouble() == -2.5);
        CHECK_EQ(j.find("e")->asInt(), std::int64_t{18446744073});
        CHECK(j.find("missing") == nullptr);
        CHECK_EQ(Json::parse(j.dump()).dump(), j.dump());

        Json built = Json::object();
        built.set("x", 1).set("y", "a\"b").set("z", Json::array());
        built.set("x", 2);  // replace keeps position
        CHECK_EQ(built.dump(), std::string(R"({"x":2,"y":"a\"b","z":[]})"));

        for (const char* bad : {"", "{", "[1,", R"({"a"})", R"({"a":})", "tru", "{\"a\":1} x", "\"abc", "[1 2]", "-", R"("\q")"}) {
            bool threw = false;
            try { Json::parse(bad); } catch (const JsonError&) { threw = true; }
            ++g_checks;
            if (!threw) { ++g_failures; std::cerr << "JSON should be rejected: " << bad << "\n"; }
        }
        std::string deep(200, '[');
        bool threw = false;
        try { Json::parse(deep); } catch (const JsonError&) { threw = true; }
        CHECK(threw);  // nesting limit instead of stack overflow
    }

    // ---- assemble, run, inspect ----
    {
        Session s;
        const Json r = ask(s, kProgram);
        CHECK(ok(r));
        CHECK_EQ(str(r, "start"), std::string("0x4000"));
        CHECK_EQ(str(r, "pc"), std::string("0x4000"));
        CHECK_EQ(str(*r.find("symbols"), "RESULT"), std::string("0x400F"));
        CHECK_EQ(str(*r.find("symbols"), "DOUBLE"), std::string("0x400D"));

        const Json run = ask(s, R"({"cmd":"run"})");
        CHECK(ok(run));
        CHECK_EQ(str(run, "stop"), std::string("halted"));
        CHECK_EQ(str(*run.find("regs"), "A"), std::string("0x1E"));  // (5+4+3+2+1)*2

        const Json mem = ask(s, R"({"cmd":"read","addr":"result","len":1})");  // symbol as address
        CHECK_EQ(str(mem, "hex"), std::string("0F"));

        const Json dis = ask(s, R"({"cmd":"dis","addr":"start","count":3})");
        CHECK_EQ(dis.find("lines")->items().size(), std::size_t{3});
        CHECK_EQ(str(dis.find("lines")->items()[0], "label"), std::string("START"));
        CHECK_EQ(str(dis.find("lines")->items()[0], "text"), std::string("LD B,0x05"));
        CHECK_EQ(str(dis.find("lines")->items()[2], "text"), std::string("ADD A,B"));
    }

    // ---- breakpoints, step over, call, checkpoints ----
    {
        Session s;
        ask(s, kProgram);
        CHECK(ok(ask(s, R"({"cmd":"break","addr":"double"})")));
        const Json hit = ask(s, R"({"cmd":"run"})");
        CHECK_EQ(str(hit, "stop"), std::string("breakpoint"));
        CHECK_EQ(str(hit, "pc"), std::string("0x400D"));  // DOUBLE

        const Json brk = ask(s, R"({"cmd":"breaks"})");
        CHECK_EQ(brk.find("breakpoints")->items().size(), std::size_t{1});
        CHECK(ok(ask(s, R"({"cmd":"clear_breaks"})")));
        CHECK_EQ(ask(s, R"({"cmd":"breaks"})").find("breakpoints")->items().size(), std::size_t{0});

        // checkpoint, modify, diff, rollback
        CHECK(ok(ask(s, R"({"cmd":"checkpoint","name":"c1"})")));
        ask(s, R"({"cmd":"write","addr":"0x8000","hex":"DE AD BE EF"})");
        const Json diff = ask(s, R"({"cmd":"diff","name":"c1"})");
        CHECK_EQ(diff.find("changed_bytes")->asInt(), std::int64_t{4});
        CHECK_EQ(str(diff.find("ranges")->items()[0], "after"), std::string("DEADBEEF"));
        CHECK(ok(ask(s, R"({"cmd":"rollback","name":"c1"})")));
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":"0x8000","len":4})"), "hex"), std::string("00000000"));
        CHECK(!ok(ask(s, R"({"cmd":"rollback","name":"nope"})")));
    }
    {
        Session s;
        ask(s, R"({"cmd":"asm","source":"ORG &5000\nfn: ADD A,B\nINC C\nRET\n"})");
        // call a routine with arguments and get the result back
        const Json r = ask(s, R"({"cmd":"call","addr":"fn","regs":{"A":40,"B":2,"C":9}})");
        CHECK(ok(r));
        CHECK(r.find("returned")->asBool());
        CHECK_EQ(str(*r.find("regs"), "A"), std::string("0x2A"));
        CHECK_EQ(str(r, "stop"), std::string("address"));
        // the stack is balanced afterwards
        CHECK_EQ(str(*r.find("regs"), "SP"), std::string("0xFFFF"));
    }
    {
        Session s;
        ask(s, R"({"cmd":"asm","source":"ORG &100\nCALL f\nNOP\nHALT\nf: LD A,7\nRET\n"})");
        ask(s, R"({"cmd":"set","regs":{"PC":"0x100"}})");
        const Json over = ask(s, R"({"cmd":"step_over"})");
        CHECK_EQ(str(over, "stop"), std::string("stepped"));
        CHECK_EQ(str(over, "pc"), std::string("0x0103"));
        const Json step = ask(s, R"({"cmd":"step","n":2,"trace":true})");
        CHECK_EQ(step.find("trace")->items().size(), std::size_t{2});
        CHECK_EQ(str(step.find("trace")->items()[0], "text"), std::string("NOP"));
    }

    // ---- state, interrupts, I/O ----
    {
        Session s;
        ask(s, R"({"cmd":"asm","source":"ORG &100\nLD A,&41\nOUT (&10),A\nIN A,(&20)\nHALT\n"})");
        ask(s, R"({"cmd":"set","regs":{"pc":256}})");
        ask(s, R"({"cmd":"input","hex":"77"})");
        ask(s, R"({"cmd":"run"})");
        const Json io = ask(s, R"({"cmd":"io"})");
        CHECK_EQ(io.find("output")->items().size(), std::size_t{1});
        CHECK_EQ(str(io.find("output")->items()[0], "port"), std::string("0x4110"));
        CHECK_EQ(str(io.find("output")->items()[0], "value"), std::string("0x41"));
        CHECK_EQ(str(*ask(s, R"({"cmd":"regs"})").find("regs"), "A"), std::string("0x77"));
        CHECK_EQ(ask(s, R"({"cmd":"io"})").find("output")->items().size(), std::size_t{0});  // log was cleared

        const Json st = ask(s, R"({"cmd":"state"})");
        CHECK(ok(st));
        CHECK(ok(ask(s, R"({"cmd":"reset"})")));
        Json setReq = Json::object();
        setReq.set("cmd", "set_state").set("state", *st.find("state"));
        CHECK(ok(s.handle(setReq)));
        CHECK_EQ(str(*ask(s, R"({"cmd":"regs"})").find("regs"), "A"), std::string("0x77"));
    }
    {
        Session s;
        ask(s, R"({"cmd":"asm","source":"ORG &100\nIM 1\nEI\nNOP\nNOP\nORG &38\nINC B\nRETI\n"})");
        ask(s, R"({"cmd":"set","regs":{"PC":256,"SP":"0x9000"}})");
        ask(s, R"({"cmd":"step","n":3})");
        ask(s, R"({"cmd":"interrupt"})");
        ask(s, R"({"cmd":"step"})");  // interrupt acknowledge
        CHECK_EQ(str(*ask(s, R"({"cmd":"regs"})").find("regs"), "PC"), std::string("0x0038"));
    }

    // ---- error handling ----
    {
        Session s;
        auto fails = [&](const std::string& request, const std::string& part) {
            const Json r = ask(s, request);
            const bool good = !ok(r) && str(r, "error").find(part) != std::string::npos;
            ++g_checks;
            if (!good) { ++g_failures; std::cerr << "expected error '" << part << "' for " << request << "\n  got " << r.dump() << "\n"; }
        };
        fails("not json", "JSON");
        fails("[1,2]", "object");
        fails("{}", "cmd");
        fails(R"({"cmd":"frobnicate"})", "unknown command");
        fails(R"({"cmd":"read"})", "addr");
        fails(R"({"cmd":"read","addr":70000})", "out of range");
        fails(R"({"cmd":"read","addr":"nosuchlabel"})", "unknown symbol");
        fails(R"({"cmd":"write","addr":0,"hex":"ABC"})", "odd");
        fails(R"({"cmd":"write","addr":0,"hex":"ZZ"})", "invalid hex");
        fails(R"({"cmd":"set","regs":{"A":300}})", "cannot set");
        fails(R"({"cmd":"set","regs":{"Q":1}})", "cannot set");
        fails(R"({"cmd":"step","n":0})", "between");
        fails(R"({"cmd":"step","n":5000,"trace":true})", "limited");
        fails(R"({"cmd":"watch","addr":0,"kind":"sideways"})", "kind");
        fails(R"({"cmd":"asm","source":"LD A,\n  FOO\n"})", "assembly failed");
        // file access is off unless the operator enables it
        fails(R"({"cmd":"load_file","file":"/etc/passwd"})", "disabled");
        fails(R"({"cmd":"save_state","file":"/tmp/x"})", "disabled");
        fails(R"({"cmd":"load_state","file":"/tmp/x"})", "disabled");

        const Json asmErr = ask(s, R"({"cmd":"asm","source":"LD A,\n  FOO\n"})");
        const Json* errors = asmErr.find("errors");
        CHECK(errors != nullptr);
        if (errors && errors->items().size() == 2) CHECK_EQ(errors->items()[1].find("line")->asInt(), std::int64_t{2});
        else CHECK(false);
        // a failed assembly must not change memory
        CHECK_EQ(str(ask(s, R"({"cmd":"read","addr":0,"len":2})"), "hex"), std::string("0000"));
    }

    // ---- files (opt-in) ----
    {
        const auto path = std::filesystem::temp_directory_path() / "z80tools_session_test.snap";
        SessionOptions options;
        options.allowFiles = true;
        Session a(options);
        ask(a, R"({"cmd":"asm","source":"ORG &100\nLD A,&66\nHALT\n","entry":256})");
        ask(a, R"({"cmd":"run"})");
        Json save = Json::object();
        save.set("cmd", "save_state").set("file", path.string());
        CHECK(ok(a.handle(save)));

        Session b(options);
        Json load = Json::object();
        load.set("cmd", "load_state").set("file", path.string());
        CHECK(ok(b.handle(load)));
        CHECK(b.cpu().state() == a.cpu().state());
        CHECK_EQ(str(*ask(b, R"({"cmd":"regs"})").find("regs"), "A"), std::string("0x66"));
        CHECK_EQ(str(ask(b, R"({"cmd":"read","addr":256,"len":3})"), "hex"), std::string("3E6676"));

        // a truncated snapshot file is an error, not a crash
        std::filesystem::resize_file(path, 100);
        CHECK(!ok(b.handle(load)));
        std::filesystem::remove(path);
    }

    // ---- help lists every command that exists ----
    {
        Session s;
        const Json help = ask(s, R"({"cmd":"help"})");
        const Json* cmds = help.find("commands");
        CHECK(cmds != nullptr);
        CHECK(cmds->find("run") != nullptr && cmds->find("call") != nullptr && cmds->find("diff") != nullptr);
    }

    return finish("session");
}
