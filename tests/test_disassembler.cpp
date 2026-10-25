#include <sstream>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/disassembler.hpp"

using z80::Byte;
using z80::Word;

namespace {

const char* kProgram =
    "        ORG &4000\n"
    "start:  CALL sub1\n"
    "        LD A,B\n"
    "        JR NZ,skip\n"
    "        INC A\n"
    "skip:   JP done\n"
    "sub1:   LD B,5\n"
    "loop:   DJNZ loop\n"
    "        RET\n"
    "done:   HALT\n"
    "        RET\n"
    "        DB 'T','E','X','T'\n";  // data behind the code, never reached

z80::Memory assembleToMemory(const char* source) {
    const auto r = z80::Assembler().assemble(source);
    if (!r.ok()) {
        std::cerr << r.formatDiagnostics("test");
    }
    z80::Memory m;
    r.applyTo(m);
    return m;
}

}  // namespace

int main() {
    const z80::Memory memory = assembleToMemory(kProgram);

    // ---- flow analysis ("chain mode") ----
    {
        z80::Disassembler d(memory);
        d.analyzeFlow(0x4000);

        CHECK(d.isInstructionStart(0x4000));   // CALL
        CHECK(d.isInstructionStart(0x4003));   // LD A,B
        CHECK(d.isInstructionStart(0x4004));   // JR NZ
        CHECK(d.isInstructionStart(0x4006));   // INC A
        CHECK(d.isInstructionStart(0x4007));   // JP done
        CHECK(d.isInstructionStart(0x400A));   // LD B,5  (sub1, reached via CALL)
        CHECK(d.isInstructionStart(0x400C));   // DJNZ
        CHECK(d.isInstructionStart(0x400E));   // RET
        CHECK(d.isInstructionStart(0x400F));   // HALT (reached via JP)
        CHECK(d.isInstructionStart(0x4010));   // RET
        CHECK(!d.isCovered(0x4011));           // trailing data is not code

        CHECK_EQ(d.instructionCount(), std::size_t{10});
        CHECK_EQ(d.lowestAddress(), Word{0x4000});
        CHECK_EQ(d.highestAddress(), Word{0x4010});

        CHECK(d.callers().count(0x400A) == 1);
        CHECK(d.callers().at(0x400A).count(0x4000) == 1);
        CHECK(d.jumpers().count(0x4007) == 1);   // JR NZ,skip
        CHECK(d.jumpers().at(0x4007).count(0x4004) == 1);
        CHECK(d.jumpers().count(0x400C) == 1);   // DJNZ loop
        CHECK(d.jumpers().count(0x400F) == 1);   // JP done
        CHECK(d.jumpers().count(0x4006) == 0);

        const std::string listing = d.render();
        CHECK(listing.find("CALL &400A") != std::string::npos);
        CHECK(listing.find("JR NZ,&4007") != std::string::npos);
        CHECK(listing.find("<- 4000") != std::string::npos);
    }

    // ---- linear sweep ----
    {
        z80::Disassembler d(memory);
        d.analyzeRange(0x4000, 0x4010);
        CHECK_EQ(d.instructionCount(), std::size_t{10});
    }

    // ---- re-assemblable output: assemble(disassemble(x)) == x ----
    {
        z80::Disassembler d(memory);
        d.analyzeFlow(0x4000);
        z80::RenderOptions o;
        o.asmSource = true;
        o.to = 0x4014;  // include the trailing data
        const std::string source = d.render(o);
        const auto r = z80::Assembler().assemble(source);
        CHECK(r.ok());
        if (!r.ok()) std::cerr << r.formatDiagnostics("generated") << source;
        const auto bytes = r.flatten();
        CHECK_EQ(bytes.size(), std::size_t{0x15});
        bool same = bytes.size() == 0x15;
        for (std::size_t i = 0; same && i < bytes.size(); ++i) {
            same = bytes[i] == memory.read(static_cast<Word>(0x4000 + i));
        }
        CHECK(same);
    }

    // ---- inline strings after a CALL ----
    {
        z80::Memory m = assembleToMemory(
            "        ORG &5000\n"
            "        CALL print\n"
            "        DB 'H','i'+&80\n"       // string terminated by a bit-7 byte
            "        LD A,1\n"
            "        RET\n"
            "print:  RET\n");
        z80::Disassembler d(m);
        d.addInlineData(0x5008, z80::InlineDataKind::StringBit7);
        d.analyzeFlow(0x5000);
        CHECK(d.isInstructionStart(0x5005));   // LD A,1 follows the string
        CHECK(!d.isInstructionStart(0x5003));  // the string itself is data
        CHECK(d.isCovered(0x5004));
    }

    // ---- no inline hook: the same bytes are (mis)read as code ----
    {
        z80::Memory m = assembleToMemory("ORG &5000\nCALL &5008\nDB &48,&E9\nLD A,1\nRET\nRET\n");
        z80::Disassembler d(m);
        d.analyzeFlow(0x5000);
        CHECK(d.isInstructionStart(0x5003));
    }

    // ---- comment file ----
    {
        z80::Disassembler d(memory);
        std::istringstream in(
            "; a comment line\n"
            "&400A,\"subroutine one\"\n"
            "16399,main loop end\n"
            "garbage line\n"
            "\n");
        const auto warnings = d.loadComments(in);
        CHECK_EQ(warnings.size(), std::size_t{1});
        d.analyzeFlow(0x4000);
        const std::string listing = d.render();
        CHECK(listing.find("subroutine one") != std::string::npos);
        CHECK(listing.find("main loop end") != std::string::npos);
        CHECK(listing.find("-> subroutine one") != std::string::npos);  // CALL to a commented address
    }

    // ---- comment addresses are entry points ----
    {
        z80::Memory m = assembleToMemory("ORG &6000\nRET\nLD A,5\nRET\n");
        z80::Disassembler d(m);
        d.addComment(0x6001, "hidden routine");
        d.analyzeFlow(0x6000);
        CHECK(d.isInstructionStart(0x6001));
        z80::DisassemblerOptions off;
        off.commentAddressesAreEntryPoints = false;
        z80::Disassembler d2(m, off);
        d2.addComment(0x6001, "hidden routine");
        d2.analyzeFlow(0x6000);
        CHECK(!d2.isInstructionStart(0x6001));
    }

    // ---- empty / degenerate input ----
    {
        z80::Memory m;
        z80::Disassembler d(m);
        CHECK(!d.hasResult());
        CHECK(d.render().find("nothing analysed") != std::string::npos);
        d.analyzeFlow(0xFFFF);  // uninitialised memory (zero run): nothing is decoded
        CHECK(!d.hasResult());

        z80::DisassemblerOptions noLimit;
        noLimit.zeroRunLimit = 0;
        z80::Disassembler d2(m, noLimit);
        d2.analyzeFlow(0xFFFF);  // wraps around the address space; must terminate
        CHECK(d2.hasResult());
        CHECK_EQ(d2.instructionCount(), std::size_t{65536});
    }

    // ---- analysis range: code outside the loaded image is not decoded ----
    {
        z80::Memory m = assembleToMemory("ORG &4000\nCALL &9000\nRET\n");
        z80::Disassembler d(m);
        d.setAnalysisRange(0x4000, 0x4003);  // only CALL fits (RET is at 4003.. inclusive)
        d.addEntryPoint(0xBB5A);             // outside: ignored
        d.analyzeFlow(0x4000);
        CHECK(d.isInstructionStart(0x4000));
        CHECK(!d.isCovered(0x9000));
        CHECK(!d.isCovered(0xBB5A));
        CHECK(d.callers().count(0x9000) == 1);  // the reference itself is still recorded
    }

    // ---- long identical runs collapse into DS and stay re-assemblable ----
    {
        z80::Memory m = assembleToMemory("ORG &7000\nRET\nDS 100,&AA\nDB 1,2,3\n");
        z80::Disassembler d(m);
        d.analyzeFlow(0x7000);
        z80::RenderOptions o;
        o.asmSource = true;
        o.to = 0x7000 + 103;
        const std::string source = d.render(o);
        CHECK(source.find("DS 100,&AA") != std::string::npos);
        const auto r = z80::Assembler().assemble(source);
        CHECK(r.ok());
        CHECK_EQ(r.size(), std::size_t{104});
        const auto bytes = r.flatten();
        bool same = bytes.size() == 104;
        for (std::size_t i = 0; same && i < bytes.size(); ++i) {
            same = bytes[i] == m.read(static_cast<Word>(0x7000 + i));
        }
        CHECK(same);
    }

    return finish("disassembler");
}
