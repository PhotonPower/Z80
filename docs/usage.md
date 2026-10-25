# Benutzung von `z80tool`

```
z80tool asm <source.asm> [Optionen]
z80tool dis <image>      [Optionen]
z80tool help | version
```

Rückgabewerte: `0` Erfolg, `1` Assemblierfehler im Quelltext, `2` falsche Aufrufparameter oder
Ein-/Ausgabefehler.

Adressen und Zahlen dürfen in allen Schreibweisen des Assemblers angegeben werden:
`4096`, `&1000`, `$1000`, `0x1000`, `1000h`.

## `asm` – Assembler

| Option            | Bedeutung                                                                     |
|-------------------|-------------------------------------------------------------------------------|
| `-o <Datei>`      | Raw-Binary schreiben (von der niedrigsten bis zur höchsten Adresse; Lücken zwischen mehreren `ORG`-Segmenten werden mit `00` gefüllt) |
| `--hex <Datei>`   | Intel HEX schreiben (nur die tatsächlich assemblierten Segmente)               |
| `--sna <Datei>`   | Code direkt in das RAM-Abbild eines CPC-Snapshots schreiben (Datei wird verändert) |
| `-l <Datei\|->`   | Listing schreiben (`-` = Standardausgabe)                                      |
| `-s <Datei\|->`   | Symboltabelle schreiben                                                        |
| `--org <Adresse>` | Startadresse vor dem ersten `ORG` (Standard `0`)                               |

Ohne `-o`, `--hex` und `--sna` entsteht `<quelle>.bin`. Bei Fehlern im Quelltext werden **keine**
Binär-, HEX- oder Snapshot-Dateien geschrieben (Listing und Symboltabelle schon, das hilft bei der
Fehlersuche). Alle Fehler eines Laufs werden gesammelt auf `stderr` ausgegeben:

```
prog.asm:12: error: undefined symbol 'LOPP'
    JR NZ,lopp
```

Beispiele:

```sh
z80tool asm prog.asm -o prog.bin -l prog.lst -s prog.sym
z80tool asm prog.asm --sna spiel.sna            # direkt in einen Snapshot patchen
z80tool asm prog.asm --hex prog.hex
```

## `dis` – Disassembler

| Option                    | Bedeutung                                                                 |
|---------------------------|---------------------------------------------------------------------------|
| `--format auto\|sna\|bin` | Eingabeformat; `auto` erkennt `.sna` am Header `MV - SNA`, sonst Raw       |
| `--org <Adresse>`         | Ladeadresse eines Raw-Binaries (Standard `0`)                              |
| `-s <Adresse>`            | Startadresse bzw. Einstiegspunkt (Standard: Ladeadresse, bei `.sna` `0`)    |
| `-e <Adresse>`            | Endadresse → **linearer Durchlauf** von `-s` bis `-e`                      |
| *(ohne `-e`)*             | **Ablaufanalyse** ab `-s`: `CALL`/`JP`/`JR`/`DJNZ`-Ziele werden verfolgt    |
| `-c <Datei>`              | Kommentardatei ([docs/formats.md](formats.md))                            |
| `--entry <Adresse>`       | zusätzlicher Einstiegspunkt (mehrfach möglich)                             |
| `--inline-string <Adr>`   | `CALL <Adr>` wird von einem String gefolgt, dessen letztes Zeichen Bit 7 gesetzt hat |
| `--inline-zstring <Adr>`  | wie oben, aber nullterminiert                                              |
| `--asm`                   | wieder assemblierbaren Quelltext statt Listing ausgeben                    |
| `--style amp\|dollar\|c\|h` | Zahlenschreibweise `&1F`, `$1F`, `0x1F`, `1FH` (Standard `amp`)           |
| `--from`, `--to <Adr>`    | Ausgabebereich einschränken                                                |
| `--no-bytes`, `--no-xrefs`| Bytespalte bzw. Aufruferlisten weglassen                                   |
| `-o <Datei>`              | in eine Datei statt auf die Standardausgabe schreiben                      |

### Ablaufanalyse („Kettenmodus“)

Ohne `-e` folgt der Disassembler dem Programmablauf. Das trennt Code von Daten, die mitten im
Programm liegen. Regeln:

- `CALL`-Ziele, bedingte `JP`-Ziele und bedingte `JR`/`DJNZ`-Ziele werden vorgemerkt und später
  abgearbeitet (in dieser Reihenfolge der Priorität).
- Ein unbedingter `JP`/`JR` setzt den Weg am Sprungziel fort.
- Ein Weg endet bei `RET`, `RETI`, `RETN`, `RST`, `JP (HL)`/`JP (IX)`/`JP (IY)` und sobald bereits
  analysierter Code erreicht wird. (`RST` beendet den Weg, weil dahinter häufig Inline-Daten stehen.)
  `HALT` beendet den Weg **nicht**.
- Adressen aus der Kommentardatei und `--entry` sind zusätzliche Einstiegspunkte.
- Eine Folge von mindestens 16 `00`-Bytes beendet den Weg (nicht initialisierter Speicher würde
  sonst als lange `NOP`-Kette „disassembliert“).
- Bei Raw-Binaries wird nur im tatsächlich geladenen Bereich analysiert; Sprünge nach außen werden
  als Querverweis vermerkt, dort aber nichts dekodiert.

### Ausgabe

Listing:

```
4003  -> LD A,(HL)                  ; fetch the next character  <- 400A
^^^^  ^^ ^^^^^^^^^^^^               ^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^
Adr.  Ziel eines CALL/JP/JR         Kommentar                   Aufrufer
```

- `->` markiert Adressen, die Ziel eines `CALL`/`JP`/`JR`/`DJNZ` sind; `<-` listet die Aufrufer.
- Steht an einem Sprungziel ein Kommentar, wird er hinter dem Sprungbefehl wiederholt (`; -> …`).
- Nach `RET` und unbedingten Sprüngen folgt eine Leerzeile.
- Nicht als Code erkannte Bytes erscheinen als `DB`-Zeilen mit ASCII-Spalte; Folgen von
  mindestens 16 gleichen Bytes werden zu `DS n,wert` zusammengefasst.
- Am Ende steht eine Zusammenfassung (Befehle, Aufrufziele mit Häufigkeit).

Mit `--asm` erscheinen statt Adressen echte Labels (`L_4003`) und Kopfzeilen mit `ORG`/`EQU`.

### Rezepte

```sh
# Programm ab Einsprungadresse analysieren (CPC-Snapshot)
z80tool dis spiel.sna -s 0x05CF -c spiel.comments.txt -o spiel.lst

# Bereich linear zerlegen, ohne dem Ablauf zu folgen
z80tool dis spiel.sna -s 0x1000 -e 0x1FFF

# Routine, die ihren Text direkt hinter dem CALL erwartet (letztes Zeichen mit Bit 7)
z80tool dis spiel.sna -s 0x05CF --inline-string 0x1B32

# Disassemblieren und neu assemblieren ergibt dasselbe Binary
z80tool dis prog.bin --org 0x4000 --asm -o prog.asm
z80tool asm prog.asm -o prog2.bin && cmp prog.bin prog2.bin
```
