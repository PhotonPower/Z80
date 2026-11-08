# Programme mit einem lokalen LLM kommentieren

Ziel: aus einem Amstrad-CPC-Programm (Snapshot `.sna` oder Binary) ein **möglichst vollständig
kommentiertes Listing** machen – mit sinnvollen Namen für Unterprogramme, Variablen und Daten,
mit Firmware-Aufrufen und Hardware-Zugriffen als benannte Symbole – und zwar mit Hilfe eines
lokalen Modells über **Ollama** (z. B. **Qwen3-VL**, das auch Bilder versteht).

```
        +-------------------------+      Tool-Aufrufe       +---------------------------+
        |  Qwen3-VL (Ollama)      | <---------------------> |  agent/z80_annotate.py    |
        |  liest, testet, benennt |   Text + Screenshots    |  (Python, nur stdlib)     |
        +-------------------------+                         +-------------+-------------+
                                                                          | JSON-Zeilen
                                                            +-------------v-------------+
                                                            |  z80tool serve            |
                                                            |  Emulator, Analyse,       |
                                                            |  Annotationen, Listing    |
                                                            +---------------------------+
```

## Was das Modell kann – und was nicht

Das Modell **führt Code aus, ohne etwas zu gefährden**:

| Werkzeug | Zweck |
|---|---|
| `get_listing`, `disassemble`, `read_memory` | ansehen |
| `run_routine` | Routine im Emulator wie eine Funktion aufrufen (Register/Speicher vorgeben); Ergebnis: Register, geänderter Speicher, Firmware-Aufrufe, Hardware-Port-Schreibzugriffe, Textausgabe, **Bildschirmänderung** (Bereich in Textzellen + optional als Bild) |
| `screenshot` | aktuellen Bildschirm als Bild, optional ausgeschnitten und vergrößert |
| `reset_machine` | Ausgangszustand wiederherstellen (sonst geschieht das nach jedem `run_routine` automatisch) |
| `name_label`, `document_routine`, `name_variable`, `mark_data`, `add_comment` | **Namen und Kommentare** festhalten (`mark_data kind=text` braucht keine Länge: der Text endet mit seinem NUL oder dem Zeichen mit Bit 7) |
| `mark_constant` | ein 16-Bit-Operand ist eine **Zahl**, keine Adresse (`LD BC,&0506`): kein `data_xxxx` dafür im Listing |
| `remove_annotation` | einen Namen, eine Doku, einen Datenbereich oder einen Kommentar zurücknehmen (ein falsches `mark_data` ließ sich vorher nicht rückgängig machen) |
| `finish_routine` | Routine abschließen |

`run_routine` kann vorher **andere Routinen auf derselben Maschine laufen lassen** (`setup`, meist der Programmeinsprung, der Variablen und
Zeiger initialisiert), Speicher als Text vorbereiten (`"memory":[{"addr":"0x5000","text":"HELLO"}]`) und sagt deutlich, wenn eine Routine nicht
wirklich zurückgekehrt ist oder durch **leeren Speicher** lief (ein Zeiger, der nie gesetzt wurde). Geänderter Speicher wird ab dem Zustand nach der
Vorbereitung gemessen, mit ASCII-Spalte und den Namen der Variablen, in denen er liegt (`[run_count]`); Schreibzugriffe **hinter das Programmende**
oder **in Code** (selbstmodifizierender Code) werden eigens gemeldet, Firmware-Aufrufe mit der Adresse ihres `CALL`.

Das Modell **kann den Code nicht verändern**. Es liefert nur Namen und Texte. Das fertige Listing wird
aus den echten Bytes erzeugt und anschließend **neu assembliert und Byte für Byte verglichen**
(`VERIFIED`). Auch halluzinierte Kommentare können das Programm deshalb nicht verfälschen; falsche
Namen (Registernamen, Mnemonics, Firmware-Namen, doppelte Namen) werden mit einer Erklärung
abgewiesen, aus der das Modell lernt.

Jedes Experiment läuft auf einem Checkpoint und wird danach zurückgerollt (inklusive Bildschirmmodus
und Palette). Das Original-Snapshot bleibt unberührt.

