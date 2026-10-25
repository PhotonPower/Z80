#include "test_util.hpp"
#include "z80/decoder.hpp"

using z80::Byte;

namespace {

void dec(std::vector<Byte> bytes, const std::string& text, int length, z80::Word address = 0x100,
         z80::DecodeOptions options = {}) {
    z80::Memory m;
    m.load(address, bytes);
    const auto ins = z80::decode(m, address, options);
    ++g_checks;
    if (ins.text() != text || ins.length != length) {
        ++g_failures;
        std::cerr << hexBytes(bytes) << " -> '" << ins.text() << "' len " << int(ins.length)
                  << ", expected '" << text << "' len " << length << "\n";
    }
}

}  // namespace

int main() {
    dec({0x00}, "NOP", 1);
    dec({0x7E}, "LD A,(HL)", 1);
    dec({0x76}, "HALT", 1);
    dec({0x3E, 0x12}, "LD A,&12", 2);
    dec({0x21, 0x34, 0x12}, "LD HL,&1234", 3);
    dec({0x22, 0x34, 0x12}, "LD (&1234),HL", 3);
    dec({0x3A, 0x34, 0x12}, "LD A,(&1234)", 3);
    dec({0xC3, 0x34, 0x12}, "JP &1234", 3);
    dec({0xCD, 0x34, 0x12}, "CALL &1234", 3);
    dec({0xC8}, "RET Z", 1);
    dec({0xFF}, "RST &38", 1);
    dec({0xCF}, "RST &08", 1);
    dec({0xF1}, "POP AF", 1);
    dec({0xD3, 0xFE}, "OUT (&FE),A", 2);
    dec({0xDB, 0xFE}, "IN A,(&FE)", 2);
    dec({0x08}, "EX AF,AF'", 1);
    dec({0x86}, "ADD A,(HL)", 1);
    dec({0x90}, "SUB B", 1);
    dec({0xFE, 0x41}, "CP &41", 2);
    dec({0x2C}, "INC L", 1);  // wrong in the original BASIC table ("INC H")
    dec({0x2D}, "DEC L", 1);

    // relative jumps are shown as absolute targets
    dec({0x18, 0xFE}, "JR &0100", 2);
    dec({0x20, 0x05}, "JR NZ,&0107", 2);
    dec({0x10, 0xFC}, "DJNZ &00FE", 2);
    dec({0x38, 0x00}, "JR C,&0102", 2);

    // prefixes
    dec({0xCB, 0x7E}, "BIT 7,(HL)", 2);
    dec({0xCB, 0x00}, "RLC B", 2);
    dec({0xED, 0xB0}, "LDIR", 2);
    dec({0xED, 0x4B, 0x34, 0x12}, "LD BC,(&1234)", 4);
    dec({0xED, 0x56}, "IM 1", 2);
    dec({0xED, 0x47}, "LD I,A", 2);
    dec({0xDD, 0x7E, 0xFB}, "LD A,(IX-&05)", 3);
    dec({0xFD, 0x36, 0x05, 0x99}, "LD (IY+&05),&99", 4);
    dec({0xDD, 0x21, 0x00, 0x40}, "LD IX,&4000", 4);
    dec({0xDD, 0xE9}, "JP (IX)", 2);
    dec({0xDD, 0x29}, "ADD IX,IX", 2);
    dec({0xFD, 0xCB, 0x02, 0x46}, "BIT 0,(IY+&02)", 4);
    dec({0xDD, 0xCB, 0x80, 0x16}, "RL (IX-&80)", 4);
    dec({0xDD, 0x66, 0x01}, "LD H,(IX+&01)", 3);  // H stays H when a memory operand is present
    dec({0xDD, 0x74, 0x01}, "LD (IX+&01),H", 3);

    // undocumented / invalid encodings become DB
    dec({0xDD, 0x00}, "DB &DD", 1);
    dec({0xFD, 0xFD, 0x00}, "DB &FD", 1);
    dec({0xED, 0x77}, "DB &ED,&77", 2);
    dec({0xED, 0x00}, "DB &ED,&00", 2);
    dec({0xCB, 0x30}, "DB &CB,&30", 2);
    dec({0xDD, 0xCB, 0x01, 0x30}, "DB &DD,&CB,&01,&30", 4);
    dec({0xDD, 0x44}, "DB &DD", 1);  // LD B,IXH: undocumented

    // flow information
    {
        z80::Memory m;
        m.load(0x200, std::vector<Byte>{0xCD, 0x00, 0x30});
        auto call = z80::decode(m, 0x200);
        CHECK(call.flow == z80::FlowKind::Call);
        CHECK(call.target.has_value() && *call.target == 0x3000);
        m.load(0x200, std::vector<Byte>{0xC9});
        CHECK(z80::decode(m, 0x200).flow == z80::FlowKind::Return);
        m.load(0x200, std::vector<Byte>{0xC0});
        CHECK(z80::decode(m, 0x200).flow == z80::FlowKind::ConditionalReturn);
        m.load(0x200, std::vector<Byte>{0xE9});
        CHECK(z80::decode(m, 0x200).flow == z80::FlowKind::JumpIndirect);
        m.load(0x200, std::vector<Byte>{0xD7});
        auto rst = z80::decode(m, 0x200);
        CHECK(rst.flow == z80::FlowKind::Restart);
        CHECK(rst.target.has_value() && *rst.target == 0x10);
        m.load(0x200, std::vector<Byte>{0x28, 0x10});
        CHECK(z80::decode(m, 0x200).flow == z80::FlowKind::ConditionalJump);
        m.load(0x200, std::vector<Byte>{0xED, 0x4D});
        CHECK(z80::decode(m, 0x200).flow == z80::FlowKind::Return);
    }

    // number styles
    {
        z80::DecodeOptions o;
        o.style = z80::NumberStyle::Dollar;
        dec({0x3E, 0xAB}, "LD A,$AB", 2, 0x100, o);
        o.style = z80::NumberStyle::CPrefix;
        dec({0x21, 0x34, 0x12}, "LD HL,0x1234", 3, 0x100, o);
        o.style = z80::NumberStyle::HSuffix;
        dec({0x3E, 0xAB}, "LD A,0ABH", 2, 0x100, o);
        dec({0x3E, 0x12}, "LD A,12H", 2, 0x100, o);
    }

    // symbolizer
    {
        z80::DecodeOptions o;
        o.symbolizer = [](z80::Word a) -> std::optional<std::string> {
            if (a == 0x1234) return std::string("main");
            return std::nullopt;
        };
        dec({0xCD, 0x34, 0x12}, "CALL main", 3, 0x100, o);
        dec({0xCD, 0x35, 0x12}, "CALL &1235", 3, 0x100, o);
    }

    // wrap-around at the end of the address space
    {
        z80::Memory m;
        m.write(0xFFFF, 0xC3);
        m.write(0x0000, 0x00);
        m.write(0x0001, 0x80);
        const auto ins = z80::decode(m, 0xFFFF);
        CHECK_EQ(ins.text(), std::string("JP &8000"));
        CHECK_EQ(ins.next(), z80::Word{0x0002});
    }

    return finish("decoder");
}
