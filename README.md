# Z80 Tools

[![CI](https://github.com/PhotonPower/Z80/actions/workflows/ci.yml/badge.svg)](https://github.com/PhotonPower/Z80/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Z80-Assembler und -Disassembler in modernem C++20 – hervorgegangen aus zwei BASIC-Programmen
(`Z80_Assembler.bas`, `z80_dissass.bas`). Geplant ist der Ausbau zu einem vollständigen
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

Beides ist getestet: u. a. ein exhaustiver Roundtrip über alle Opcode-Präfixe, ein Property-Test
(`assemble(disassemble(x)) == x` für Zufallsdaten) und eine Kreuzprüfung gegen den unabhängigen
Assembler `z80asm` ([docs/testing.md](docs/testing.md)).

## Schnellstart

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build                  # Tests

./build/z80tool asm examples/hello_cpc.asm -l -         # assemblieren, Listing auf den Bildschirm
./build/z80tool dis hello_cpc.bin --org 0x4000          # disassemblieren
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
tests/         Unit- und Property-Tests (CTest)
tools/         Entwicklerwerkzeuge (Kreuzprüfung gegen z80asm)
examples/      Beispielprogramm und Kommentardatei
docs/          Dokumentation
```

## Grenzen (Version 0.1)

- Nur **dokumentierte** Befehle. Undokumentierte Opcodes (`SLL`, `IXH`/`IXL`, `IN F,(C)`, …) werden
  vom Disassembler als `DB` ausgegeben und vom Assembler nicht akzeptiert.
- Keine Makros, bedingte Assemblierung oder `INCLUDE`.
- Der Monitor (Speicher ansehen/ändern, Einzelschritt, Haltepunkte) ist noch nicht implementiert.

## Lizenz

[MIT](LICENSE)