## Voraussetzungen

- gebautes `z80tool` (siehe README), Python ≥ 3.9 (nur Standardbibliothek)
- Ollama mit einem Modell, das **Tools und Bilder** kann, z. B.:

  ```sh
  ollama pull qwen3-vl:8b        # oder eine größere Variante, siehe unten
  ```

- ein Snapshot (`.sna`) des laufenden Programms, ideal an einer Stelle, an der der Bildschirm
  bereits aufgebaut ist; das Programm sollte als `PC` im Snapshot eine sinnvolle Einsprungadresse haben.
  Optional die CPC-ROMs (eigene Kopie; sie sind nicht Teil dieses Projekts).

## Benutzung

```sh
python3 agent/z80_annotate.py --sna spiel.sna --project spiel.z80proj.json --out spiel_kommentiert.asm \
        --lang Deutsch --model qwen3-vl:8b --log spiel.log.jsonl
```

| Option | Bedeutung |
|---|---|
| `--sna DATEI` / `--bin DATEI --org 0x4000` | Eingabe; eine Datei mit **AMSDOS-Kopf** wird dort geladen, wo der Kopf sagt (Einsprung ebenso) |
| `--dsk ABBILD --dsk-file NAME` | ein Programm direkt von einem CPC-Disk-Abbild (`z80tool dsk ls ABBILD` zeigt die Dateien) |
| `--dry-run` | nur laden und analysieren: Routinen in Bearbeitungsreihenfolge, Größen, Sprungtabellen, Daten, Token-Schätzung; **kein** Modell |
| `--no-auto-tables`, `--no-auto-data` | vorgeschlagene Zeigertabellen nicht verfolgen bzw. erkannte Strings/Tabellen nicht als Daten eintragen |
| `--entry ADR` (mehrfach) | Einsprungpunkte (Standard: PC aus dem Snapshot) |
| `--inline-string ADR` (mehrfach) | Routinen, hinter deren `CALL` ein Text steht (letztes Zeichen Bit 7) |
| `--rom DATEI:lower\|upper\|both` | echte Firmware benutzen statt der Stubs (siehe unten) |
| `--boot`, `--boot-instructions N` | Firmware vor dem Laden per Kaltstart hochfahren (für `--bin`) |
| `--irq ZYKLEN` | während `run_routine` alle ZYKLEN T-States einen Interrupt auslösen (CPC: 13312) |
| `--project DATEI` | Annotationen; wird geladen, falls vorhanden, und nach **jeder Routine** gespeichert → Abbrechen und Fortsetzen ist möglich |
| `--out DATEI` | fertiges, verifiziertes Listing |
| `--only ADR` | nur diese Routine bearbeiten (zum Ausprobieren) |
| `--model`, `--host`, `--think true\|false`, `--num-ctx`, `--max-turns` | Modell und Grenzen |
| `--lang` | Sprache der Kommentare |
| `--log DATEI` | jede Modellnachricht und jedes Tool-Ergebnis (JSON-Zeilen), zum Nachvollziehen |

Rückgabewert: `0`, wenn das Listing verifiziert ist; `1`, wenn nicht; `2` bei Verbindungsproblemen.

**Vollständigkeitsprüfung:** Beim ersten `finish_routine` prüft der Orchestrator die Routine: noch unbenannte Labels
(`loc_…`), ungenannte Speicherstellen (`LD (&1C97),A`, `LD HL,data_40A5`), eine Dokumentation ohne `inputs`/`outputs`/`clobbers` oder ohne
`confidence`, und zu wenige Kommentare (etwa einer je 10 Befehle, höchstens einer je zwei) werden dem Modell aufgelistet. Ein zweiter Aufruf gilt
im Orchestrator immer als endgültig, das Modell kann also nie festhängen (`z80_drive.py finish` ist dagegen streng, siehe unten).

Ablauf: Analyse ab den Einsprungpunkten → Routinen **von den Blättern zur Wurzel** (zuerst, was nichts
Undokumentiertes aufruft), je Routine eine eigene Unterhaltung mit dem Listing, den Namen der bereits
dokumentierten Aufgerufenen und den Werkzeugen → Annotationen speichern → am Ende das verifizierte Listing.
Eine zweite Ausführung mit demselben `--project` fragt das Modell nur noch nach noch offenen Routinen.

