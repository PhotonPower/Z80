// Randomised (but deterministic) tests:
//  * for arbitrary memory contents, disassembling to source and assembling again must
//    reproduce the original bytes exactly,
//  * neither tool may crash on garbage input.

#include <random>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/disassembler.hpp"

using z80::Byte;
using z80::Word;

int main() {
    std::mt19937 rng(0xC0FFEE);

    // ---- disassemble -> assemble round trip on random data ----
    for (int iteration = 0; iteration < 150; ++iteration) {
        z80::Memory memory;
        const Word base = static_cast<Word>(rng() % 0xE000);
        const std::size_t size = 16 + rng() % 1500;
        // mix of random bytes and plausible code-like bytes to exercise flow following
        for (std::size_t i = 0; i < size; ++i) {
            Byte b = static_cast<Byte>(rng());
            if (rng() % 7 == 0) b = 0x00;
            if (rng() % 50 == 0) b = 0xC9;  // RET
            memory.write(static_cast<Word>(base + i), b);
        }

        z80::DisassemblerOptions options;
        options.zeroRunLimit = rng() % 2 ? 16 : 0;
        z80::Disassembler d(memory, options);
        d.setAnalysisRange(base, static_cast<Word>(base + size - 1));
        if (iteration % 3 == 0) {
            d.analyzeRange(base, static_cast<Word>(base + size - 1));
        } else {
            d.addEntryPoint(static_cast<Word>(base + rng() % size));
            d.analyzeFlow(base);
        }

        z80::RenderOptions render;
        render.asmSource = true;
        render.from = base;
        render.to = static_cast<Word>(base + size - 1);
        const std::string source = d.render(render);

        const auto result = z80::Assembler().assemble(source);
        ++g_checks;
        if (!result.ok()) {
            ++g_failures;
            std::cerr << "iteration " << iteration << ": generated source does not assemble\n"
                      << result.formatDiagnostics("generated") << "\n";
            continue;
        }
        const std::vector<Byte> bytes = result.flatten();
        ++g_checks;
        bool same = bytes.size() == size && result.lowAddress() == base;
        for (std::size_t i = 0; same && i < size; ++i) {
            same = bytes[i] == memory.read(static_cast<Word>(base + i));
        }
        if (!same) {
            ++g_failures;
            std::cerr << "iteration " << iteration << ": round trip mismatch (size " << bytes.size()
                      << " vs " << size << ")\n";
        }

        // the listing renderer must also cope with the same data
        const std::string listing = d.render({});
        CHECK(!listing.empty());
    }

    // ---- assembler must survive garbage ----
    {
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ,;:()+-*/$&%#'\"\t._@?!<>=~^|";
        for (int iteration = 0; iteration < 3000; ++iteration) {
            std::string source;
            const int lines = 1 + static_cast<int>(rng() % 6);
            for (int l = 0; l < lines; ++l) {
                const int length = static_cast<int>(rng() % 40);
                for (int i = 0; i < length; ++i) source += alphabet[rng() % alphabet.size()];
                source += '\n';
            }
            const auto r = z80::Assembler().assemble(source);
            (void)r.formatListing();
            (void)r.formatDiagnostics("fuzz");
            ++g_checks;  // reaching this point without crashing is the assertion
        }

        // structured garbage: real mnemonics with random operands
        const char* mnemonics[] = {"LD", "ADD", "JP", "JR", "CALL", "BIT", "IN", "OUT", "PUSH", "EX", "RST", "IM", "DB", "DW", "DS", "ORG", "EQU"};
        const char* operands[] = {"A", "B,C", "(HL)", "(IX+5)", "(IY-200)", "HL,(&1234)", "NZ,$", "7,(IX+1)", "&FFFF", "-1", "(", ")", ",", ",,", "1+", "AF'", "(C),A", "\"str\"", "'x'", "$+300"};
        for (int iteration = 0; iteration < 3000; ++iteration) {
            std::string source = "  ";
            source += mnemonics[rng() % std::size(mnemonics)];
            source += ' ';
            source += operands[rng() % std::size(operands)];
            if (rng() % 2) {
                source += ',';
                source += operands[rng() % std::size(operands)];
            }
            source += '\n';
            const auto r = z80::Assembler().assemble(source);
            ++g_checks;
            // an accepted line must produce exactly what the decoder reads back as a valid instruction
            if (r.ok() && r.size() > 0 && r.size() <= 4 && source.find("DB") == std::string::npos &&
                source.find("DW") == std::string::npos && source.find("DS") == std::string::npos &&
                source.find("ORG") == std::string::npos && source.find("EQU") == std::string::npos) {
                z80::Memory m;
                r.applyTo(m);
                const auto ins = z80::decode(m, r.lowAddress());
                ++g_checks;
                if (!ins.valid || ins.length != r.size()) {
                    ++g_failures;
                    std::cerr << "assembled '" << source << "' to bytes the decoder rejects or reads with another length\n";
                }
            }
        }
    }

    return finish("robustness");
}
