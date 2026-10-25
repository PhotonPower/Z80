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

| `cpu`              | Emulator: Flags, BCD, Zeitverhalten (inkl. indizierter Befehle, `LDIR`, `DJNZ`), Block-/Index-/undokumentierte Befehle, Aufruf/Rückkehr, `runUntilReturn` (auch verschachtelt), `stepOver`, Breakpoints, Limits, Watchpoints, Interrupts IM 1/2, EI-Verzögerung, NMI/`RETN`, `HALT`, Ports, Registernamen, Snapshots (Roundtrip, beschädigte Dateien), JSON-Zustand, **40 × Zufallscode: Wiederholung aus Snapshot ist identisch** |
| `session`          | JSON-Parser/-Ausgabe (inkl. Fehlerfälle, Verschachtelungsgrenze), Agenten-Protokoll: Assemblieren/Laufen/Lesen, Breakpoints, `call`, Checkpoints, `diff`, E/A, Interrupts, Fehlerbehandlung, gesperrter Dateizugriff, Snapshot-Dateien |

| `monitor`          | Hex-Standard der Ausdrücke, vordefinierte Symbole im Assembler, alle Monitorbefehle (Speicher, Assemble-Modus, Lauf/Step/Haltepunkte/Watchpoints, Register, Checkpoints, Interrupts, E/A, Symbole, Dateien inkl. gesperrtem Dateizugriff), Robustheit gegen fehlerhafte Eingaben |

| `cpc`              | Firmware-Tabelle (231 Einträge, keine Doppelten, dichte Sprungleiste, `SCR_FLOOD_BOX`), Farben, Port-Ausdrücke und -Beschreibungen, Videozustand aus Port-Schreibzugriffen, Pixeldekodierung der Modi 0/1/2, Speicherlayout, Scrolling, Rand, Bildvergleich, Ausschnitt/Zoom, PNG (Chunk-CRCs unabhängig geprüft), Base64, `.sna`-Header |
| `listing`          | Annotationsregeln (Namen, Duplikate, reservierte Wörter, JSON), Listing mit/ohne Annotationen **mit Neu-Assemblieren-Prüfung**, Hardware-Kommentare, Teilbereiche, abgeschnittene Befehle, `DS`-Läufe, Bit-7-Text |
| `reverse`          | Session-Befehle: CPC-Lauf mit Firmware-Stubs, Screenshot/Diff/Zoom, Routinenliste, Annotationen, Listing, Dateien (Snapshot, ROM-Überlagerung, Annotationen), Interrupts, Video-Rollback |
| `agent`            | Python: Orchestrator gegen einen **scripteten Mock-Ollama** und den echten `z80tool serve`: Bilder erreichen das Modell, Fehlertexte werden zurückgemeldet, Reihenfolge bottom-up, Wiederaufnahme, nie endendes Modell, nicht erreichbarer Server |

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

## Emulator: ZEXDOC und ZEXALL

Die Befehlstestprogramme von Frank Cringle (ZEXDOC prüft dokumentierte Flags, ZEXALL zusätzlich die
undokumentierten) prüfen jeden Befehl mit Millionen Eingaben gegen CRC-Werte, die von echter
Hardware stammen.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZ80_BUILD_TOOLS=ON && cmake --build build
curl -LO https://raw.githubusercontent.com/superzazu/z80/master/roms/zexdoc.cim   # MIT, superzazu/z80
curl -LO https://raw.githubusercontent.com/superzazu/z80/master/roms/zexall.cim
./build/z80_zex zexdoc.cim
./build/z80_zex zexall.cim
```

Ergebnis: **beide 67/67 `OK`, 0 `ERROR`** (je ≈5,8 Mrd. Befehle, ≈ 1 Minute im Release-Build). Der
Runner gibt bei einem Fehler den Exit-Code 1 zurück. Die Programme liegen nicht im Repository;
die CI lädt sie (Job `zex`). Eine Abkürzung für den Alltag gibt es nicht: ohne Release-Build ist der
Lauf deutlich langsamer.

## Sanitizer

```sh
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DZ80_SANITIZE=ON
cmake --build build-san && ctest --test-dir build-san
```

Läuft ebenfalls in der CI (AddressSanitizer + UBSan).