## Mehrere Durchgänge: schnell zuerst, gründlich für die Lücken

Ein Thinking-Modell braucht je Antwort Minuten; für das Standardmäßige ist ein Instruct-Modell viel schneller. Deshalb
lassen sich **mehrere Modelle nacheinander** einsetzen:

```sh
python3 agent/z80_annotate.py --sna spiel.sna --entry 0x05DC --project spiel.proj.json --out spiel.asm \
        --lang Deutsch --report spiel_luecken.md \
        --model qwen3-vl:8b-instruct --max-turns 12 \
        --refine-model qwen3-vl:8b --refine-max-turns 25 --refine-max-tokens 12000
```

- **Durchgang 1** (`--model`): das schnelle Modell arbeitet alle Routinen von unten nach oben ab.
- **Lücken werden gemessen, nicht geschätzt**: nicht dokumentiert, Labels noch `loc_…`/`sub_…`, Speicherstellen ohne Namen,
  zu wenige Kommentare, oder das Modell hat seine Dokumentation selbst als **`confidence: low`** markiert
  (`document_routine` hat dafür ein Feld; im Listing erscheint „confidence: low – a guess, please verify“).
- **Durchgang 2, 3, …** (`--refine-model`, beliebig oft): jedes Modell bekommt **nur die Routinen mit Lücken**, sieht die
  Annotationen des Vorgängers als Hypothesen eines Kollegen, soll sie durch Ausführen **prüfen, korrigieren (überschreiben)
  und ergänzen** und die Lücken schließen. Vollständige Routinen werden nicht erneut angefasst (`--refine-all` prüft trotzdem
  alle, `--refine-medium` nimmt auch „medium“ mit).
- **Fähigkeiten erkennt der Orchestrator selbst** (Ollama `/api/show`): ein Modell ohne Tool-Unterstützung wird sofort
  abgelehnt; eines ohne Vision bekommt statt Screenshots nur die Textzusammenfassung (so lassen sich auch reine
  Textmodelle als Prüfer einsetzen); Thinking wird in Refine-Durchgängen eingeschaltet, wenn das Modell es kann
  (`--refine-think`, `--refine-max-tokens`).
- **`--report`** schreibt am Ende, was trotz allem offen blieb (je Routine die Gründe), damit du gezielt nachsehen kannst.
- Abbrechen und Fortsetzen geht mit derselben `--project`-Datei weiter: Durchgang 1 überspringt Dokumentiertes,
  Durchgang 2 bewertet alles neu.

### Stufenweise arbeiten

Weil `--project` fortgesetzt wird, lässt sich das auch in Etappen machen: erst nur das schnelle Modell über **alles** laufen lassen
(`--model qwen3-vl:8b-instruct`, ohne `--refine-model`), dann den Bericht ansehen, dann mit demselben `--project` und
`--refine-model …` die Lücken schließen. Das Modell soll dabei in **einer** Antwort mehrere Tool-Aufrufe schreiben; vor dem
Rundenlimit wird es gewarnt, und eine bereits dokumentierte, nur nicht abgeschlossene Routine zählt als „partial“ und wird im
nächsten Durchgang geprüft.

**Was `gaps left: 0` bedeutet:** Der Bericht misst *Vollständigkeit* (Namen, Kommentare, Dokumentation, Selbsteinschätzung),
nicht *Richtigkeit*. Kleine Modelle schreiben fast immer `confidence: high`. Wichtige Routinen kannst du deshalb mit
`--refine-all` (jede dokumentierte Routine wird erneut geprüft) oder von einem größeren Modell gegenlesen lassen.

## Probelauf ohne Modell (`--dry-run`)

```sh
build/z80tool asm examples/cpc/demo_cpc.asm -o demo_cpc.bin
python3 agent/z80_annotate.py --bin demo_cpc.bin --org 0x4000 --dry-run
```

