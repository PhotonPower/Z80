#include <filesystem>
#include <fstream>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/expression.hpp"
#include "z80/monitor.hpp"

using namespace z80;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void expectIn(const std::string& output, const std::string& part, int line) {
    ++g_checks;
    if (!has(output, part)) {
        ++g_failures;
        std::cerr << "test_monitor.cpp:" << line << ": expected \"" << part << "\" in output:\n" << output << "\n";
    }
}
#define EXPECT_IN(out, part) expectIn((out), (part), __LINE__)

/// Types a program with the monitor's own assemble mode.
void typeProgram(Monitor& m, const std::string& at, std::initializer_list<const char*> lines) {
    m.execute("a " + at);
    for (const char* l : lines) m.execute(l);
    m.execute("");
}

}  // namespace

int main() {
    // ---- expression evaluator: hexadecimal default ----
    {
        const auto hex = [](const char* text, std::function<std::optional<std::int64_t>(const std::string&)> lookup = {}) {
            return ExpressionEvaluator(std::move(lookup), 0, true).evaluate(text);
        };
        CHECK_EQ(hex("4000").value, 0x4000);
        CHECK_EQ(hex("FF").value, 0xFF);          // starts with a letter, but is only hex digits
        CHECK_EQ(hex("BEEF+1").value, 0xBEF0);
        CHECK_EQ(hex("&10").value, 0x10);
        CHECK_EQ(hex("0x10").value, 0x10);
        CHECK_EQ(hex("10h").value, 0x10);
        CHECK_EQ(hex("%101").value, 5);            // binary needs the prefix in hex mode
        CHECK_EQ(hex("1010b").value, 0x1010B);     // ... because a suffix 'b' is just a hex digit here
        CHECK(!hex("XYZ").undefinedNames.empty());  // not hex digits: still an undefined symbol
        // a known symbol wins over the hex reading
        const auto r = hex("BEEF", [](const std::string& n) -> std::optional<std::int64_t> {
            if (n == "BEEF") return 7;
            return std::nullopt;
        });
        CHECK_EQ(r.value, 7);
        // decimal mode is unchanged
        CHECK_EQ(ExpressionEvaluator{}.evaluate("4000").value, 4000);
    }

    // ---- assembler: predefined symbols ----
    {
        AssemblerOptions options;
        options.origin = 0x1000;
        options.predefined["target"] = 0x2345;
        const auto r = Assembler(options).assemble("CALL target\nJP TARGET+1\n");
        CHECK(r.ok());
        const auto bytes = r.flatten();
        CHECK(bytes == std::vector<Byte>({0xCD, 0x45, 0x23, 0xC3, 0x46, 0x23}));
        CHECK(!Assembler().assemble("CALL target\n").ok());  // not defined without the option
    }

    // ---- basics ----
    {
        Monitor m;
        CHECK(has(Monitor::banner(), "Z80 monitor"));
        CHECK_EQ(m.prompt(), std::string("- "));
        CHECK_EQ(m.execute(""), std::string(""));
        CHECK_EQ(m.execute("   "), std::string(""));
        EXPECT_IN(m.execute("frobnicate"), "unknown command");
        EXPECT_IN(m.execute("d nosuchsymbol"), "unknown symbol");
        EXPECT_IN(m.execute("d 10000"), "out of range");
        EXPECT_IN(m.execute("e"), "missing");
        EXPECT_IN(m.execute("?"), "disassemble");
        CHECK(!m.finished());
        m.execute("q");
        CHECK(m.finished());
    }

    // ---- memory commands (numbers are hex) ----
    {
        Monitor m;
        EXPECT_IN(m.execute("e 100 3E 05 C9"), "3 byte(s) written at 0100");
        CHECK_EQ(m.memory().read(0x100), Byte{0x3E});
        EXPECT_IN(m.execute("d 100 3"), "LD A,&05");
        EXPECT_IN(m.execute("d"), "0104");  // continues after the last line (3 instructions: 0100, 0102, 0103)

        m.execute("e 200 10.");           // decimal
        m.execute("e 201 1+2");           // expression
        m.execute("e 202 \"Hi\"");        // text
        CHECK_EQ(m.memory().read(0x200), Byte{10});
        CHECK_EQ(m.memory().read(0x201), Byte{3});
        CHECK_EQ(m.memory().read(0x202), Byte{'H'});
        EXPECT_IN(m.execute("e 300 100"), "byte out of range");

        const std::string dump = m.execute("m 200 4");
        EXPECT_IN(dump, "0200  0A 03 48 69");
        EXPECT_IN(dump, "|..Hi|");
        EXPECT_IN(m.execute("m 0 20"), "0010");  // 0x20 bytes = two rows
        EXPECT_IN(m.execute("m"), "0020");       // continues

        m.execute("f 400 40F AA 55");
        CHECK_EQ(m.memory().read(0x400), Byte{0xAA});
        CHECK_EQ(m.memory().read(0x401), Byte{0x55});
        CHECK_EQ(m.memory().read(0x40F), Byte{0x55});
        EXPECT_IN(m.execute("c 400 40F 410"), "16 difference(s)");
        m.execute("t 400 40F 410");
        EXPECT_IN(m.execute("c 400 40F 410"), "equal");

        // overlapping copy must behave like memmove
        m.execute("e 500 1 2 3 4");
        m.execute("t 500 503 501");
        CHECK_EQ(m.memory().read(0x501), Byte{1});
        CHECK_EQ(m.memory().read(0x504), Byte{4});

        EXPECT_IN(m.execute("h 0 FFF 48 69"), "0202");
        EXPECT_IN(m.execute("h 0 FFF \"Hi\""), "0202");
        EXPECT_IN(m.execute("h 0 FFF DE AD"), "not found");
    }

    // ---- assemble mode ----
    {
        Monitor m;
        EXPECT_IN(m.execute("a 4000"), "assemble mode");
        CHECK_EQ(m.prompt(), std::string("4000: "));
        EXPECT_IN(m.execute("start: LD B,5"), "4000  06 05");
        EXPECT_IN(m.execute("LD B,5"), "LD B,&05");
        CHECK_EQ(m.prompt(), std::string("4004: "));
        EXPECT_IN(m.execute("BOGUS"), "unknown mnemonic");  // not silently taken as a label
        EXPECT_IN(m.execute("  FOO A,B"), "unknown mnemonic");  // error: address does not advance
        EXPECT_IN(m.execute("JP nowhere"), "define it first");
        CHECK_EQ(m.prompt(), std::string("4004: "));
        EXPECT_IN(m.execute("loop: DJNZ loop"), "DJNZ LOOP");
        EXPECT_IN(m.execute("DB 1,2,3,4,5,6"), "6 byte(s) of data");
        CHECK_EQ(m.execute(""), std::string(""));
        CHECK_EQ(m.prompt(), std::string("- "));
        CHECK_EQ(m.memory().read(0x4000), Byte{0x06});
        CHECK_EQ(m.memory().read(0x4004), Byte{0x10});
        CHECK_EQ(m.memory().read(0x4005), Byte{0xFE});  // JR to itself
        EXPECT_IN(m.execute("sym"), "LOOP");
        EXPECT_IN(m.execute("sym"), "START");
        EXPECT_IN(m.execute("d start 3"), "START:");
    }

    // ---- running, stepping, breakpoints ----
    {
        Monitor m;
        m.execute("sym twice 400D");
        typeProgram(m, "4000",
                    {"LD B,5", "XOR A", "loop: ADD A,B", "DJNZ loop", "LD (&4010),A", "CALL twice", "HALT",
                     "ADD A,A", "RET"});
        EXPECT_IN(m.execute("d 4009 1"), "CALL TWICE");

        m.execute("b twice");
        EXPECT_IN(m.execute("b"), "break 400D");
        const std::string hit = m.execute("g 4000");
        EXPECT_IN(hit, "stopped: breakpoint at 400D");
        EXPECT_IN(hit, "PC=400D");
        EXPECT_IN(hit, "AF=0F");  // A = 15
        CHECK_EQ(m.cpu().regs().sp, Word{0xFFFD});

        EXPECT_IN(m.execute("u"), "stopped: returned");
        CHECK_EQ(m.cpu().regs().pc, Word{0x400C});
        CHECK_EQ(m.cpu().regs().a(), Byte{0x1E});

        const std::string step = m.execute("n");
        EXPECT_IN(step, "HALT");
        CHECK(m.cpu().regs().halted);
        EXPECT_IN(m.execute("g"), "stopped: halted");

        // step over
        m.execute("bc");
        EXPECT_IN(m.execute("b"), "no breakpoints");
        m.execute("r pc=4009 sp=FFFF");
        EXPECT_IN(m.execute("o"), "stopped: stepped");
        CHECK_EQ(m.cpu().regs().pc, Word{0x400C});

        // several steps list every executed instruction
        m.execute("r pc=4000");
        const std::string steps = m.execute("n 3");
        EXPECT_IN(steps, "LD B,&05");
        EXPECT_IN(steps, "XOR A");
        EXPECT_IN(steps, "ADD A,B");

        // run until an address
        EXPECT_IN(m.execute("ru 4006"), "stopped: address at 4006");
    }
    {
        Monitor m;
        typeProgram(m, "100", {"loop: JR loop"});
        EXPECT_IN(m.execute("limit 64"), "limit 100 instructions");  // 0x64 = 100
        EXPECT_IN(m.execute("g 100"), "stopped: instruction_limit");
        EXPECT_IN(m.execute("limit 0"), "between");
    }
    {
        Monitor m;
        typeProgram(m, "100", {"LD A,1", "LD (&800),A", "HALT"});
        m.execute("w 800 w");
        EXPECT_IN(m.execute("b"), "watch 0800 w");
        EXPECT_IN(m.execute("g 100"), "stopped: watchpoint at 0800 (write)");
        EXPECT_IN(m.execute("w 800 q"), "kind");
    }

    // ---- registers ----
    {
        Monitor m;
        const std::string regs = m.execute("r hl=1234 a=7 ix=@sp");
        EXPECT_IN(regs, "HL=1234");
        EXPECT_IN(regs, "IX=FFFF");
        CHECK_EQ(m.cpu().regs().a(), Byte{7});
        EXPECT_IN(m.execute("r a=100"), "cannot set");
        EXPECT_IN(m.execute("r zz=1"), "cannot set");
        EXPECT_IN(m.execute("r a"), "usage");
        EXPECT_IN(m.execute("d @hl 1"), "1234");
        m.execute("reset");
        CHECK_EQ(m.cpu().regs().pc, Word{0});
        m.execute("e 10 AA");
        m.execute("reset mem");
        CHECK_EQ(m.memory().read(0x10), Byte{0});
    }

    // ---- checkpoints, interrupts, I/O ----
    {
        Monitor m;
        m.execute("e 100 11");
        m.execute("ck a");
        m.execute("e 100 22");
        EXPECT_IN(m.execute("rb a"), "PC=");
        CHECK_EQ(m.memory().read(0x100), Byte{0x11});
        EXPECT_IN(m.execute("rb nope"), "unknown checkpoint");
    }
    {
        Monitor m;
        typeProgram(m, "100", {"LD A,&41", "OUT (&10),A", "IN A,(&20)", "HALT"});
        m.execute("input 77");
        m.execute("g 100");
        EXPECT_IN(m.execute("io"), "OUT 4110 <- 41");
        EXPECT_IN(m.execute("io"), "no output");
        CHECK_EQ(m.cpu().regs().a(), Byte{0x77});
    }
    {
        Monitor m;
        typeProgram(m, "100", {"IM 1", "EI", "NOP", "NOP"});
        typeProgram(m, "38", {"INC B", "RETI"});
        m.execute("r pc=100 sp=9000");
        m.execute("n 3");
        EXPECT_IN(m.execute("int"), "interrupt requested");
        m.execute("n");
        CHECK_EQ(m.cpu().regs().pc, Word{0x38});
        EXPECT_IN(m.execute("nmi"), "NMI requested");
        m.execute("n");
        CHECK_EQ(m.cpu().regs().pc, Word{0x66});
    }

    // ---- symbols ----
    {
        Monitor m;
        m.execute("sym port 10");
        m.execute("sym DEAD 20");
        EXPECT_IN(m.execute("sym"), "PORT");
        m.execute("e DEAD AA");  // a symbol named like a hex number wins
        CHECK_EQ(m.memory().read(0x20), Byte{0xAA});
        CHECK_EQ(m.memory().read(0xDEAD), Byte{0});
        m.execute("sym -dead");
        m.execute("e DEAD BB");  // now it is just a number
        CHECK_EQ(m.memory().read(0xDEAD), Byte{0xBB});
        EXPECT_IN(m.execute("sym x"), "usage");
    }

    // ---- files ----
    {
        const auto dir = std::filesystem::temp_directory_path();
        const auto src = dir / "z80tools_monitor_test.asm";
        const auto bin = dir / "z80tools_monitor_test.bin";
        const auto sym = dir / "z80tools_monitor_test.sym";
        const auto snap = dir / "z80tools_monitor_test.snap";
        { std::ofstream(src) << "ORG &6000\nmain: LD A,&42\nRET\ndata DB 9\n"; }

        Monitor m;
        const std::string a = m.execute("asm " + src.string());
        EXPECT_IN(a, "4 bytes at 6000-6003");
        CHECK_EQ(m.memory().read(0x6001), Byte{0x42});
        EXPECT_IN(m.execute("d main 1"), "LD A,&42");

        EXPECT_IN(m.execute("sv " + bin.string() + " 6000 6003"), "4 bytes saved");
        CHECK_EQ(std::filesystem::file_size(bin), std::uintmax_t{4});
        m.execute("symsave " + sym.string());
        m.execute("savestate " + snap.string());

        Monitor n;
        EXPECT_IN(n.execute("l " + bin.string() + " 7000"), "4 bytes loaded at 7000");
        CHECK_EQ(n.memory().read(0x7001), Byte{0x42});
        EXPECT_IN(n.execute("symload " + sym.string()), "symbol(s) loaded");
        EXPECT_IN(n.execute("sym"), "MAIN");
        EXPECT_IN(n.execute("loadstate " + snap.string()), "PC=");
        CHECK_EQ(n.memory().read(0x6001), Byte{0x42});

        EXPECT_IN(n.execute("l /nonexistent/file.bin"), "cannot open");
        { std::ofstream(src) << "LD A,\n"; }
        EXPECT_IN(n.execute("asm " + src.string()), "nothing loaded");

        MonitorOptions locked;
        locked.allowFiles = false;
        Monitor k(locked);
        EXPECT_IN(k.execute("l " + bin.string()), "disabled");
        EXPECT_IN(k.execute("asm " + src.string()), "disabled");
        EXPECT_IN(k.execute("savestate " + snap.string()), "disabled");

        for (const auto& p : {src, bin, sym, snap}) std::filesystem::remove(p);
    }

    // ---- the monitor survives arbitrary input ----
    {
        Monitor m;
        const char* inputs[] = {"d ((", "m ,,,", "e 0 \"unterminated", "h 0 0", "t 5 1 0", "f 0 FFFF", "g zz", "b 99999",
                                "w", "sym - ", "asm", "l", "sv x 0", "ck", "int zz", "limit x", "r =", "r pc=", "n -1",
                                "d 0 0", "m 0 0"};
        for (const char* in : inputs) {
            const std::string out = m.execute(in);
            ++g_checks;  // must return normally (no exception, no crash)
            (void)out;
        }
        CHECK(!m.finished());
    }

    return finish("monitor");
}
