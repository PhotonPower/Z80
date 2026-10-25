# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt
- **Z80-Emulator** (`z80::Cpu`): alle dokumentierten und die verbreiteten undokumentierten Befehle,
  Flags 3/5 und `MEMPTR`, T-States pro Befehl, Interrupts (IM 0/1/2, NMI), Port-Handler.
  Besteht ZEXDOC und ZEXALL (67/67).
- Debugger-Funktionen: Einzelschritt, `run`/`runUntil`/`runUntilReturn`/`stepOver`, Breakpoints,
  Watchpoints, Befehls- und Zyklenlimits, Trace-Hook.
- Zustand: `CpuState`, `Snapshot` (Checkpoints im Speicher, Snapshot-Datei `Z80EMUS1`), CPU-Zustand
  als JSON, Registerzugriff per Name.
- **`z80tool serve`**: JSON-Zeilenprotokoll für LLM-Agenten (Assemblieren, Ausführen, `call`, `diff`,
  Checkpoints, Ein-/Ausgabe, …); Dateizugriff nur mit `--allow-files`.
- `tools/zex_runner.cpp` und CI-Job für ZEXDOC/ZEXALL; Tests `cpu`, `session`.

## [0.1.0]

Erste C++-Version: Portierung der beiden BASIC-Programme (Z80-Assembler, Z80-Disassembler)
in ein gemeinsames Projekt.

### Hinzugefügt
- **Assembler** (`z80::Assembler`): Mehrpass-Auflösung von Labels und Vorwärtsreferenzen,
  Ausdrücke (`+ - * / % << >> & | ^ ~`, `$`, `HIGH()`, `LOW()`), alle dokumentierten Z80-Befehle,
  Pseudobefehle `ORG EQU END DB DW DM DS`, mehrere `ORG`-Segmente, Listing und Symboltabelle.
- **Disassembler** (`z80::Disassembler`): Dekoder für alle dokumentierten Befehle, Ablaufanalyse
  („Kettenmodus“) und linearer Durchlauf, Querverweise (Aufrufer von Sprung-/Callzielen),
  Kommentardatei, Inline-Daten nach CALL (z. B. Strings), Ausgabe als Listing oder als
  wieder assemblierbarer Quelltext.
- **Kommandozeilenwerkzeug** `z80tool` mit den Unterbefehlen `asm` und `dis`; Ein-/Ausgabe von
  Raw-Binaries, CPC-`.sna`-Snapshots und Intel HEX.
- Tests (7 Suiten, u. a. exhaustiver Dekoder↔Assembler-Roundtrip und Property-Test
  `assemble(disassemble(x)) == x`), CI (Linux, macOS, Windows, Sanitizer, Kreuzprüfung gegen
  `z80asm`), Dokumentation in `docs/`.

### Behoben (gegenüber den BASIC-Originalen)
Siehe [docs/migration-from-basic.md](docs/migration-from-basic.md).