zeigt, was ein echter Lauf tun würde: welche Datenbereiche und Sprungtabellen automatisch erkannt werden, die Routinen in Bearbeitungsreihenfolge mit
Größe, Aufrufern, Firmware-Aufrufen und einer Schätzung der Prompt-Größe, und eine Warnung, wenn die größte Routine mehr als 40 % des
Kontextfensters braucht. So lassen sich Ladeadresse, Einsprungpunkte und `--num-ctx` prüfen, bevor ein Modell Stunden rechnet. Eine ausführliche
Anleitung mit diesem Beispiel steht in [walkthrough.md](walkthrough.md).

## Von Hand: `agent/z80_drive.py`

Dieselben Werkzeuge, Berichte und Prüfungen wie im Orchestrator, aber **ein Schritt je Aufruf**, der Zustand in einem Arbeitsverzeichnis: für einen
Menschen oder einen Agenten in einer Shell, der die Rolle des Modells übernimmt (wenn kein Ollama läuft).

```sh
python3 agent/z80_drive.py --work W init --bin demo_cpc.bin --org 0x4000
python3 agent/z80_drive.py --work W status                 # Routinen in Reihenfolge, was offen ist, was als Nächstes kommt
python3 agent/z80_drive.py --work W show 0x401C            # der Auftragstext, den ein Modell bekäme (--system: auch die Anweisungen)
python3 agent/z80_drive.py --work W tool run_routine '{"addr":"0x401C","registers":{"HL":"0x5000"},"memory":[{"addr":"0x5000","text":"Hi"}]}'
python3 agent/z80_drive.py --work W tool document_routine '{"addr":"0x401C","name":"print_string","summary":"…","inputs":"HL = text","outputs":"none","clobbers":"AF HL","confidence":"high"}'
python3 agent/z80_drive.py --work W finish 0x401C          # streng: erst, wenn nichts mehr fehlt (--accept-gaps: die Regel des Orchestrators)
python3 agent/z80_drive.py --work W listing --out kommentiert.asm
python3 agent/z80_drive.py --work W report
```

Screenshots (`run_routine` mit `"screenshot": true`, Werkzeug `screenshot`) landen als `W/shot_N.png`.

## Was die Analyse erkennt

### Sprungtabellen

Code, der nur über einen berechneten Sprung erreicht wird, fand die Analyse früher nicht. Jetzt erkennt sie:

- **Adresstabellen hinter einem Verteiler**: `LD HL,tabelle` … `LD E,(HL)` / `INC HL` / `LD D,(HL)` / `EX DE,HL` / `JP (HL)` (auch `JP (IX)`,
  `JP (IY)` und `PUSH HL` / `RET`); die Tabelle endet vor dem nächsten Verweis, Sprungziel oder dem ersten Ziel hinter ihr. Ihre Einträge werden als
  Code verfolgt, die Tabellenbytes werden nie als Code dekodiert.
- **Reihen von `JP nn`**, in die ein berechnetes `JP (HL)` springt (`LD DE,tabelle` / `ADD HL,DE` / `JP (HL)`).
- **Vorschläge**: drei oder mehr Codeadressen hintereinander hinter einem bloßen Datenverweis (`LD HL,vektoren`) könnten auch Zahlen sein; sie
  werden nur vorgeschlagen (`followed:false`). Der Orchestrator markiert sie selbst als `pointers` und analysiert neu (`--no-auto-tables` schaltet
  das ab); ein von Hand oder vom Modell als `pointers` markierter Bereich wird immer verfolgt.

Die Fälle einer Tabelle sind **eigene Routinen** (`via_table`; der Auftragstext sagt „reached through the jump table at …“). Im Listing steht über
der Tabelle ein Kommentar, die Einträge erscheinen als `DW sub_4038,sub_403E,…`. Auch `z80tool dis` folgt den Tabellen.

### Datentypen (`data_regions`)

Für Bytes, die weder Code noch annotiert sind:

- **Strings**: druckbare Zeichen mit NUL am Ende oder mit Bit 7 im letzten Zeichen (die CPC-Konvention), referenziert oder lang genug; das Listing
  schreibt sie als `DB "Hello",&0D,&00` bzw. `DB "Worl",'d'+&80`, ein String je Zeile.
