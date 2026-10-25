// Runs a CP/M .com test program (ZEXDOC / ZEXALL from the Z80 instruction exerciser) on the
// emulator. Only the two BDOS calls the exercisers use are emulated:
//   C = 2: print character in E        C = 9: print '$'-terminated string at DE
//
//   z80_zex zexdoc.cim

#include <chrono>
#include <iostream>
#include <string>

#include "z80/cpu.hpp"
#include "z80/image_io.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: z80_zex <test.com>\n";
        return 2;
    }
    z80::Memory memory;
    z80::Cpu cpu(memory);
    memory.load(0x0100, z80::readFile(argv[1]));
    cpu.regs().pc = 0x0100;
    cpu.regs().sp = 0xFFFF;

    std::string transcript;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        z80::Registers& r = cpu.regs();
        if (r.pc == 0x0000) break;  // warm boot = program finished
        if (r.pc == 0x0005) {       // BDOS entry
            const unsigned c = r.main[1];
            if (c == 2) {
                transcript += static_cast<char>(r.main[3]);
                std::cout << static_cast<char>(r.main[3]) << std::flush;
            } else if (c == 9) {
                for (z80::Word a = r.de(); memory.read(a) != '$'; ++a) {
                    transcript += static_cast<char>(memory.read(a));
                    std::cout << static_cast<char>(memory.read(a));
                }
                std::cout << std::flush;
            }
            r.pc = static_cast<z80::Word>(memory.read(r.sp) | (memory.read(static_cast<z80::Word>(r.sp + 1)) << 8));
            r.sp = static_cast<z80::Word>(r.sp + 2);
            continue;
        }
        cpu.step();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "\n[" << cpu.instructionCount() << " instructions, " << cpu.cycles() << " T-states, " << seconds
              << " s]\n";
    if (transcript.find("ERROR") != std::string::npos || transcript.find("Tests complete") == std::string::npos) {
        std::cout << "FAILED\n";
        return 1;
    }
    std::cout << "PASSED\n";
    return 0;
}
