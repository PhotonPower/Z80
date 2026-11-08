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

## CPC-Disk-Abbild (`.dsk`)

- **Standard** („MV - CPCEMU Disk-File“): 256 Byte Kopf (Spuren bei &30, Seiten bei &31, Spurgröße bei &32), danach je Spur ein
  „Track-Info“-Block von 256 Byte (Spur, Seite, Sektorgröße N, Zahl der Sektoren, je Sektor C/H/R/N/ST1/ST2) und die Sektordaten in der
  gespeicherten Reihenfolge, jeder Sektor 128·2^N Byte.
- **Extended** („EXTENDED CPC DSK File“): die Größe jeder Spur steht in einer Tabelle ab &34 (×256, 0 = unformatiert), die tatsächliche Länge
  jedes Sektors im Sektoreintrag (Bytes 6–7); damit lassen sich auch kopiergeschützte Disketten abbilden. Abbilder, die hinter der letzten
  benutzten Spur abgeschnitten sind, werden gelesen.
- **AMSDOS-Dateisystem** (CP/M 2.2): das Format erkennt die kleinste Sektor-ID der Spur 0: **DATA** &C1 (keine reservierten Spuren),
  **SYSTEM/VENDOR** &41 (zwei reservierte Spuren, Bootsektor Spur 0/&41), **IBM** &01 (eine reservierte Spur, 8 Sektoren je Spur). Blöcke von
  1 KiB (zwei Sektoren zu 512 Byte), das Verzeichnis belegt die ersten beiden Blöcke (64 Einträge zu 32 Byte: Benutzer, Name 8+3 mit Attribut-Bits
  „schreibgeschützt“ und „System“, Extent, Zahl der 128-Byte-Records, 16 Blocknummern). Eine Datei über 16 KiB hat mehrere Einträge
  (Extents), gelöschte Einträge haben den Benutzer &E5.

## AMSDOS-Kopf

Dateien, die AMSDOS mit `SAVE` schreibt (Binärdateien, geschütztes BASIC), beginnen mit einem 128-Byte-Kopf: Benutzer (0), Name (1–11),
Dateityp (18; 2 = binär), Ladeadresse (21–22), Länge (24–25), Einsprung (26–27), 24-Bit-Länge (64–66) und die **Prüfsumme** über die Bytes
0–66 (67–68, 16 Bit). Erkannt wird der Kopf nur an der Prüfsumme (128 Nullbytes zählen nicht). `asm --amsdos` schreibt ihn, `dsk extract`
entfernt ihn, `dis`, `monitor` und `load_file` legen die Datei an die Ladeadresse.

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