- **Tabellen von String-Zeigern** (auch auf kurze Strings wie „No“), samt der Strings; sie werden vor den Strings gesucht, weil das High-Byte einer
  Adresse oft druckbar ist (&40 = `@`).
- **Zahlentabellen**: `LD HL,tabelle` und danach ein wortweiser Zugriff `LD r,(HL)` / `INC HL` / `LD r,(HL)`, auch in der Routine, die per `CALL`
  direkt danach aufgerufen wird (die Tabelle wird oft in HL übergeben).
- **16-Bit-Variablen**: eine Speicherstelle, auf die der Code nur mit 16-Bit-Ladebefehlen zugreift (`LD HL,(nn)`, `LD (nn),DE`) und deren Adresse
  nirgends geladen wird. Bytes sagen nichts (das zeigt das Listing ohnehin).
- **Nullbereiche** ab 16 Byte (das Listing schreibt `DS n,&00`; keine Annotation nötig).

Der Orchestrator übernimmt die Vorschläge selbst (`--no-auto-data` schaltet das ab). Sie sind Vermutungen: `remove_annotation` nimmt einen falschen
Bereich zurück.

### Referenzen, Konstanten, Namen mit Versatz

- Unter dem Listing einer Routine steht im Auftragstext **„Referenced from this routine“**: worauf ihre Operanden zeigen, mit Art und Inhalt
  (`0x40A5 data_40A5 (text, 10 bytes): "Z80 demo\r"`, Zahlentabellen mit ihren Werten, Bildschirmadressen); das Modell sieht so den Text hinter einer
  nackten Adresse.
- Ein 16-Bit-Wert, der zufällig einer Adresse im Programm gleicht, aber eine Zahl ist (`LD BC,&0506`), bekommt mit `mark_constant` keinen Platzhalter.
- Adressen in benannten Variablen und Datenbereichen erscheinen als `name+versatz`, ab 10 hexadezimal (`spieler+&26`).

### Routinen: Tail-Calls und Blöcke

Ein Sprung zum Anfang einer **anderen** Routine (`JP print` am Ende eines Falls) ist ein **Tail-Call**: Das Ziel ist ein Aufgerufener, nicht Teil
der Routine, und der Sprung zählt bei ihm als Aufrufer. Was eine Routine wirklich umfasst, beschreiben ihre **Blöcke** (`blocks`): Ein gemeinsamer
Ausgang weit hinten ist ein eigener Block, ohne dass die Hülle die Nachbarn mitnimmt. Auftragstext („size: 11 bytes in 2 parts“), Größe und
Vollständigkeitsprüfung benutzen die Blöcke.

## Was im Listing automatisch passiert

- **Firmware**: `CALL &BB5A` → `CALL TXT_OUTPUT`, dazu der `EQU`-Block `TXT_OUTPUT EQU &BB5A`
  (231 Einträge der Sprungleiste, siehe `tools/gen/gen_cpc_data.py`).
- **Hardware**: `LD BC,&7F8D` + `OUT (C),C` → `LD BC,GATE_ARRAY+&8D` mit dem Kommentar
  *Gate Array: screen mode 1, lower ROM off, upper ROM off*. Erkannt werden Gate Array, CRTC, ROM-Auswahl,
  Drucker, PPI und Disc-Ports, auch bei `LD B,n` / `LD A,n` / `OUT (C),A` (der Wert in `A` wird verfolgt).
- **Idiome, die man leicht falsch liest**, werden erklärt: `LD SP,&C000` (der Stack wächst *abwärts*, er liegt
  unterhalb von &C000, nicht im Bildschirm), `HALT`, `IM 1/2` und das Installieren eines Interrupt-Handlers
  (`LD A,&C3` / `LD (&0038),A` / `LD HL,x` / `LD (&0039),HL` → „Handler bei x“). Auslöser war ein echter Lauf, in dem ein
  Modell genau das als „Scroll-Register“ missdeutet hat.
