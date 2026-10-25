# Roadmap: Weg zum Maschinensprache-Monitor

> Dieses Dokument beschreibt **Pläne**, keine fertigen Funktionen. Reihenfolge und Umfang sind
> Vorschläge zur Diskussion.

## Wo wir stehen (0.1)

Assembler und Disassembler als Bibliothek plus Kommandozeile, vollständig getestet
([testing.md](testing.md)). Die Bibliothek enthält bereits die Bausteine, die ein Monitor braucht
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

## Meilenstein 0.3 – CPU-Kern und Ausführen

- Z80-Interpreter (`z80emu`, eigene Bibliothek, nutzt `Memory`): alle dokumentierten Befehle,
  Flags, Zyklen, Interrupts.
- Registeranzeige und -änderung (`r`), Einzelschritt (`n`), Ablauf bis Haltepunkt (`g`),
  Haltepunkte (`b`), Ablaufprotokoll (`t`).
- Ports und Speicher als Hooks (Geräte-Anbindung), Banking.
- Test des Kerns gegen bekannte Befehlstestprogramme (z. B. ZEXDOC/ZEXALL).

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
3. **Ausführen:** eigener Z80-Emulator im Projekt (Meilenstein 0.3) oder Anbindung an einen
   vorhandenen Emulator (z. B. über dessen Debug-Schnittstelle)?
4. **Befehlsstil:** eigener Befehlssatz (wie oben vorgeschlagen) oder Anlehnung an einen bestimmten
   historischen Monitor (z. B. Zeus, Devpac-Monitor, MAXAM)?
