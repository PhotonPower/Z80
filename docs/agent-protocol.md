# Agenten-Protokoll (`z80tool serve`)

Ein lokaler Agent (z. B. ein LLM mit Werkzeugaufruf) führt Z80-Code aus, indem er JSON-Objekte
an `z80tool serve` schickt. **Eine Zeile Anfrage → genau eine Zeile Antwort**, über stdin/stdout.

```sh
z80tool serve                  # ohne Dateizugriff (Standard)
z80tool serve --allow-files    # erlaubt load_file / save_state / load_state
```

```console
$ echo '{"cmd":"asm","source":"LD A,5\nADD A,A\nHALT"}' | z80tool serve
{"ok":true,"size":4,"segments":[{"start":"0x0000","size":4}],"symbols":{},"start":"0x0000","end":"0x0003"}
```

Eine vollständige Beispielsitzung liegt in `examples/agent_session.jsonl`:
`z80tool serve < examples/agent_session.jsonl`.

## Grundregeln

- Antwort: `{"ok":true, …}` oder `{"ok":false,"error":"…"}`. Der Server **stürzt bei Fehlern nicht ab**;
  ungültiges JSON ergibt ebenfalls eine Fehlerantwort. Ein fehlgeschlagenes `asm` ändert nichts; bei `set` und `call` können Register, die vor dem fehlerhaften Eintrag genannt wurden, bereits gesetzt sein.
- **Zahlen** dürfen überall als JSON-Zahl oder als String geschrieben werden: `"0x4000"`, `"&4000"`,
  `"$4000"`, `"16384"`. Strings sind **Ausdrücke**: `"start+3"`, `"HL"`, `"SP-2"`; Marken aus dem
  letzten `asm` und Registernamen sind verwendbar.
- **Ausgaben** sind Strings in der Form `"0x4000"` (16 Bit) und `"0x2A"` (8 Bit).
- Läufe haben immer ein **Limit** (`max_instructions`, Standard 1 000 000, höchstens 100 000 000; `max_cycles`, Standard
  unbegrenzt). Endlosschleifen halten also mit `"stop":"instruction_limit"` an. `max_instructions:0` (kein Befehlslimit) ist nur
  zusammen mit `max_cycles` erlaubt: Ein Modell kann den Server nicht mit einem riesigen Limit blockieren.
- Dateizugriff ist **aus**, solange der Betreiber `--allow-files` nicht angibt.
- Eine Sitzung ist eine Maschine (64 KiB, eine CPU). Parallele Maschinen = mehrere Prozesse.

## Antwort der Lauf-Befehle

`step`, `step_over`, `run`, `run_until_return`, `call` liefern:

```json
{"ok":true,"stop":"breakpoint","instructions":14,"cycles":121,"pc":"0x400D",
 "regs":{"AF":"0x0F08","BC":"0x0000", … ,"flags":"....X...","IM":0,"IFF1":false,"halted":false}}
```

`stop` ist einer von: `stepped`, `breakpoint`, `address`, `returned`, `halted`, `instruction_limit`,
`cycle_limit`, `watchpoint` (dann zusätzlich `"watch":{"addr":"0x…","write":true}`), `unpacked` (nur `run_until_unpacked`).

Holt ein Lauf Befehle aus **leerem Speicher** (acht Nullbytes ab dem PC: eine NOP-Rutsche durch RAM, in das nichts geladen wurde), stehen
zusätzlich `fetched_from_empty_memory` (Anzahl) und `first_empty_fetch` in der Antwort. Das bedeutet fast immer einen Sprung durch einen Zeiger,
der nie gesetzt wurde, oder eine Routine, die Zustand braucht, den anderer Code herstellt. Die Sprungleiste der Firmware-Stubs zählt nicht.
`flags` ist `SZYHXPNC` mit `.` für gelöschte Bits.

## Befehle

