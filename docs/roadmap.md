# Roadmap: Weg zum Maschinensprache-Monitor

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion.

## Wo wir stehen

Assembler, Disassembler, **Emulator** und **Monitor** als Bibliothek plus Kommandozeile, vollständig getestet
([testing.md](testing.md)). Der Emulator ([emulator.md](emulator.md)) besteht ZEXDOC/ZEXALL und bietet
Einzelschritt, Breakpoints, Watchpoints, Lauf bis Rückkehr, Checkpoints und Snapshot-Dateien;
`z80tool serve` macht ihn für Agenten nutzbar ([agent-protocol.md](agent-protocol.md)). Die Bibliothek enthält bereits die Bausteine, die ein Monitor braucht
([architecture.md](architecture.md#erweiterungspunkte-für-den-monitor)): zustandsloses
`decode()`, einzeiliges `encodeInstruction()`, `Symbolizer`, `Memory`.

## Meilenstein 0.2 – Monitor (erledigt)

`z80tool monitor` ([monitor.md](monitor.md)) bietet die klassischen Befehle: `d m e f c t h a r n o u g b w
sym asm l sv ck rb` und mehr. Der Monitor läuft auf dem Emulator, sodass Ausführen, Einzelschritt und
Haltepunkte von Anfang an dabei sind.

Offen (Ideen):
- Zeileneditor mit Verlauf im Programm (statt `rlwrap`), Tab-Vervollständigung für Symbole;
- Vorwärtsreferenzen beim zeilenweisen Assemblieren (Fixups, die bei der Definition nachgetragen werden);
- Textoberfläche (TUI) mit Registern, Disassembly und Speicher gleichzeitig;
- Befehle `x` (Ausdruck berechnen), Skript-Dateien mit Kommentaren, Symbol-Import aus `.sym` mit Gültigkeitsbereichen.

## Meilenstein 0.3 – Emulator (weitgehend erledigt)

Erledigt: Interpreter mit allen dokumentierten und den verbreiteten undokumentierten Befehlen,
Flags, Zyklen pro Befehl, Interrupts; Einzelschritt, Breakpoints, Watchpoints, Lauf bis Rückkehr,
Ablaufprotokoll; Port-Hooks; Test gegen ZEXDOC/ZEXALL; Zustand speichern/laden; Agenten-Protokoll.

Offen:
- Speicher mit Banking/ROM-Überlagerung und Speicher-Hooks (aktuell flache 64 KiB);
- Bus-genaues Timing (Wait-States, Contention) für maschinenspezifisches Verhalten;
- Geräte-Modelle (z. B. CPC: Gate Array, CRTC, PPI; Spectrum: ULA) und Firmware-Aufrufe als
  Hooks (`CALL &BB5A` → Textausgabe) – nötig, damit echte Programme ohne ROM laufen;


## Danach (Ideen)

- Undokumentierte Befehle (`SLL`, `IXH`/`IXL`, `IN F,(C)`) – optional per Schalter.
- Bessere Analyse: Sprungtabellen, Datentyperkennung, Funktionsgrenzen.
- Projektdatei (Kommentare, Symbole, Inline-Daten-Hooks, Bereiche) statt vieler Kommandozeilenoptionen.
- Makros, `INCLUDE`, bedingte Assemblierung im Assembler.
- Zielsystem-Profile (Amstrad CPC: Firmware-Symbole wie `&BB5A`, Bank-Umschaltung bei 128-KiB-Snapshots).

## Offene Fragen an dich

1. **Zielplattform:** nur Amstrad CPC (`.sna`) oder allgemein Z80 (ZX Spectrum, MSX, CP/M, …)?
2. **Bedienung:** ~~Textkonsole~~ – umgesetzt (klassischer Monitor). Soll eine TUI (Register,
   Disassembly und Speicher gleichzeitig) oder eine grafische Oberfläche folgen?
3. **Ausführen:** ~~eigener Emulator oder Anbindung an einen vorhandenen~~ – entschieden: eigener
   Emulator (`z80::Cpu`). Offen ist, welche Geräte/Firmware nachgebildet werden sollen.
4. **Befehlsstil:** eigener Befehlssatz (wie oben vorgeschlagen) oder Anlehnung an einen bestimmten
   historischen Monitor (z. B. Zeus, Devpac-Monitor, MAXAM)?
