# Roadmap: Weg zum Maschinensprache-Monitor

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion.

## Wo wir stehen

Assembler, Disassembler und **Emulator** als Bibliothek plus Kommandozeile, vollständig getestet
([testing.md](testing.md)). Der Emulator ([emulator.md](emulator.md)) besteht ZEXDOC/ZEXALL und bietet
Einzelschritt, Breakpoints, Watchpoints, Lauf bis Rückkehr, Checkpoints und Snapshot-Dateien;
`z80tool serve` macht ihn für Agenten nutzbar ([agent-protocol.md](agent-protocol.md)). Die Bibliothek enthält bereits die Bausteine, die ein Monitor braucht
([architecture.md](architecture.md#erweiterungspunkte-für-den-monitor)): zustandsloses
`decode()`, einzeiliges `encodeInstruction()`, `Symbolizer`, `Memory`.

## Meilenstein 0.2 – Monitor ohne CPU

Interaktive Schleife (`z80tool monitor [image]`) mit den klassischen Befehlen, die nur Speicher
und Bibliothek brauchen:

| Befehl (Vorschlag) | Funktion |
|--------------------|----------|
| `d [adr [n]]`      | disassemblieren (Fortsetzung mit Enter) |
| `m [adr [n]]`      | Speicher hexadezimal + ASCII anzeigen |
| `e adr bytes…`     | Bytes schreiben |
| `a adr`            | Zeile für Zeile assemblieren und ablegen |
| `f von bis wert`   | füllen |
| `c von bis adr`    | vergleichen |
| `t von bis adr`    | kopieren (Überlappung korrekt) |
| `h von bis bytes…` | Bytes/Text suchen |
| `l datei [adr]`, `s datei von bis` | laden / speichern (Raw, `.sna`, Intel HEX) |
| `sym`, `label name adr` | Symbole verwalten; Dekoder zeigt sie über den `Symbolizer` |
| `x`                | beenden |

Dafür nötig:
- API „eine Zeile an Adresse assemblieren“ (`encodeInstruction` ist vorhanden, es fehlt der
  Komfortaufruf mit Symboltabelle);
- „disassembliere n Befehle ab Adresse“ ohne Gesamtanalyse;
- Symboltabelle laden/speichern (das `.sym`-Format der Assembler-Ausgabe lesen);
- Zeileneingabe mit Verlauf (z. B. `linenoise`/`readline` oder eigene Minimalversion) und ein
  Kommandoparser mit Tests.

## Meilenstein 0.3 – Emulator (weitgehend erledigt)

Erledigt: Interpreter mit allen dokumentierten und den verbreiteten undokumentierten Befehlen,
Flags, Zyklen pro Befehl, Interrupts; Einzelschritt, Breakpoints, Watchpoints, Lauf bis Rückkehr,
Ablaufprotokoll; Port-Hooks; Test gegen ZEXDOC/ZEXALL; Zustand speichern/laden; Agenten-Protokoll.

Offen:
- Speicher mit Banking/ROM-Überlagerung und Speicher-Hooks (aktuell flache 64 KiB);
- Bus-genaues Timing (Wait-States, Contention) für maschinenspezifisches Verhalten;
- Geräte-Modelle (z. B. CPC: Gate Array, CRTC, PPI; Spectrum: ULA) und Firmware-Aufrufe als
  Hooks (`CALL &BB5A` → Textausgabe) – nötig, damit echte Programme ohne ROM laufen;
- Monitor-Befehle `r`, `n`, `g`, `b`, `t` im interaktiven Frontend auf Basis der vorhandenen API.

## Danach (Ideen)

- Undokumentierte Befehle (`SLL`, `IXH`/`IXL`, `IN F,(C)`) – optional per Schalter.
- Bessere Analyse: Sprungtabellen, Datentyperkennung, Funktionsgrenzen.
- Projektdatei (Kommentare, Symbole, Inline-Daten-Hooks, Bereiche) statt vieler Kommandozeilenoptionen.
- Makros, `INCLUDE`, bedingte Assemblierung im Assembler.
- Zielsystem-Profile (Amstrad CPC: Firmware-Symbole wie `&BB5A`, Bank-Umschaltung bei 128-KiB-Snapshots).

## Offene Fragen an dich

1. **Zielplattform:** nur Amstrad CPC (`.sna`) oder allgemein Z80 (ZX Spectrum, MSX, CP/M, …)?
2. **Bedienung:** reine Textkonsole im Stil klassischer Monitore, eine TUI (z. B. mit Registern und
   Disassembly gleichzeitig) oder später eine grafische Oberfläche?
3. **Ausführen:** ~~eigener Emulator oder Anbindung an einen vorhandenen~~ – entschieden: eigener
   Emulator (`z80::Cpu`). Offen ist, welche Geräte/Firmware nachgebildet werden sollen.
4. **Befehlsstil:** eigener Befehlssatz (wie oben vorgeschlagen) oder Anlehnung an einen bestimmten
   historischen Monitor (z. B. Zeus, Devpac-Monitor, MAXAM)?