- **Kommentare gehen nie verloren oder falsch**: Das Listing, das das Modell liest, hat die **Adresse jedes Befehls in der linken
  Spalte** (das Modell muss nicht mehr Bytes zählen); ein Kommentar mitten in einem Befehl wird abgelehnt statt stillschweigend
  verschluckt; und neben einem Benutzerkommentar bleibt die automatisch erkannte Hardware-Erklärung stehen
  (`; mein Text [Gate Array: screen mode 1, …]`), damit ein falscher Kommentar eine Tatsache nicht verdeckt.
  `run_routine` meldet für jeden Port-Schreibzugriff, **welcher Befehl** ihn ausgelöst hat und ob der zur Routine selbst gehört
  oder zu aufgerufenem Code (ein Modell hatte PPI-Zugriffe einer Unterroutine den `OUT`-Zeilen der Routine zugeschrieben).
- **Bildschirmadressen**: `LD DE,&C78A` bekommt den Hinweis *char row 24, byte column 10, scan line 0*.
- **Variablen und Tabellen**: `LD A,(&A010)` → `LD A,(run_counter)` sobald benannt; Zeiger auf noch
  unbenannte Daten erscheinen als `data_xxxx`, Sprungziele als `sub_xxxx`/`loc_xxxx` (Platzhalter,
  die das Modell ersetzt).
- **Daten**: Texte (`DB "HELLO",0`, auch Bit-7-Endezeichen), Bytes, Wörter, Zeigertabellen mit Namen.

## Firmware: Stubs oder echte ROMs

Ohne `--rom` fängt der Emulator Aufrufe der Firmware-Sprungleiste (`&B900-&BDFF`) ab: sie werden
**protokolliert** (`firmware_log`: Name und Register), wenige werden **nachgebildet**
(`SCR_SET_MODE`, `MC_SET_MODE`, `SCR_CLEAR`, `SCR_SET_INK`, `SCR_SET_BORDER`, `MC_SET_INKS`;
`TXT_OUTPUT` sammelt den Text in `console`), danach kehrt die Routine zurück. Das reicht, um zu sehen,
*was* ein Programm von der Firmware will. Zeichen werden **nicht** gemalt (ohne ROM gibt es keinen
Zeichensatz).

Für ganze Spiele bilden die Stubs einen kleinen **Kernel** nach:

- **Laufwerk** (`insert_disk`): AMSDOS-Dateien (`CAS_IN_*`), die Diskbefehle über `KL_FIND_COMMAND` und `RST &18`
  (READ/WRITE SECTOR auch auf kopiergeschützten Spuren), `MC_START_PROGRAM`; `disk_log` zeigt, was wann geladen wurde.
  So lässt sich ein Spiel analysieren, das während des Spiels nachlädt.
- **Interrupts**: der Interrupt-Einsprung der Firmware (`&0038` → `&B941`), das Frame-Flyback-Ereignis
  (`KL_NEW_FRAME_FLY`, mit gesicherten Registern), das Zurückschreiben der Farbtabelle bei jedem Bildrücklauf
  (wie das Screen-Pack), `KL_L_ROM_*` mit `EI` am Ende.
- **Sprungleiste und Restart-Block**: die Einträge enthalten wie im Original `RST &08` + ROM-Adresse; ein Programm, das
  die Adresse liest und das ROM direkt aufruft (z. B. `MC WAIT FLYBACK`), landet trotzdem im Stub. `&000E`/`&0016`/`&001E`
  enthalten PCBC/PCDE/PCHL.
- **Eingaben** (`controls`): Joystick und Tasten für `KM_GET_JOYSTICK`, `KM_TEST_KEY` und die Tastaturmatrix.
- **Bild**: Rasterteilungen (Modus/Farben mitten im Bild) erscheinen im Screenshot zeilengenau.
- **`coverage`** zeichnet auf, welcher Code lief – ausgeführte Adressen, die die Flussanalyse nicht erreicht, sind
  Einsprungpunkte (Interrupt-Handler, Vektoren, Tabellen).

Mit `--rom DATEI:both` (ein 32-KiB-Abbild: unteres ROM + BASIC) bzw. `--rom os.rom:lower --rom basic.rom:upper`
läuft die **echte Firmware**; die ROMs blendet das Gate Array wie auf der Hardware ein und aus.

