# Von BASIC nach C++

Die ursprünglichen Programme sind nicht Teil des Repositorys:

- `Z80_Assembler.bas` – Assembler (FreeBASIC)
- `z80_dissass.bas` – Disassembler (FreeBASIC, „für Caprice Snapshot files .sna“)

## Zuordnung

| BASIC                                               | C++                                                     |
|-----------------------------------------------------|---------------------------------------------------------|
| globale Variablen, `GOTO`-Labels (`L10130` …)        | Klassen mit Zustand in Instanzen, keine Globals          |
| Textmuster `teadr_`, `teed_`, `teb1_` + `wb1()`/`wed()`| `Encoder` (kodiert nach Befehlsfamilie)                  |
| `label_test`, `zahl_test`, `werter`, `werter_low`    | `ExpressionEvaluator` + Symboltabelle                    |
| `offset_berechnen`                                   | relative Sprünge im `Encoder` (`relative()`)             |
| `r_test`, `rps_test`, `cond_test`, `L11240`          | Operandenklassifikation (`parseOperand`)                 |
| `lade_spezial_8bit`                                  | Fälle in `encodeLd()`                                    |
| Tabellen `befstring`, `regtabstring`, … (`DATA`)     | algorithmischer Dekoder (`decoder.cpp`)                  |
| `getbyte`, `getasmline`, `interpreter`, `codezerleg` | `decode()`                                               |
| `memorymap`, `memorymap2`, `memorymap3`              | `callers_`, `covered_`/`isStart_`, `jumpers_`            |
| verkettete Liste `caller`/`ext1`/`ext2`              | `std::map<Word, std::set<Word>>`                         |
| `QuickSort`, `finditem`, `swap_integer`              | sortierte Standardcontainer                              |
| feste Dateinamen (`sorcery.sna`, `out.txt`, …)       | Kommandozeilenparameter                                  |
| `INPUT "Aufzeichnung (j/n)"`                         | Optionen `-o`, `--hex`, `--sna`                          |
| Kettenmodus bei Endadresse `0`                       | Ablaufanalyse, wenn kein `-e` angegeben ist              |
| `sorcery`-Abfrage (`CALL &1B32`)                     | allgemein: `--inline-string <Adresse>`                   |

## Verhaltensänderungen

| Thema | BASIC | C++ |
|---|---|---|
| Labels | höchstens 6 Zeichen (längere wurden mit Meldung gekürzt) | beliebig lang |
| Ausdrücke | Zahl oder Label | volle Ausdrücke (`msg+2`, `1<<4`, `HIGH(x)`) |
| Zahlen | `&hex` und dezimal | zusätzlich `$1F`, `0x1F`, `1Fh`, `%1010`, `'A'` |
| Fehler | meldet Fehler, bot das Speichern trotzdem an | keine Binär-/HEX-/Snapshot-Ausgabe bei Fehlern |
| `DS n` | reservierte Platz (Adresse weiter), schrieb nichts | schreibt `n` Bytes (Füllwert optional) |
| Startadresse | fest `&A000` | `0` bzw. `--org`/`ORG` |
| Snapshot schreiben | schrieb den gesamten Speicher zurück | nur die assemblierten Bytes (Ergebnis gleich) |
| Hexdump (48 Zeichen/Zeile, `*` für `&C9`) | Daten als ASCII-Streifen | `DB`-Zeilen (8 Bytes) mit ASCII-Spalte, `DS` für lange Läufe |
| `ED`-Aliase (z. B. `ED 63`) | als Befehl angezeigt | `DB` (nur kanonische Formen) |
| Kommentar-Ausgabe | `ADDR::Text` | Kommentar hinter dem Befehl (`; Text`) |
| `RETI`/`RETN` im Kettenmodus | beendeten den Weg nicht | beenden den Weg |
| Aufruferliste (`<-------`) | nur für `CALL`- und bedingte `JP`/`JR`-Ziele | auch für unbedingte `JP`/`JR`-Ziele und `DJNZ` |
| Inline-String nach `CALL` | Schleife erhöhte `pc`, **bevor** das erste Byte geprüft wurde | prüft ab dem ersten Byte direkt hinter dem `CALL` |

## Behobene Fehler

Beim Lesen der BASIC-Quellen aufgefallen und in der C++-Version nicht mehr vorhanden
(jeweils Zeilenangaben der Originaldateien):

1. **`INC L`/`DEC L` falsch** (`z80_dissass.bas`, Zeile 749): Die `DATA`-Tabelle enthält für die
   Opcodes `&2C`/`&2D` die Texte `INC H`/`DEC H` statt `INC L`/`DEC L`.
2. **`RST &08` falsch** (`z80_dissass.bas`, Zeile 759): Opcode `&CF` wird als `RST &0B`
   ausgegeben.
3. **Falsche Reichweitenprüfung bei `JR`/`DJNZ`** (`Z80_Assembler.bas`, Zeile 968): Die Prüfung
   `of>129 OR of<-126` lässt Verschiebungen `+128`/`+129` zu (das Ergebnis springt
   an eine völlig andere Stelle) und weist gültige Verschiebungen `-127`/`-128` zurück.
   Richtig ist `-128…+127`; getestet in `tests/test_assembler.cpp`.
4. **`RETI`/`RETN` beendeten den Kettenmodus nicht** (`z80_dissass.bas`, Zeile 178): Die Abfrage
   prüft, ob der Befehlstext auf `RET` *endet*; `RETI`/`RETN` enden aber auf `ETI`/`ETN`.

Fehlerklassen wie 1 und 2 sind in der neuen Version konstruktionsbedingt ausgeschlossen: Der Dekoder
leitet Opcode-Namen aus dem Bitschema ab, und der Test `test_roundtrip` prüft **jeden** Opcode
gegen den Assembler und stichprobenhaft gegen `z80asm`.

## Noch nicht übernommen

- Die auskommentierten Reste im Assembler (Aufzeichnung als `.obj` über `SAVE`, BASIC-Zeilenlisting
  mit `laze`/`zenr`/`bpc`) – sie hängen am CPC-BASIC und haben in der neuen Struktur keine Entsprechung.
- Undokumentierte Befehle: Das Original kannte `SLL` ebenfalls nicht (Platzhalter `***`).
