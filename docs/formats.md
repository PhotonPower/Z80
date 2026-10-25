# Dateiformate

## Kommentardatei (Disassembler, `-c`)

Textdatei, eine Zeile pro Kommentar:

```
; Zeilen mit Semikolon und Leerzeilen werden ignoriert
&5CF,"Einsprung: Hauptprogramm"
1B32,"Text ausgeben (String folgt direkt dahinter)"
0x4010,Kommentar ohne Anführungszeichen geht auch
```

- Die Adresse darf dezimal, `&hex`, `$hex`, `0xhex` oder `…h` geschrieben werden (dieselben
  Schreibweisen wie im Assembler). **Achtung:** Eine Adresse wie `1B32` ohne Präfix ist *dezimal*
  und daher ungültig; schreibe `&1B32`.
- Der Text steht in Anführungszeichen (`""` ergibt ein Anführungszeichen) oder bis Zeilenende.
- Mehrere Kommentare zur selben Adresse werden mit ` / ` zusammengefügt.
- Der Kommentar erscheint an der Zieladresse und – als `; -> …` – hinter jedem Sprung, der dorthin führt.
- Standardmäßig sind Kommentaradressen zusätzliche **Einstiegspunkte** der Ablaufanalyse
  (so verhielt sich auch das BASIC-Original). Adressen außerhalb des geladenen Bereichs
  einer Raw-Datei werden dabei ignoriert.
- Fehlerhafte Zeilen erzeugen eine Warnung auf `stderr`, der Rest wird trotzdem gelesen.

Das BASIC-Original las dieselbe Form (`input #`: Zahl, Komma, Text); Dateien in diesem Format
sollten weiter funktionieren.

## Amstrad-CPC-Snapshot (`.sna`)

- Erkennung über die Kennung `MV - SNA` am Dateianfang.
- Die 0x100 Byte große Kopfzeile wird übersprungen; es folgt das RAM-Abbild. Verwendet werden die
  **ersten 64 KiB**; Snapshots mit 128 KiB (Bank-Umschaltung) werden nur in diesem ersten Teil
  gelesen bzw. gepatcht.
- Der Assembler (`asm --sna`) schreibt ausschließlich die assemblierten Bytes in diese Datei und
  lässt Kopfzeile (Register, Hardwarezustand) sowie alles andere unverändert.

## Raw-Binary

Die Datei wird 1:1 ab `--org` in den Speicher gelegt. Zum Disassemblieren gilt nur dieser Bereich
als vorhanden; erzeugt wird beim Assemblieren der Bereich von der niedrigsten bis zur höchsten
Adresse, Lücken zwischen `ORG`-Segmenten werden mit `00` gefüllt.

## Intel HEX

Datensätze Typ `00` mit höchstens 16 Datenbytes, abschließend `:00000001FF`. Jedes
`ORG`-Segment wird getrennt ausgegeben (keine Füllbytes in den Lücken). Erweiterte
Adressierung ist nicht nötig (64-KiB-Adressraum).

## Listing (Assembler, `-l`)

```
8000  3E05        4  START:  LD A,5
^^^^  ^^^^^^   ^^^^
Adr.  Bytes   Zeile  Quelltext
```

Längere Datenzeilen (`DB`, `DW`, `DS`) werden in Gruppen zu je 4 Bytes umbrochen.
`EQU`-Zeilen zeigen den Wert in der Adressspalte.

## Symboltabelle (Assembler, `-s`)

```
CR              000D  EQU
MESSAGE         400C
START           4000
```

Alphabetisch sortiert; Namen in Großbuchstaben; `EQU` markiert Konstanten (im Unterschied zu Labels).