Das ist nicht nur behauptet, sondern getestet: Mit dem ROM der CPC 6128 (liegt dem Emulator Caprice32 bei;
Amstrad hat die Weitergabe für Emulatoren erlaubt – das ROM ist **nicht** Teil dieses Repositorys) bootet
der Emulator per Kaltstart bis *„Amstrad 128K Microcomputer (v3) … BASIC 1.1 … Ready“* (30 Millionen Befehle,
Vsync am PPI und 300-Hz-Interrupts werden nachgebildet). Ein anschließender Aufruf von `TXT_OUTPUT` mit `A="Z"`
zeichnet das Zeichen mit dem echten Zeichensatz, und `screen_diff`/`screen` zeigen genau diese Stelle –
dieselbe Kette, mit der das Modell später sieht, was eine Routine am Bildschirm verändert
(`tests/agent/test_firmware.py`, läuft in der CI).

- Ein Snapshot enthält schon eine laufende Firmware (RAM-Variablen); dann genügt `--rom`.
- Für ein rohes Binary (`--bin`) ohne Snapshot startet `--boot` zuerst die Firmware per Kaltstart.
- Routinen, die auf Interrupts angewiesen sind (Zeitgeber, Tastaturabfrage der Firmware), brauchen
  `--irq 13312` (ein Interrupt alle 13 312 T-States ≈ 300 Hz).

## Qwen3-VL über Ollama – Hinweise

- Das Skript ist gegen die dokumentierte Ollama-Schnittstelle (`/api/chat` mit `tools`, Bilder in
  `images`) geschrieben und gegen einen **Mock-Server** getestet (`tests/agent`). **Mit einem echten
  Qwen3-VL habe ich es noch nicht ausprobiert** – bitte rechne mit etwas Feinschliff an
  Prompt und Parametern.
- Größere Modelle machen deutlich weniger Fehler bei Tool-Aufrufen und Hardware-Wissen. Der Lauf
  braucht je Routine mehrere Runden; `--max-turns` begrenzt sie.
- **Thinking-Modus**: Standard ist `--think false`. Qwen3-VL „denkt“ sonst oft seitenlang, wiederholt sich
  dabei und verbraucht das ganze Kontextfenster, bevor es einen einzigen Tool-Aufruf schreibt. Das Log zeigt
  dann `"content": ""` und ein riesiges, mittendrin abgebrochenes `"thinking"`. Der Orchestrator erkennt das
  (`done_reason: length`), verwirft die Antwort und fordert einen konkreten nächsten Schritt an. Hilft das nicht:
  einen `-instruct`-Tag wählen (z. B. `qwen3-vl:8b-instruct`, Tag-Namen mit `ollama list`/Bibliothek prüfen).
  Modelle ohne Thinking-Modus werden automatisch ohne den Parameter erneut angefragt.
- **Modelle, die trotz `--think false` denken** (reine Thinking-Varianten): Das funktioniert, ist aber langsam,
  und jede Antwort enthält erst Tausende Token Nachdenken. Der Orchestrator erkennt das (Hinweis auf der Konsole)
  und hebt das Antwortlimit bei jedem Abschneiden um die Hälfte an (4096 → 6144 → 9216 → … bis 16384). Schneller
  und billiger ist ein `-instruct`-Tag; welche Tags dein Ollama kennt, zeigt `ollama list` bzw. die Modellseite.
- **Nach `git pull` immer neu bauen** (`cmake --build build`): Idiom-Kommentare, Vollständigkeitsprüfung und
  Protokoll stecken im C++-Teil. Ein veraltetes `z80tool` bricht mit einer entsprechenden Meldung ab.
- **Abtastung**: Standard `--temperature 0.7`, `top_p 0.8`, `top_k 20`, `presence_penalty 1.0`. Niedrige
  Temperaturen (oder Greedy) führen bei Qwen zu endlosen Wiederholungen.
- `--max-tokens` (Standard 4096) begrenzt **eine** Antwort, `--num-ctx` (Standard 32768) das ganze Fenster
  aus Listing, Verlauf und Bildern (ein zugeschnittener Screenshot ist klein, ein Vollbild nicht). Mit
  `--think true` brauchst du deutlich mehr, z. B. `--max-tokens 12000`.
