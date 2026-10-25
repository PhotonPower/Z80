#!/usr/bin/env python3
"""Cross-check the decoder against an independent assembler (z80asm).

For every documented instruction form the decoder can produce, the instruction text is handed
to z80asm and the resulting bytes must equal the bytes the decoder started from.

Usage:
    cmake -S . -B build -DZ80_BUILD_TOOLS=ON && cmake --build build
    python3 tools/crosscheck_z80asm.py build/z80_dump_forms [path/to/z80asm]

z80asm is the GNU/Debian package "z80asm" (apt install z80asm).
"""
import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dump_forms = sys.argv[1]
    z80asm = sys.argv[2] if len(sys.argv) > 2 else shutil.which("z80asm")
    if not z80asm:
        print("z80asm not found (apt install z80asm), skipping")
        return 0

    forms = sorted(set(subprocess.run([dump_forms], capture_output=True, text=True, check=True).stdout.splitlines()))
    mismatches = 0
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = os.path.join(tmp, "c.asm"), os.path.join(tmp, "c.bin")
        for line in forms:
            expected, text = line.split("|", 1)
            with open(source, "w") as f:
                f.write(f"  org 0x4000\n  {text.lower()}\n")
            result = subprocess.run([z80asm, "-o", binary, source], capture_output=True, text=True)
            got = open(binary, "rb").read().hex().upper() if result.returncode == 0 else "ERROR " + result.stderr.strip()[:60]
            if got != expected:
                mismatches += 1
                print(f"MISMATCH {text!r}: decoder bytes {expected}, z80asm {got}")

    print(f"{len(forms)} instruction forms checked, {mismatches} mismatches")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
