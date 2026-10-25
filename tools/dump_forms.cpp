// Prints every documented instruction form the decoder produces, one per line:
//
//     <hex bytes>|<instruction text, 0x-style numbers>
//
// Used by tools/crosscheck_z80asm.py to compare against an independent assembler.

#include <iostream>
#include <vector>

#include "z80/decoder.hpp"

int main() {
    using z80::Byte;
    const Byte patterns[2][2] = {{0x12, 0x34}, {0xF2, 0x81}};  // operand fill bytes (positive / negative)

    for (const auto& pattern : patterns) {
        for (int kind = 0; kind < 7; ++kind) {
            for (int op = 0; op < 256; ++op) {
                std::vector<Byte> bytes;
                switch (kind) {
                    case 0: bytes = {static_cast<Byte>(op)}; break;
                    case 1: bytes = {0xED, static_cast<Byte>(op)}; break;
                    case 2: bytes = {0xCB, static_cast<Byte>(op)}; break;
                    case 3: bytes = {0xDD, static_cast<Byte>(op)}; break;
                    case 4: bytes = {0xFD, static_cast<Byte>(op)}; break;
                    case 5: bytes = {0xDD, 0xCB, pattern[0], static_cast<Byte>(op)}; break;
                    default: bytes = {0xFD, 0xCB, pattern[0], static_cast<Byte>(op)}; break;
                }
                for (int i = 0; i < 2; ++i) {
                    bytes.push_back(pattern[0]);
                    bytes.push_back(pattern[1]);
                }
                if (kind == 0 && (op == 0xCB || op == 0xED || op == 0xDD || op == 0xFD)) continue;

                z80::Memory memory;
                memory.load(0x4000, bytes);
                z80::DecodeOptions options;
                options.style = z80::NumberStyle::CPrefix;
                const auto ins = z80::decode(memory, 0x4000, options);
                if (!ins.valid) continue;
                for (int i = 0; i < ins.length; ++i) std::cout << z80::hexDigits(ins.bytes[i], 2);
                std::cout << "|" << ins.text() << "\n";
            }
        }
    }
}