- Beobachte `--log`: Dort siehst du, welche Tests das Modell gemacht und was es daraus geschlossen hat.

## Robustheit gegenüber dem Modell

Echte Modelle machen Formatfehler; der Orchestrator fängt die häufigen ab (alles in `tests/agent`):

- Zahlen/Booleans als Strings (`"false"`, `"500"`), verschachteltes JSON als String, Argumente als
  JSON-Text, unbekannte Zusatzargumente (werden gemeldet und ignoriert), Namen wie `functions.run_routine`
- Aufrufe, die das Modell als Text schreibt (`<tool_call>{"name": …}</tool_call>`, ```json-Block)
  statt in `tool_calls`
- Fehlversuche bekommen eine Erklärung, nach dreimal derselben Fehlermeldung einen Hinweis
- begrenzter Kontext: Bilder werden bis auf die neuesten zwei entfernt, alte Tool-Ergebnisse gekürzt
- `run_routine` ist auf 5 Millionen Befehle begrenzt; Endlosschleifen halten an
- das Listing ist am Ende **immer** korrekt, auch wenn das Modell nichts oder Unsinn liefert

## Wenn etwas schiefgeht

| Meldung | Bedeutung und Verhalten |
|---|---|
| `the model wrote an invalid tool call` / Ollama `HTTP 500 … invalid character 'x' …` | Das Modell hat ein Argument als ungültiges JSON geschrieben (typisch: nackte `0x0C5A` statt `"0x0C5A"`). Das ist ein Fehler der Modellantwort, nicht des Servers. Der Orchestrator weist das Modell darauf hin und fragt neu (bis zu 4-mal je Routine). |
| `model error on 0x…: … - skipping this routine` | Der Server ist bei **einer** Routine gescheitert (z. B. Modell abgestürzt). Sie wird übersprungen, der Lauf geht weiter; beim nächsten Start mit demselben `--project` wird sie erneut versucht. Nach drei Routinen in Folge endet der Lauf (Server vermutlich weg). |
| `cannot reach Ollama …` / `HTTP 404` vor der ersten Antwort | Konfigurationsproblem (Host, Modellname): sofortiger Abbruch mit Rückgabewert 2. |
| `answer cut off at the token limit` | siehe oben: Antwortlimit wird automatisch erhöht. |
| `turn limit reached … counted as partial` | Routine ist dokumentiert, aber `finish_routine` fehlte; ein Refine-Durchgang prüft sie. |

## Grenzen

- Nur flache 64 KiB (128K-Banking der CPC 6128 fehlt), nur Modi 0–2, kein Blinken, kein CRTC-Timing,
  keine Tastatur/Joystick/Sound/Floppy.
- Interrupts werden nur auf Wunsch periodisch ausgelöst (`irq_every_cycles` bzw. `--irq`); die Tastatur
  liefert „keine Taste“, Eingaben an laufende Programme gibt es noch nicht.
- Der Vsync des PPI ist ein einfaches Zeitmodell (79 872 T-States je Bild); Programme, die auf exakte
  Rasterpositionen warten, laufen damit nicht korrekt.
- Die Analyse folgt Sprüngen statisch. Berechnete Sprünge über Tabellen erkennt sie in den üblichen Formen (siehe oben); Code, der auf andere Weise
  erreicht wird (selbst gebaute Adressen, `RST`-Verteiler mit Daten dahinter), muss als weiterer `--entry` angegeben oder als `pointers` markiert
  werden. Eine Tabelle, die nur als Zeiger weitergereicht wird, ohne dass Code sie wortweise liest, erkennt die Datentyp-Suche nicht.
- Gepackte Programme: siehe [unpacking.md](unpacking.md).
- Die Qualität der Namen und Kommentare hängt am Modell. Das Verfahren stellt nur sicher, dass sie das
  Programm nicht verändern und dass das Modell seine Behauptungen am laufenden Code prüfen *kann*.

## Eigene Anbindung

Alles, was der Orchestrator tut, geht über das JSON-Protokoll von `z80tool serve`
([agent-protocol.md](agent-protocol.md)); ein anderes Framework (MCP, eigene Schleife) kann dieselben
Befehle verwenden.