| Befehl | Argumente | Wirkung / Antwort |
|---|---|---|
| `asm` | `source`, `origin?`, `entry?`, `optimize?` | Quelltext assemblieren (`optimize`: `JP` → `JR`, siehe [assembler-syntax.md](assembler-syntax.md#optimierung--o)) und in den Speicher schreiben. Marken werden für spätere Argumente merkbar. `entry` setzt PC. Antwort: `size`, `start`, `end`, `segments`, `symbols`. Bei Fehlern `ok:false` und `errors:[{line,message,source}]`; der Speicher bleibt unverändert. |
| `dis` | `addr?` (PC), `count?` (8) | disassemblieren → `lines:[{addr,label?,bytes,text}]` (Zahlen als `0x..`, Sprungziele als Marken) |
| `read` | `addr`, `len?` (16) | `hex`, `ascii` |
| `write` | `addr`, `hex` oder `bytes[]` | Speicher schreiben |
| `load_file` | `file`, `addr?`, `name?`, `header?`, `set_pc?` | Datei laden; `.sna` ohne `addr` ersetzt das RAM. Eine Datei mit **AMSDOS-Kopf** wird an ihre Ladeadresse geladen und setzt PC auf ihren Einsprung (Antwort `amsdos:{name,type,load,entry,length}`; `header:false` lädt sie unverändert, `set_pc:false` lässt PC stehen). Ein **Disk-Abbild** (`.dsk`) braucht `name` (die Datei darauf). Setzt den Programmbereich für die Analyse (*braucht `--allow-files`*) |
| `dsk_ls` | `file` | Verzeichnis eines CPC-Disk-Abbilds: `format`, `tracks`, `files:[{name,user,size,system,read_only,load?,entry?,type?,length?}]` (*braucht `--allow-files`*) |
| `regs` | – | alle Register |
| `set` | `regs:{"A":5,"HL":"0x4000","PC":"start"}` | Register setzen (Namen wie `A`, `HL`, `AF'`, `IXH`, `IFF1`, `IM`, …) |
| `step` | `n?` (1), `trace?` | n Befehle; mit `trace:true` zusätzlich `trace:[{addr,bytes,text}]` (höchstens 1000 Schritte) |
| `step_over` | – | ein Befehl; bei `CALL`/`RST` bis zur Rückkehr |
| `run` | `until?`, `max_instructions?`, `max_cycles?` | bis Breakpoint/Halt/Limit; mit `until` bis PC diese Adresse erreicht |
| `run_until_return` | Limits | bis die aktuelle Routine zurückkehrt |
| `call` | `addr`, `regs?`, `return_address?` (0xFFF0), Limits | Routine wie eine Funktion aufrufen: Rücksprungadresse auf den Stack, Register setzen, ausführen bis zur Rückkehr. Antwort enthält `returned`: true nur, wenn die Rücksprungadresse **mit dem Stapelzeiger von vor dem Aufruf** erreicht wurde (sonst `note`) |
| `track_writes` | `on?` (true), `clear?` (true) | merken, welche Bytes das Programm schreibt (Laden zählt nicht) |
| `written` | `min_length?` (16), `limit?` (20) | die größten geschriebenen Bereiche: `tracking`, `bytes`, `ranges:[{start,end,size}]` |
| `run_until_unpacked` | `min_written?` (1024), `continue?`, `ignore?:[[first,last],…]`, Limits (Standard 50 Mio. Befehle) | laufen, bis ein Befehl aus einem geschriebenen Bereich von mindestens `min_written` Byte geholt werden soll (`stop:"unpacked"`, `region`); siehe [unpacking.md](unpacking.md) |
| `save_memory` | `addr`, `len`, `file` | Speicher (RAM) in eine Datei schreiben (*braucht `--allow-files`*) |
| `break` / `unbreak` | `addr` | Breakpoint setzen / entfernen |
| `watch` / `unwatch` | `addr`, `kind?` (`read`,`write`,`access`) | Speicherüberwachung |
| `breaks` / `clear_breaks` | – | auflisten / alles löschen |
| `checkpoint` / `rollback` / `drop` | `name` | kompletten Zustand (CPU + RAM) merken / zurückspielen / löschen |
| `checkpoints` | – | Namen auflisten |
| `diff` | `name` | geänderte Speicherbereiche seit dem Checkpoint: `changed_bytes`, `ranges:[{addr,len,before,after}]` (≤ 50 Bereiche, je ≤ 64 Byte angezeigt) |
| `state` / `set_state` | `state` | CPU-Zustand als JSON lesen / setzen |
| `save_state` / `load_state` | `file` | Snapshot-Datei (CPU + RAM) schreiben / lesen (*braucht `--allow-files`*) |
| `interrupt` / `nmi` | `data?` (0xFF) | Interrupt auslösen |
| `input` | `hex` oder `bytes[]` | Bytes für `IN`-Befehle bereitstellen (leer: `0xFF`) |
| `io` | – | Protokoll aller `OUT`-Zugriffe (`output:[{port,value}]`, höchstens 4096, wird geleert) |
| `reset` | `memory?` | CPU zurücksetzen (mit `memory:true` auch RAM löschen) |
| `symbols` | – | Marken des letzten `asm` |
| `help` | – | Befehlsliste |
| `machine` | `type:"cpc"` | CPC-Modus: Videohardware (Gate Array/CRTC), Firmware-Stubs, Bildschirm; setzt einen Stack unterhalb des Bildschirms |
| `load_sna` | `file`, `state?` | CPC-Snapshot inkl. Register und Videozustand laden (*braucht `--allow-files`*) |
| `load_rom` | `file`, `which` (`lower`, `upper`, `both`), `number?` (obere ROMs nach Nummer: BASIC 0, AMSDOS 7, …) | ROM-Abbild; blendet sich wie auf der Hardware ein/aus, der ROM-Select-Port (`&DFxx`) wählt das obere ROM (ohne ROM unter einer Nummer: ROM 0); ersetzt die Firmware-Stubs (*braucht `--allow-files`*) |
| `video` | – | Modus, Farben der 16 Stifte, Rand, CRTC-Register |
| `screen` | `name?`, `border?`, `crop?{col0,row0,col1,row1}`, `zoom?` | Bildschirm als PNG (`png_base64`), 640 px breit; `crop` in Textzellen (80×25), `zoom` 1–16 |
| `screen_diff` | `a`, `b?` | Vergleich gespeicherter Bilder: `changed_pixels`, `bbox`, `text_cells`, `changed_percent` |
| `screens` / `screen_drop` | `name` | gespeicherte Bilder auflisten / löschen |
| `firmware_log` | `clear?` | abgefangene Firmware-Aufrufe mit Registern und `call_site` (der `CALL`, der dorthin führte) |
| `console` | `clear?` | über `TXT_OUTPUT` ausgegebener Text (Stubs) |
| `insert_disk` | `file` | eine `.dsk` ins virtuelle Laufwerk der Stubs: `CAS_IN_OPEN/DIRECT/CHAR/CLOSE/ABANDON` lesen ihre Dateien (ohne Endung: wie AMSDOS auch `.BAS`, `.BIN`), `KL_FIND_COMMAND` liefert die Diskbefehle &81-&89, `READ SECTOR` (&84) / `WRITE SECTOR` (&85) lesen und schreiben Sektoren – direkt oder per `RST &18` (*braucht `--allow-files`*) |
| `disk_log` | `clear?` | was vom Laufwerk gelesen wurde (`load NAME`, `sector T/&ID`) und von welchem `CALL` |
| `save_disk` | `file` | die eingelegte Diskette samt geschriebener Sektoren speichern (*braucht `--allow-files`*) |
| `controls` | `joystick?`, `joystick2?` (Bits 0 hoch, 1 runter, 2 links, 3 rechts, 4/5 Feuer), `keys?` (Tastennummern 0-79) | was das Programm als Joystick und Tasten sieht: `KM_GET_JOYSTICK`, `KM_TEST_KEY` und die Tastaturmatrix hinter PSG-Register 14 |
| `coverage` | `on?`, `clear?`, `from?`, `to?`, `addresses?` | wo Befehle ausgeführt wurden: `ranges` (zusammenhängende Strecken), mit `addresses:true` jede Befehlsadresse – gute Einsprungpunkte für `analyze`, auch für Code, den nur Vektoren erreichen |
| `analyze` | `entries[]`, `from?`+`to?` (das Programm), `inline_strings[]`, `inline_kind?`, `inline_control_args?` (`{"0x1F":2}`: Steuercodes in Inline-Texten und ihre Argumentbytes), `inline_words[]` (Ziele von `CALL`/`RST`, hinter denen eine Adresse steht, z. B. `"0x18"` für FAR CALL), `rst_calls[]` (eigene Restarts eines Programms, die wie ein `CALL` zurückkehren, z. B. `["0x08","0x10"]`: der Fluss geht hinter dem `RST` weiter), `linear?{from,to}`, `reset?`, `tables?` (true) | Codefluss analysieren (nötig für `routines` und `listing`); folgt **Sprungtabellen**, bis nichts Neues mehr auftaucht. Antwort: `instructions`, `jump_tables`, `program` |
| `routines` | `order?` (`bottom_up`, `address`), `undocumented_only?` | Unterprogramme: `start`, `end`, `blocks` (der eigene Code als Bereiche), `size`, `bytes`, `instructions`, `callers` (auch Tail-Calls), `calls`, `depth`, `via_table` (Fall einer Sprungtabelle), bei dokumentierten auch `summary`/`inputs`/`outputs`/`clobbers`/`confidence`; `external_calls` (Firmware/nicht analysiert) |
| `jump_tables` | `max_entries?` (64) | gefundene Sprungtabellen: `kind` (`jump_row`, `pointers`), `jump`, `table`, `bytes`, `followed`, `how`, `entries:[{slot,target,name?}]`; nicht verfolgte (`followed:false`) sind Vorschläge mit `hint` |
| `data_regions` | `apply?` | Vorschläge für Daten: `text`, `pointers` (Tabellen von String-Zeigern), `words` (Zahlentabellen, 16-Bit-Variablen), `zeros`; je mit `reason`, `preview`; `apply:true` trägt sie (außer `zeros`) als Datenbereiche ein |
| `references` | `from`, `to?` (letztes Byte) | worauf die 16-Bit-Operanden eines Codeabschnitts zeigen: `addr`, `name`, `kind` (`code`, `text`, `words`, `pointers`, `bytes`, `variable`, `text?`, `screen`, `firmware`, `unknown`), `length`, `preview`, `used_at` |
| `listing` | `from?`, `to?`, `verify?`, `max_chars?`, `addresses?` (Adresse jedes Befehls in der linken Spalte; nur zum Lesen, nicht verifizierbar), `equates?` (false: kein EQU-Vorspann, für Ausschnitte) | kommentiertes, **neu assemblierbares** Listing; `verified` = identische Bytes. Ohne `to` reicht es bis zum Programmende (Daten hinter dem Code gehören dazu); Adressen im ganzen Programm bekommen Namen, auch außerhalb des Ausschnitts |
| `ann_label`, `ann_routine`, `ann_variable`, `ann_data`, `ann_comment`, `ann_title`, `ann_list` | siehe `help` | Namen und Kommentare; ungültige/doppelte/reservierte Namen werden mit Begründung abgewiesen |
| `ann_constant` | `addr`, `remove?` | der 16-Bit-Operand des Befehls an `addr` ist eine **Zahl**, keine Adresse (`LD BC,&0506`): das Listing erfindet dafür kein `data_xxxx` |
| `ann_remove` | `addr` | nimmt Name, Routinendoku, Variable, **Datenbereich**, Kommentar und Konstanten-Markierung an `addr` zurück |
| `ann_save` / `ann_load` | `file` | Annotationen als JSON speichern / laden (*braucht `--allow-files`*) |

`run` und `call` akzeptieren zusätzlich `irq_every_cycles` (Interrupt alle n T-States; im Takt der Maschinenzeit, also mit dem
Vsync gleichlaufend, und ein `HALT` wartet auf den nächsten) und `history:n` (die letzten n Befehlsadressen in der Antwort).
`screen` zeichnet jede Zeile mit dem Modus und den Farben, die während ihrer Darstellung galten (`raster_splits`), wenn ein
Programm sie mitten im Bild umschaltet; `raster:false` schaltet das ab. `read` und `dis`
zeigen standardmäßig das **RAM**; mit `"view":"cpu"` sieht man, was die CPU mit eingeblendeten ROMs sieht.
`io` liefert je Schreibzugriff `pc` (Adresse des schreibenden `OUT`-Befehls) und bei CPC-Maschinen ein Feld `what` mit der
Wirkung des Zugriffs. `ann_comment` lehnt Adressen **mitten in einem Befehl** mit Erklärung ab (der Kommentar würde nie erscheinen);
`ann_data` lehnt Bereiche ab, die Code enthalten. `info` meldet `protocol: 4` (Sprungtabellen, Datentypen, Referenzen, Konstanten, Blöcke,
Entpacken, Aufrufstellen der Firmware).
Checkpoints sichern auch den Videozustand.

Für das Kommentieren von Programmen mit einem Sprachmodell siehe
[reverse-engineering.md](reverse-engineering.md).

## Typischer Ablauf

```json
{"cmd":"asm","source":"ORG &4000\nstart: LD B,5\n XOR A\nloop: ADD A,B\n DJNZ loop\n HALT","entry":"start"}
{"cmd":"checkpoint","name":"fresh"}
{"cmd":"run"}
{"cmd":"regs"}
{"cmd":"diff","name":"fresh"}
{"cmd":"rollback","name":"fresh"}
{"cmd":"call","addr":"start","regs":{"B":3}}
```

Hinweise zur Verwendung durch ein Modell:

- Nach `asm` die Marken verwenden statt Adressen zu merken (`"until":"loop"`).
- `diff` zeigt, was ein Programm im Speicher verändert hat, ohne den ganzen RAM zu lesen.
- `checkpoint` vor riskantem Code, `rollback` danach; ein Rücksetzen kostet 64 KiB Kopie.
- Mit `trace:true` bekommt man bei kurzen Programmen den Ablauf Befehl für Befehl.
- `call` ist der einfachste Weg, eine einzelne Routine zu testen (Argumente in Registern, Ergebnis
  in `regs`).

## Sicherheit

Der emulierte Code kann nur den eigenen 64-KiB-Speicher und die Port-Warteschlangen beeinflussen,
nichts auf dem Host. Dateizugriff gibt es nur mit `--allow-files`; dann kann der Agent beliebige
Pfade lesen/schreiben, die der Serverprozess erreicht – den Server deshalb mit eingeschränkten
Rechten oder in einem Verzeichnis-Sandbox starten. Laufzeit ist über `max_instructions`
begrenzt (höchstens 100 Millionen Befehle je Aufruf). Ein Robustheitstest schickt dem Server Tausende zufälliger Befehle mit Argumenten
aller JSON-Typen (`tests/test_robustness.cpp`, auch unter den Sanitizern).
