# Tests

```sh
ctest --test-dir build --output-on-failure
```

Die Tests brauchen kein externes Framework; `tests/test_util.hpp` stellt `CHECK`/`CHECK_EQ` bereit.

| Suite              | Inhalt |
|--------------------|--------|
| `expression`       | alle Zahlenschreibweisen, Operatoren, Präzedenz, `$`, Symbole, Fehlerfälle |
| `assembler`        | über 120 Befehle mit erwarteten Bytes (aus der Zilog-Referenz), Labels, Vorwärtsreferenzen, `EQU`, mehrere `ORG`, Pseudobefehle, Reichweite von `JR`, Fehlermeldungen |
| `decoder`          | Befehle mit Bytes → Text/Länge, Präfixe, undokumentierte Formen, Flussinformation, Zahlenstile, Symbolizer, Überlauf bei `0xFFFF` |
| `roundtrip`        | **exhaustiv**: jeder Opcode mit jedem Präfix (`–`, `ED`, `CB`, `DD`, `FD`, `DD CB`, `FD CB`), je mit positiven und negativen Operandenbytes; was der Dekoder als Befehl erkennt, muss der Assembler zu denselben Bytes zurückübersetzen (870 Befehlsformen) |
| `disassembler`     | Ablaufanalyse, Aufrufer/Springer, linearer Durchlauf, Inline-Strings, Kommentardatei, Analysebereich, Null-Folgen, `DS`-Zusammenfassung |
| `image_io`         | `.sna` laden/patchen, Intel-HEX-Prüfsummen, Dateizugriff |
| `robustness`       | feste Zufallsfolge: für 150 Zufallsspeicher gilt `assemble(disassemble(x)) == x`; 6000 zufällige/halbstrukturierte Quelltexte dürfen den Assembler nicht zum Absturz bringen; jede akzeptierte Zeile muss vom Dekoder wieder als gültiger Befehl gleicher Länge erkannt werden |

Der Roundtrip-Test beweist die Konsistenz von Dekoder und Assembler **untereinander**. Gegen
gemeinsame Denkfehler schützt die Kreuzprüfung mit unabhängigen Werkzeugen.

## Kreuzprüfung gegen `z80asm` / `z80dasm`

```sh
sudo apt install z80asm z80dasm
cmake -S . -B build -DZ80_BUILD_TOOLS=ON && cmake --build build
python3 tools/crosscheck_z80asm.py build/z80_dump_forms
```

`z80_dump_forms` gibt jede vom Dekoder erkannte Befehlsform aus; das Skript lässt `z80asm` den Text
assemblieren und vergleicht die Bytes. Die CI führt das bei jedem Push aus.

Ergebnis bei der Entwicklung (Debian `z80asm` 1.8, `z80dasm` 1.1.6):

- **870 Befehlsformen, 0 Abweichungen** zwischen den Bytes des Dekoders und der Übersetzung durch
  `z80asm`.
- Umgekehrt wurden alle **1092** Byte-Folgen, die der Dekoder als ungültig (`DB`) einstuft, an
  `z80dasm` gegeben: 1090 davon erkennt auch `z80dasm` nicht als regulären Befehl; die übrigen
  zwei (`DD CB d 30`, `FD CB d 30`) sind `SLI`, also der undokumentierte `SLL`.

## Sanitizer

```sh
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DZ80_SANITIZE=ON
cmake --build build-san && ctest --test-dir build-san
```

Läuft ebenfalls in der CI (AddressSanitizer + UBSan).
