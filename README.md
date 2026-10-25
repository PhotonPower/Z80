# Z80 Tools

[![CI](https://github.com/PhotonPower/Z80/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/Z80/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Z80-Assembler, -Disassembler und **-Emulator** in modernem C++20 – hervorgegangen aus zwei
BASIC-Programmen (`Z80_Assembler.bas`, `z80_dissass.bas`). Der Emulator ist als Ausführungsumgebung
für einen lokalen LLM-Agenten gedacht; geplant ist außerdem der Ausbau zu einem vollständigen
**Maschinensprache-Monitor** ([Roadmap](docs/roadmap.md)).

## Funktionen

**Assembler**
- alle dokumentierten Z80-Befehle inkl. `IX`/`IY`-Adressierung
- Labels mit Vorwärtsreferenzen, `EQU`, `ORG` (auch mehrfach), `DB`/`DW`/`DM`/`DS`, `END`
- Ausdrücke: `+ - * / % << >> & | ^ ~`, Klammern, `$` (aktuelle Adresse), `HIGH()`, `LOW()`
- Zahlen als `123`, `&1F`, `$1F`, `0x1F`, `1Fh`, `%1010`, `'A'`
- Ausgabe: Raw-Binary, Intel HEX, direkt in einen Amstrad-CPC-`.sna`-Snapshot, Listing, Symboltabelle
- Fehlermeldungen mit Zeilennummer; alle Fehler eines Laufs werden gesammelt

**Disassembler**
- Ablaufanalyse („Kettenmodus“): folgt `CALL`, `JP`, `JR`, `DJNZ` ab einem Einstiegspunkt und trennt
  so Code von Daten; alternativ linearer Durchlauf über einen Adressbereich
- Querverweise: zu jedem Sprung-/Aufrufziel werden die Aufrufer angezeigt
- Kommentardatei (`Adresse,"Text"`), Inline-Daten hinter einem `CALL` (z. B. Texte)
- Ausgabe als lesbares Listing **oder** als wieder assemblierbarer Quelltext (`--asm`, mit Labels)
- Eingabe: Raw-Binary oder CPC-`.sna`-Snapshot

**Emulator** ([docs/emulator.md](docs/emulator.md))
- befehlsgenauer Z80-Kern: **besteht ZEXDOC und ZEXALL** (inkl. undokumentierter Flags und `MEMPTR`),
  führt auch undokumentierte Befehle (`SLL`, `IXH/IXL`, …) aus; T-States pro Befehl
- Einzelschritt, Lauf bis Breakpoint/Adresse/Rückkehr (`runUntilReturn`), Step-over, Watchpoints,
  Befehls-/Zyklenlimits, Trace-Hook, Interrupts (IM 0/1/2) und NMI, Port-Handler
- **Zustand speichern/laden:** Checkpoints im Speicher (CPU + 64 KiB), Snapshot-Datei, CPU-Zustand
  als JSON; Wiederaufnahme ist bit-identisch
- **Agenten-Schnittstelle** `z80tool serve`: JSON-Zeilenprotokoll über stdin/stdout
  (assemblieren, ausführen, Register/Speicher lesen und setzen, `call`, `diff`, Checkpoints …),
  Dateizugriff nur mit `--allow-files` ([docs/agent-protocol.md](docs/agent-protocol.md))

**Reverse Engineering mit lokalem LLM** ([docs/reverse-engineering.md](docs/reverse-engineering.md))
- `agent/z80_annotate.py` lässt ein Modell über **Ollama** (z. B. **Qwen3-VL**) Unterprogramm für
  Unterprogramm analysieren: lesen, im Emulator **ausführen und testen**, **Screenshots anfordern**
  (als Bild, auf den geänderten Bereich zugeschnitten), und dann Namen und Kommentare vergeben
- Ergebnis: ein kommentiertes Listing mit sinnvollen Namen, Firmware-Aufrufen (`CALL TXT_OUTPUT`) und
  Hardware-Zugriffen (`GATE_ARRAY+&8D`, mit Erklärung) als Symbole – **neu assembliert und Byte für Byte
  verifiziert**, das Modell kann den Code also nicht verändern
- Amstrad CPC: 231 Firmware-Einträge, Gate Array/CRTC/PPI, Videorendering (Modi 0–2), Snapshots, ROMs

**Monitor** ([docs/monitor.md](docs/monitor.md)) – `z80tool monitor`
- klassische Eingabeschleife mit Hex-Standard: disassemblieren (`d`), Speicher ansehen/ändern/
  füllen/kopieren/suchen (`m e f t h c`), **Zeilen assemblieren** (`a`), Register (`r`)
- ausführen im Emulator: Einzelschritt (`n`), Step over (`o`), bis Rückkehr (`u`), `g`, Haltepunkte
  (`b`), Watchpoints (`w`), Checkpoints (`ck`/`rb`), Symbole (`sym`), Dateien (`asm`, `l`, `sv`)
- als Klasse `z80::Monitor` ohne eigene Ein-/Ausgabe, deshalb testbar und einbettbar

Alles ist getestet: u. a. ein exhaustiver Roundtrip über alle Opcode-Präfixe, ein Property-Test
(`assemble(disassemble(x)) == x` für Zufallsdaten) und eine Kreuzprüfung gegen den unabhängigen
Assembler `z80asm` ([docs/testing.md](docs/testing.md)).

## Schnellstart

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build                  # Tests

./build/z80tool asm examples/hello_cpc.asm -l -         # assemblieren, Listing auf den Bildschirm
./build/z80tool dis hello_cpc.bin --org 0x4000          # disassemblieren
./build/z80tool monitor                                 # interaktiver Monitor (? für Hilfe)
./build/z80tool serve < examples/agent_session.jsonl    # Emulator per JSON-Protokoll
```

Voraussetzungen: CMake ≥ 3.16, C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

## Beispiel

`examples/hello_cpc.asm` assemblieren und wieder zerlegen:

```console
$ z80tool asm examples/hello_cpc.asm -o hello.bin
examples/hello_cpc.asm: 26 bytes, &4000 - &4019, 0 error(s)

$ z80tool dis hello.bin --org 0x4000 -c examples/hello_cpc.comments.txt --no-bytes
; Z80 disassembly  &4000 - &4019
4000     LD HL,&400C                ; entry: print the message below
4003  -> LD A,(HL)                  ; fetch the next character  <- 400A
4004     OR A
4005     RET Z
4006     CALL &BB5A                 ; -> firmware: TXT OUTPUT
4009     INC HL
400A     JR &4003                   ; -> fetch the next character

400C     DB &48,&65,&6C,&6C,&6F,&2C,&20,&5A ; |Hello, Z|
4014     DB &38,&30,&21,&0D,&0A,&00 ; |80!...|
```

Mit `--asm` entsteht Quelltext, der sich wieder zum identischen Binary assemblieren lässt:

```console
$ z80tool dis hello.bin --org 0x4000 --asm -o re.asm && z80tool asm re.asm -o re.bin && cmp hello.bin re.bin
```

## Dokumentation

| Dokument                                                | Inhalt                                              |
|---------------------------------------------------------|-----------------------------------------------------|
| [docs/usage.md](docs/usage.md)                          | Kommandozeile, alle Optionen, Rezepte               |
| [docs/assembler-syntax.md](docs/assembler-syntax.md)    | Quelltextformat, Ausdrücke, Pseudobefehle           |
| [docs/formats.md](docs/formats.md)                      | Kommentardatei, `.sna`, Intel HEX, Ausgabeformate   |
| [docs/reverse-engineering.md](docs/reverse-engineering.md) | Programme mit Ollama/Qwen3-VL kommentieren       |
| [docs/monitor.md](docs/monitor.md)                      | Monitor: Befehle, Zahlenschreibweise, Beispiel     |
| [docs/emulator.md](docs/emulator.md)                    | Z80-Emulator: API, Genauigkeit, Zustand speichern  |
| [docs/agent-protocol.md](docs/agent-protocol.md)        | JSON-Protokoll für LLM-Agenten (`z80tool serve`)   |
| [docs/architecture.md](docs/architecture.md)            | Aufbau des Codes, Algorithmen, Erweiterungspunkte   |
| [docs/migration-from-basic.md](docs/migration-from-basic.md) | Was aus den BASIC-Programmen wurde; behobene Fehler |
| [docs/testing.md](docs/testing.md)                      | Teststrategie und Kreuzprüfung                      |
| [docs/roadmap.md](docs/roadmap.md)                      | Weg zum Maschinensprache-Monitor                    |
| [CONTRIBUTING.md](CONTRIBUTING.md)                      | Bauen, Konventionen                                 |

## Projektstruktur

```
include/z80/   öffentliche Header der Bibliothek (z80core)
src/           Implementierung
app/main.cpp   Kommandozeilenwerkzeug z80tool
agent/         Orchestrator für Ollama (Python, nur Standardbibliothek)
tests/         Unit- und Property-Tests (CTest), tests/agent: Orchestrator gegen einen Mock-Ollama
tools/         Entwicklerwerkzeuge (Kreuzprüfung gegen z80asm, ZEXDOC/ZEXALL-Runner)
examples/      Beispielprogramm und Kommentardatei
docs/          Dokumentation
```

## Grenzen (Version 0.1)

- Nur **dokumentierte** Befehle. Undokumentierte Opcodes (`SLL`, `IXH`/`IXL`, `IN F,(C)`, …) werden
  vom Disassembler als `DB` ausgegeben und vom Assembler nicht akzeptiert.
- Keine Makros, bedingte Assemblierung oder `INCLUDE`.
- Der Orchestrator wurde noch nicht mit einem echten Qwen3-VL ausprobiert (nur gegen einen Mock-Server);
  Prompt und Parameter brauchen vermutlich Feinschliff.
- Der Emulator rechnet pro Befehl, nicht pro Bus-Zyklus (keine Wait-States/Contention); nur ein
  Speicher-Layout (flache 64 KiB, keine Bank-Umschaltung).
- Der Monitor hat keine eigene Zeilenbearbeitung (Tipp: `rlwrap z80tool monitor`) und assembliert
  zeilenweise ohne Vorwärtsreferenzen.

## Lizenz

[MIT](LICENSE)
