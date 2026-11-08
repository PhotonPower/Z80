# Anleitung: vom Programm zum kommentierten Listing

Diese Anleitung führt Schritt für Schritt durch die ganze Werkzeugkette – mit dem mitgelieferten Beispielprogramm
`examples/cpc/demo_cpc.asm`, danach mit einem eigenen CPC-Programm. Die Schritte 1–4 brauchen **kein Modell** und laufen vollständig
lokal; ab Schritt 5 wird ein Ollama-Server gebraucht. Der Orchestrator ist bisher nur gegen einen Mock getestet
([reverse-engineering.md](reverse-engineering.md)): Prompt und Parameter brauchen nach dem ersten echten Lauf vermutlich Feinschliff.

## 1. Bauen und prüfen

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Voraussetzungen: ein C++20-Compiler und CMake; für die Python-Teile Python ≥ 3.9 (nur Standardbibliothek). Der Orchestrator sucht
`build/z80tool` selbst, sonst `--z80tool <Pfad>`.

## 2. Das Beispiel übersetzen und starten

```sh
build/z80tool asm examples/cpc/demo_cpc.asm -o demo_cpc.bin                        # ein rohes Binary: so sieht ein fremdes Programm aus
build/z80tool asm examples/cpc/demo_cpc.asm --amsdos DEMO.BIN --exec 0x4000         # dasselbe mit AMSDOS-Kopf (wie von einer CPC-Diskette)
```

Das Programm gibt über die Firmware (`TXT_OUTPUT`) Text aus. Ohne ROM fängt der Emulator die Firmware-Aufrufe ab und sammelt den Text:

```sh
build/z80tool serve --allow-files
{"cmd":"machine","type":"cpc"}
{"cmd":"load_file","file":"DEMO.BIN"}
{"cmd":"call","addr":"0x4000"}
{"cmd":"console"}
```

`load_file` liest den AMSDOS-Kopf: Ladeadresse &4000, PC auf den Einsprung. `console` liefert
`"Z80 demo\nHello from the Z80 example\nsum=150\none two three\n"`.

## 3. Die Analyse ansehen

```json
{"cmd":"analyze","entries":["0x4000"]}
{"cmd":"jump_tables"}
{"cmd":"data_regions"}
{"cmd":"routines"}
```

Erwartet: **eine Sprungtabelle** (`pointers` bei &4032 mit drei Einträgen, gefunden über `LD HL,commands` … `JP (HL)`), **sieben Routinen**
(`main`, `print`, `dispatch`, `print_dec` und die drei Fälle der Tabelle mit `via_table`) und als Daten sechs Strings, eine Tabelle von drei
String-Zeigern, zwei Zahlentabellen (die Werte, die `cmd_sum` addiert, und die Zehnerpotenzen von `print_dec`) und die 16-Bit-Variable `runs`.
Die Tail-Calls (`JP print` am Ende von `cmd_hello` und `cmd_sum`) zählen bei `print` als Aufrufer.

## 4. Das Listing ohne Modell

```json
{"cmd":"data_regions","apply":true}
{"cmd":"analyze","entries":["0x4000"]}
{"cmd":"listing"}
```

Das Listing ist schon lesbar, bevor ein Modell etwas getan hat: Strings als `DB "Hello from the Z80 example",&0D,&00`, die Sprungtabelle mit
Kommentar und `DW sub_4038,sub_403E,sub_4062`, die Zeigertabelle als `DW data_40D8,data_40DD,data_40E2`, Firmware-Aufrufe mit Namen. `verified:true`
heißt: Es wurde neu assembliert und ergibt Byte für Byte dasselbe Programm. Dasselbe in einem Schritt, mit Bearbeitungsreihenfolge und
Token-Schätzung:

```sh
python3 agent/z80_annotate.py --bin DEMO.BIN --dry-run
```

## 5. Ollama vorbereiten

```sh
ollama pull qwen3-vl:8b-instruct      # schnell; für einen zweiten, gründlichen Durchgang z. B. qwen3-vl:8b
ollama list
```

