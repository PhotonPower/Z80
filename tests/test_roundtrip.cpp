// Exhaustive consistency check between decoder and assembler:
// every documented encoding that the decoder produces must be reassembled to the same bytes.

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/decoder.hpp"

using z80::Byte;

int main() {
    constexpr z80::Word kAddress = 0x4000;

    struct Pattern {
        Byte a, b;  // operand bytes used to fill n / nn / d positions
    };
    // second pattern: negative displacements / high bit set / backwards jumps
    const Pattern patterns[] = {{0x12, 0x34}, {0xF2, 0x81}};

    int decodedValid = 0;
    int decodedInvalid = 0;

    for (const auto& pattern : patterns) {
        for (int prefixKind = 0; prefixKind < 7; ++prefixKind) {
            for (int op = 0; op < 256; ++op) {
                std::vector<Byte> bytes;
                switch (prefixKind) {
                    case 0: bytes = {static_cast<Byte>(op)}; break;
                    case 1: bytes = {0xED, static_cast<Byte>(op)}; break;
                    case 2: bytes = {0xCB, static_cast<Byte>(op)}; break;
                    case 3: bytes = {0xDD, static_cast<Byte>(op)}; break;
                    case 4: bytes = {0xFD, static_cast<Byte>(op)}; break;
                    case 5: bytes = {0xDD, 0xCB, pattern.a, static_cast<Byte>(op)}; break;
                    default: bytes = {0xFD, 0xCB, pattern.a, static_cast<Byte>(op)}; break;
                }
                // operand bytes follow
                bytes.push_back(pattern.a);
                bytes.push_back(pattern.b);
                bytes.push_back(pattern.a);
                bytes.push_back(pattern.b);

                // Skip bare prefixes (0xCB, 0xED, 0xDD, 0xFD as first byte are handled by the other kinds).
                if (prefixKind == 0 && (op == 0xCB || op == 0xED || op == 0xDD || op == 0xFD)) continue;

                z80::Memory memory;
                memory.load(kAddress, bytes);
                const auto ins = z80::decode(memory, kAddress);

                ++g_checks;
                if (ins.length < 1 || ins.length > 4) {
                    ++g_failures;
                    std::cerr << "bad length for " << hexBytes(bytes) << "\n";
                    continue;
                }
                if (!ins.valid) {
                    ++decodedInvalid;
                    CHECK_EQ(ins.mnemonic, std::string("DB"));
                    continue;
                }
                ++decodedValid;

                const std::string source = "ORG &4000\n" + ins.text() + "\n";
                const auto result = z80::Assembler().assemble(source);
                std::vector<Byte> expected(bytes.begin(), bytes.begin() + ins.length);
                ++g_checks;
                if (!result.ok() || result.flatten() != expected) {
                    ++g_failures;
                    std::cerr << "roundtrip failed: " << hexBytes(expected) << " -> '" << ins.text()
                              << "' -> " << (result.ok() ? hexBytes(result.flatten()) : "assembly error: " + result.diagnostics[0].message)
                              << "\n";
                }
            }
        }
    }

    std::cout << "decoded " << decodedValid / 2 << " valid and " << decodedInvalid / 2
              << " invalid opcode forms per operand pattern\n";
    // The documented Z80 instruction set has well over 600 distinct opcode forms.
    CHECK(decodedValid / 2 > 600);
    return finish("roundtrip");
}
