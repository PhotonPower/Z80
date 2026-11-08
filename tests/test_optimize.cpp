// Assembler optimisation (-O): JP -> JR where the target is in reach. The effect must be the same: random programs assembled with and
// without -O end in the same registers and the same data memory.

#include <random>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/cpu.hpp"
#include "z80/decoder.hpp"

using namespace z80;

namespace {

AssemblyResult assemble(const std::string& source, bool optimize) {
    AssemblerOptions options;
    options.optimize = optimize;
    return Assembler(options).assemble(source);
}

std::string firstText(const AssemblyResult& r) {
    Memory m;
    r.applyTo(m);
    return decode(m, r.lowAddress()).text();
}

}  // namespace

int main() {
    // ---- the forms ----
    {
        const auto plain = assemble("        ORG &4000\n        JP next\nnext:   RET\n", false);
        const auto opt = assemble("        ORG &4000\n        JP next\nnext:   RET\n", true);
        CHECK(plain.ok() && opt.ok());
        CHECK_EQ(plain.size(), std::size_t{4});
        CHECK_EQ(opt.size(), std::size_t{3});
        CHECK_EQ(firstText(opt), std::string("JR &4002"));
        for (const char* cc : {"NZ", "Z", "NC", "C"}) {
            const auto r = assemble(std::string("        ORG &4000\nhere:   JP ") + cc + ",here\n", true);
            CHECK(r.ok());
            CHECK_EQ(r.size(), std::size_t{2});
        }
        for (const char* cc : {"PO", "PE", "P", "M"}) {  // JR has no such conditions
            const auto r = assemble(std::string("        ORG &4000\nhere:   JP ") + cc + ",here\n", true);
            CHECK_EQ(r.size(), std::size_t{3});
        }
        CHECK_EQ(assemble("        JP (HL)\n", true).size(), std::size_t{1});
        CHECK_EQ(assemble("        JP (IX)\n", true).size(), std::size_t{2});
        // out of reach stays JP; an absolute number in reach becomes JR as well
        const auto far = assemble("        ORG &4000\n        JP far\n        DS 200\nfar:    RET\n", true);
        CHECK_EQ(far.size(), std::size_t{204});
        CHECK_EQ(assemble("        ORG &4000\n        JP &4010\n", true).size(), std::size_t{2});
        // a forward target that only comes into reach because other jumps got shorter
        std::string chain = "        ORG &4000\n        JP end\n";
        for (int i = 0; i < 41; ++i) chain += "        JP end\n";  // 42 x 3 = 126 bytes: as JR 84 bytes
        chain += "end:    RET\n";
        const auto c = assemble(chain, true);
        CHECK(c.ok());
        CHECK_EQ(c.size(), std::size_t{42 * 2 + 1});
        // the default is off; JR written by hand is untouched
        CHECK_EQ(assemble("        ORG &4000\n        JP next\nnext:   RET\n", false).size(), std::size_t{4});
        CHECK_EQ(assemble("        ORG &4000\n        JR next\nnext:   RET\n", true).size(), std::size_t{3});
    }

    // ---- random programs: the same effect with and without -O ----
    std::mt19937 rng(4242);
    int compared = 0, shorter = 0;
    for (int round = 0; round < 3000; ++round) {
        const int blocks = 3 + static_cast<int>(rng() % 12);
        std::string src = "        ORG &4000\n        LD SP,&C000\n        LD HL,&8000\n";
        const char* work[] = {"INC A", "ADD A,B", "LD B,A", "INC HL", "LD (HL),A", "DEC C", "XOR &5A", "ADD A,7", "RLCA", "LD C,A",
                              "SUB 3", "OR B", "CPL", "LD D,A", "INC E", "LD (HL),C", "DEC HL", "CP &40"};  // HL stays near &8000: no writes into the code
        for (int b = 0; b < blocks; ++b) {
            src += "b" + std::to_string(b) + ":\n";
            const int n = 1 + static_cast<int>(rng() % 30);
            for (int k = 0; k < n; ++k) src += std::string("        ") + work[rng() % std::size(work)] + "\n";
            if (rng() % 8 == 0) src += "        DS " + std::to_string(rng() % 150) + "\n        ORG $\n";
            // forward jumps only (with a guard counter), so that the program always ends
            const int target = b + 1 + static_cast<int>(rng() % 3);
            const std::string t = target >= blocks ? "done" : "b" + std::to_string(target);
            const char* conds[] = {"", "NZ,", "Z,", "NC,", "C,", "PE,", "M,"};
            src += std::string("        JP ") + conds[rng() % std::size(conds)] + t + "\n";
        }
        src += "done:   HALT\n";
        const auto plain = assemble(src, false);
        const auto opt = assemble(src, true);
        if (!plain.ok() || !opt.ok()) {
            ++g_failures;
            std::cerr << "round " << round << ": does not assemble\n" << plain.formatDiagnostics("plain") << opt.formatDiagnostics("opt");
            continue;
        }
        if (opt.size() < plain.size()) ++shorter;
        CHECK(opt.size() <= plain.size());
        Memory m1, m2;
        plain.applyTo(m1);
        opt.applyTo(m2);
        Cpu c1(m1), c2(m2);
        c1.regs().pc = c2.regs().pc = 0x4000;
        RunLimits limits;
        limits.maxInstructions = 100000;
        const RunResult r1 = c1.run(limits);
        const RunResult r2 = c2.run(limits);
        ++compared;
        CHECK_EQ(std::string(toString(r1.reason)), std::string("halted"));
        CHECK_EQ(std::string(toString(r2.reason)), std::string("halted"));
        CHECK_EQ(r1.instructions, r2.instructions);  // the same path, instruction for instruction
        const Registers& a = c1.regs();
        const Registers& b = c2.regs();
        CHECK(a.af() == b.af() && a.bc() == b.bc() && a.de() == b.de() && a.hl() == b.hl() && a.sp == b.sp);
        bool sameData = true;
        for (unsigned addr = 0x7F00; addr < 0x8100 && sameData; ++addr) sameData = m1.read(static_cast<Word>(addr)) == m2.read(static_cast<Word>(addr));
        CHECK(sameData);
    }
    CHECK(compared > 2900);
    CHECK(shorter > 2500);
    return finish("optimize");
}