Der Orchestrator fragt Ollama, was ein Modell kann (Tools, Bilder, Thinking), und lehnt eines ohne Tool-Unterstützung sofort ab.

## 6. Der erste Lauf

```sh
python3 agent/z80_annotate.py --bin DEMO.BIN --project demo.proj.json --out demo_kommentiert.asm \
        --report demo_luecken.md --log demo.log.jsonl --lang Deutsch
```

Ablauf: Datenbereiche und Tabellen werden automatisch eingetragen, dann bekommt das Modell Routine für Routine (zuerst die Blätter) einen Auftrag mit
dem Listing, den Aufgerufenen und dem, worauf die Operanden zeigen. Es testet mit `run_routine`, benennt und kommentiert und ruft `finish_routine`.
Nach jeder Routine wird `demo.proj.json` gespeichert; ein Abbruch verliert nichts. Am Ende steht das verifizierte Listing in `demo_kommentiert.asm`,
was offen blieb, in `demo_luecken.md`.

## 6b. Ohne Ollama: ein Agent oder ein Mensch übernimmt die Rolle des Modells

`agent/z80_drive.py` bietet dieselben Werkzeuge, aber einen Schritt je Aufruf:

```sh
python3 agent/z80_drive.py --work arbeit init --bin DEMO.BIN
python3 agent/z80_drive.py --work arbeit status
python3 agent/z80_drive.py --work arbeit show 0x401C
python3 agent/z80_drive.py --work arbeit tool run_routine '{"addr":"0x401C","registers":{"HL":"0x5000"},"memory":[{"addr":"0x5000","text":"Hi"}]}'
python3 agent/z80_drive.py --work arbeit tool document_routine '{"addr":"0x401C","name":"print_string","summary":"Prints the zero-terminated text at HL.","inputs":"HL = text","outputs":"HL = the terminator","clobbers":"AF","confidence":"high"}'
python3 agent/z80_drive.py --work arbeit tool add_comment '{"addr":"0x401D","text":"a zero ends the text"}'
python3 agent/z80_drive.py --work arbeit tool add_comment '{"addr":"0x401F","text":"one character to the screen"}'
python3 agent/z80_drive.py --work arbeit finish 0x401C
python3 agent/z80_drive.py --work arbeit listing --out demo.asm
```

`finish` ist streng: Es nimmt eine Routine erst an, wenn nichts mehr fehlt (Namen, Kommentare, `inputs`/`outputs`/`clobbers`/`confidence`), und
sagt sonst, was fehlt. `--accept-gaps` wendet die Regel des Orchestrators an (der zweite Aufruf ist endgültig).

## 7. Ein eigenes Programm

- **Von einer Diskette:** `build/z80tool dsk ls spiel.dsk` zeigt die Dateien mit Lade- und Einsprungadresse; dann
  `python3 agent/z80_annotate.py --dsk spiel.dsk --dsk-file SPIEL.BIN --dry-run`.
- **Aus einem Snapshot** eines laufenden Programms: `--sna spiel.sna` (PC des Snapshots ist der Einsprung).
- **Gepackt?** Wenn die Analyse nur wenige hundert Byte Code findet und der Rest wie Zufall aussieht, ist das Programm vermutlich gepackt:
  [unpacking.md](unpacking.md).

## 8. Woran man ein Problem erkennt

| Beobachtung | Ursache und Abhilfe |
|---|---|
| Die Analyse findet kaum Code | falsche Ladeadresse (`--org`), falscher Einsprung (`--entry`), oder ein gepacktes Programm |
| Viele Routinen ohne Aufrufer | Code hinter berechneten Sprüngen, die die Analyse nicht kennt: `jump_tables` ansehen, Tabellen als `pointers` markieren |
| `run_routine` meldet `EMPTY memory` | die Routine braucht Zustand, den anderer Code herstellt: `setup` mit dem Programmeinsprung |
| `WARNING: the largest routine …` im Dry-Run | `--num-ctx` erhöhen (wenn das Modell es kann) |
| Das Listing ist `NOT verified` | ein Fehler im Werkzeug, nicht im Modell: bitte mit dem Projekt melden |
