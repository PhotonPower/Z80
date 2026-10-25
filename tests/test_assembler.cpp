#include "test_util.hpp"
#include "z80/assembler.hpp"

using z80::Byte;

namespace {

std::vector<Byte> asmOne(const std::string& line, z80::Word org = 0) {
    z80::AssemblerOptions options;
    options.origin = org;
    const auto r = z80::Assembler(options).assemble(line + "\n");
    if (!r.ok()) return {};
    return r.flatten();
}

void vec(const std::string& line, std::vector<Byte> expected) {
    ++g_checks;
    const auto actual = asmOne(line);
    if (actual != expected) {
        ++g_failures;
        std::cerr << "'" << line << "' -> " << hexBytes(actual) << ", expected " << hexBytes(expected) << "\n";
    }
}

void rejects(const std::string& source, const std::string& messagePart) {
    ++g_checks;
    const auto r = z80::Assembler().assemble(source);
    bool found = false;
    for (const auto& d : r.diagnostics) {
        if (d.message.find(messagePart) != std::string::npos) found = true;
    }
    if (r.ok() || !found) {
        ++g_failures;
        std::cerr << "source should fail with '" << messagePart << "':\n" << source << "\n"
                  << r.formatDiagnostics("test");
    }
}

}  // namespace

int main() {
    // ---- loads ----
    vec("NOP", {0x00});
    vec("LD B,C", {0x41});
    vec("LD A,(HL)", {0x7E});
    vec("LD (HL),A", {0x77});
    vec("LD A,&12", {0x3E, 0x12});
    vec("LD (HL),&34", {0x36, 0x34});
    vec("LD HL,&1234", {0x21, 0x34, 0x12});
    vec("LD SP,HL", {0xF9});
    vec("LD SP,IX", {0xDD, 0xF9});
    vec("LD (&1234),HL", {0x22, 0x34, 0x12});
    vec("LD HL,(&1234)", {0x2A, 0x34, 0x12});
    vec("LD (&1234),A", {0x32, 0x34, 0x12});
    vec("LD A,(&1234)", {0x3A, 0x34, 0x12});
    vec("LD BC,(&1234)", {0xED, 0x4B, 0x34, 0x12});
    vec("LD (&1234),SP", {0xED, 0x73, 0x34, 0x12});
    vec("LD (IX+5),&99", {0xDD, 0x36, 0x05, 0x99});
    vec("LD (IY-1),B", {0xFD, 0x70, 0xFF});
    vec("LD C,(IX+2)", {0xDD, 0x4E, 0x02});
    vec("LD H,(IX+0)", {0xDD, 0x66, 0x00});
    vec("LD IX,&4000", {0xDD, 0x21, 0x00, 0x40});
    vec("LD (&4000),IY", {0xFD, 0x22, 0x00, 0x40});
    vec("LD A,I", {0xED, 0x57});
    vec("LD R,A", {0xED, 0x4F});
    vec("LD (BC),A", {0x02});
    vec("LD A,(DE)", {0x1A});
    vec("ld a,b", {0x78});  // case-insensitive
    vec("LD A, B", {0x78});  // spaces after comma

    // ---- stack / exchange ----
    vec("PUSH AF", {0xF5});
    vec("POP BC", {0xC1});
    vec("PUSH IX", {0xDD, 0xE5});
    vec("EX DE,HL", {0xEB});
    vec("EX AF,AF'", {0x08});
    vec("EX (SP),HL", {0xE3});
    vec("EX (SP),IY", {0xFD, 0xE3});
    vec("EXX", {0xD9});

    // ---- arithmetic / logic ----
    vec("ADD A,B", {0x80});
    vec("ADD A,&10", {0xC6, 0x10});
    vec("ADD B", {0x80});
    vec("ADC A,(HL)", {0x8E});
    vec("SUB &05", {0xD6, 0x05});
    vec("SBC A,C", {0x99});
    vec("AND (IX+3)", {0xDD, 0xA6, 0x03});
    vec("XOR A", {0xAF});
    vec("OR B", {0xB0});
    vec("CP &FF", {0xFE, 0xFF});
    vec("ADD HL,DE", {0x19});
    vec("ADD IX,BC", {0xDD, 0x09});
    vec("ADD IX,IX", {0xDD, 0x29});
    vec("ADD IY,SP", {0xFD, 0x39});
    vec("ADC HL,BC", {0xED, 0x4A});
    vec("SBC HL,SP", {0xED, 0x72});
    vec("INC B", {0x04});
    vec("DEC C", {0x0D});
    vec("INC (HL)", {0x34});
    vec("DEC (IX+1)", {0xDD, 0x35, 0x01});
    vec("INC BC", {0x03});
    vec("DEC SP", {0x3B});
    vec("INC IX", {0xDD, 0x23});
    vec("DEC IY", {0xFD, 0x2B});

    // ---- rotate / bit ----
    vec("RLC B", {0xCB, 0x00});
    vec("RRC C", {0xCB, 0x09});
    vec("RL D", {0xCB, 0x12});
    vec("RR E", {0xCB, 0x1B});
    vec("SLA H", {0xCB, 0x24});
    vec("SRA L", {0xCB, 0x2D});
    vec("SRL A", {0xCB, 0x3F});
    vec("RLC (HL)", {0xCB, 0x06});
    vec("RL (IX+2)", {0xDD, 0xCB, 0x02, 0x16});
    vec("BIT 0,B", {0xCB, 0x40});
    vec("BIT 7,(HL)", {0xCB, 0x7E});
    vec("RES 3,C", {0xCB, 0x99});
    vec("SET 7,A", {0xCB, 0xFF});
    vec("SET 1,(IY+4)", {0xFD, 0xCB, 0x04, 0xCE});

    // ---- jumps / calls ----
    vec("JP &1234", {0xC3, 0x34, 0x12});
    vec("JP NZ,&1234", {0xC2, 0x34, 0x12});
    vec("JP M,&1234", {0xFA, 0x34, 0x12});
    vec("JP (HL)", {0xE9});
    vec("JP (IX)", {0xDD, 0xE9});
    vec("JR $", {0x18, 0xFE});
    vec("JR C,$", {0x38, 0xFE});
    vec("JR NZ,$+2", {0x20, 0x00});
    vec("DJNZ $", {0x10, 0xFE});
    vec("CALL &1234", {0xCD, 0x34, 0x12});
    vec("CALL Z,&1234", {0xCC, 0x34, 0x12});
    vec("RET", {0xC9});
    vec("RET NZ", {0xC0});
    vec("RET M", {0xF8});
    vec("RST &08", {0xCF});
    vec("RST 0", {0xC7});
    vec("RST &38", {0xFF});

    // ---- I/O and misc ----
    vec("IN A,(&FE)", {0xDB, 0xFE});
    vec("IN B,(C)", {0xED, 0x40});
    vec("OUT (&FE),A", {0xD3, 0xFE});
    vec("OUT (C),E", {0xED, 0x59});
    vec("IM 0", {0xED, 0x46});
    vec("IM 1", {0xED, 0x56});
    vec("IM 2", {0xED, 0x5E});
    vec("NEG", {0xED, 0x44});
    vec("LDIR", {0xED, 0xB0});
    vec("CPDR", {0xED, 0xB9});
    vec("RETI", {0xED, 0x4D});
    vec("RRD", {0xED, 0x67});
    vec("HALT", {0x76});
    vec("DI", {0xF3});
    vec("EI", {0xFB});
    vec("SCF", {0x37});
    vec("DAA", {0x27});

    // ---- numbers / expressions in operands ----
    vec("LD A,10", {0x3E, 0x0A});
    vec("LD A,0FFh", {0x3E, 0xFF});
    vec("LD A,-1", {0x3E, 0xFF});
    vec("LD A,'A'", {0x3E, 0x41});
    vec("LD HL,(&10+2)*2", {0x21, 0x24, 0x00});
    vec("LD A,(IX+&10)", {0xDD, 0x7E, 0x10});
    vec("LD A,(IX + 3)", {0xDD, 0x7E, 0x03});
    vec("LD A,(IX)", {0xDD, 0x7E, 0x00});

    // ---- pseudo instructions ----
    vec("DB 1,2,&FF", {1, 2, 0xFF});
    vec("DB \"Hi;\",0", {0x48, 0x69, 0x3B, 0x00});
    vec("DW &1234,5", {0x34, 0x12, 0x05, 0x00});
    vec("DS 3,&AA", {0xAA, 0xAA, 0xAA});
    vec("DM \"AB\"", {0x41, 0x42});
    vec("NOP ; comment", {0x00});
    vec("EX AF,AF' ; comment with 'quote", {0x08});

    // ---- labels, forward references, ORG, EQU ----
    {
        const auto r = z80::Assembler().assemble(
            "        ORG &8000\n"
            "start:  LD HL,data      ; forward reference\n"
            "loop    DJNZ loop\n"
            "        JR NZ,done\n"
            "        CALL subr\n"
            "done:   RET\n"
            "subr    RET\n"
            "data    DB 1,2\n"
            "COUNT   EQU 5\n"
            "        LD B,count      ; case-insensitive\n"
            "        END\n"
            "        NOP\n");
        CHECK(r.ok());
        CHECK_EQ(r.symbols.at("START").value, 0x8000);
        CHECK_EQ(r.symbols.at("LOOP").value, 0x8003);
        CHECK_EQ(r.symbols.at("DONE").value, 0x800A);
        CHECK_EQ(r.symbols.at("SUBR").value, 0x800B);
        CHECK_EQ(r.symbols.at("DATA").value, 0x800C);
        CHECK_EQ(r.symbols.at("COUNT").value, 5);
        CHECK(r.symbols.at("COUNT").isEqu);
        CHECK(!r.symbols.at("START").isEqu);
        const std::vector<Byte> expected{0x21, 0x0C, 0x80, 0x10, 0xFE, 0x20, 0x03, 0xCD, 0x0B, 0x80,
                                         0xC9, 0xC9, 0x01, 0x02, 0x06, 0x05};
        CHECK_EQ(r.lowAddress(), 0x8000);
        CHECK(r.flatten() == expected);  // END stops assembly: the trailing NOP is ignored
    }

    // EQU defined after its use
    {
        const auto r = z80::Assembler().assemble("        LD A,LATER\nLATER   EQU 42\n");
        CHECK(r.ok());
        CHECK_EQ(r.flatten()[1], Byte{42});
    }
    // multiple ORG segments
    {
        const auto r = z80::Assembler().assemble("ORG &100\nDB 1\nORG &200\nDB 2\n");
        CHECK(r.ok());
        CHECK_EQ(r.segments.size(), std::size_t{2});
        CHECK_EQ(r.segments[1].start, z80::Word{0x200});
        CHECK_EQ(r.flatten(0xEE).size(), std::size_t{0x101});
        CHECK_EQ(r.flatten(0xEE)[1], Byte{0xEE});
    }
    // origin option
    CHECK(asmOne("JR $", 0x4000) == std::vector<Byte>({0x18, 0xFE}));
    // relative jump limits: +127 / -128 are fine, +128 / -129 are not
    CHECK(!asmOne("JR $+129").empty());
    CHECK(!asmOne("JR $-126").empty());
    CHECK(asmOne("JR $+130").empty());
    CHECK(asmOne("JR $-127").empty());

    // ---- errors ----
    rejects("FOO A,B\n", "unknown mnemonic");
    rejects("        FOO\n", "unknown mnemonic");
    rejects("LD A\n", "invalid operands");
    rejects("LD (HL),(HL)\n", "invalid operands");
    rejects("LD A,&1234\n", "8-bit");
    rejects("LD HL,&12345\n", "16-bit");
    rejects("JR NZ,&8000\n", "relative jump");
    rejects("JP undefined_label\n", "undefined symbol");
    rejects("lbl: NOP\nlbl: NOP\n", "duplicate label");
    rejects("HL: NOP\n", "reserved");
    rejects("sub: NOP\n", "reserved");
    rejects("NOP A\n", "no operands");
    rejects("BIT 8,A\n", "bit number");
    rejects("RST 3\n", "RST");
    rejects("IM 3\n", "interrupt mode");
    rejects("LD A,(IX+200)\n", "displacement");
    rejects("JR C,Z,5\n", "invalid");
    rejects("JR PO,$\n", "invalid operands");
    rejects("LD A,1+\n", "expression");
    rejects("DS\n", "DS");

    // diagnostics carry the line number
    {
        const auto r = z80::Assembler().assemble("NOP\nNOP\n  BOGUS\n");
        CHECK_EQ(r.errorCount(), std::size_t{1});
        CHECK_EQ(r.diagnostics[0].line, 3);
    }

    // listing and symbol output do not crash and contain addresses
    {
        const auto r = z80::Assembler().assemble("ORG &1000\nstart: LD A,1\n");
        CHECK(r.formatListing().find("1000  3E01") != std::string::npos);
        CHECK(r.formatSymbols().find("START") != std::string::npos);
    }

    return finish("assembler");
}
