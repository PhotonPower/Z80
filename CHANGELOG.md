# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden hier festgehalten.
Das Format orientiert sich an [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionierung an [Semantic Versioning](https://semver.org/lang/de/).

## [Unreleased]

### Hinzugefügt (Firmware-Kernel für ganze Spiele, erprobt an SORCERY+)
- **Virtuelles Laufwerk** (`insert_disk`, `disk_log`, `save_disk`): `CAS_IN_OPEN/DIRECT/CHAR/CLOSE/ABANDON` lesen Dateien einer `.dsk`
  (ohne Endung wie AMSDOS auch `.BAS`/`.BIN`), `KL_FIND_COMMAND` liefert die Diskbefehle &81-&89, `READ SECTOR`/`WRITE SECTOR` lesen und
  schreiben beliebige Sektoren (auch kopiergeschützte Spuren), direkt oder per `RST &18` (FAR CALL); `MC_START_PROGRAM`.
  `DskImage::writeSector`.
- **Interrupts wie mit der Firmware**: `&0038` → Interrupt-Einsprung der Firmware, Frame-Flyback-Ereignis (`KL_NEW_FRAME_FLY`, Register
  gesichert), Farbtabelle (`SCR_SET_INK`) wird bei jedem Bildrücklauf zurückgeschrieben; `irq_every_cycles` läuft im Takt der Maschinenzeit
  (gleichlaufend mit dem Vsync) und `HALT` wartet auf den nächsten Interrupt; `KL_L_ROM_*`/`KL_ROM_RESTORE` enden mit `EI`.
- **Sprungleiste mit ROM-Adressen** (`RST &08` + Adresse) und ein „Fake-ROM“: Programme, die eine ROM-Adresse aus der Sprungleiste lesen und
  direkt aufrufen (z. B. `MC WAIT FLYBACK`), landen im Stub; Restart-Block mit PCBC/PCDE/PCHL. `Cpu::setExecHookRange`.
- **Eingaben** (`controls`): Joystick und Tasten für `KM_GET_JOYSTICK`, `KM_TEST_KEY` und die Tastaturmatrix (PSG-Register 14).
  Weitere Stubs: `SCR_GET_MODE`, `SCR_GET_INK`, `MC_WAIT_FLYBACK`.
- **Rasterteilung im Screenshot**: Modus- und Farbwechsel mitten im Bild werden mit ihrer Bildzeile aufgezeichnet; `screen` zeichnet jede
  Zeile mit ihrem Zustand (`raster_splits`, `cpc::renderScreenRaster`).
- **`coverage`**: ausgeführte Befehlsadressen (Strecken oder alle), als Einsprungpunkte für die Analyse; **`history:n`** bei `run` und Co.
- **Analyse**: `inline_words` (Adresse hinter `RST &18`/`CALL`, im Listing als `DW name`), `inline_control_args` (Steuercodes mit
  Argumentbytes in Inline-Texten); die Firmware-Sprungleiste ist auf dem CPC kein Teil des Programms.

### Hinzugefügt (Hardware-Scrolling, erprobt an ROLAND IN THE CAVES)
- Firmware-Stubs `SCR_SET_OFFSET`, `SCR_SET_BASE`, `SCR_GET_LOCATION`, `SCR_CHAR_POSITION` (mit dem Offset und dem 2-KiB-Umlauf);
  `video` meldet `screen_offset`, `cpc::VideoState::screenOffset()`.

### Hinzugefügt (echte Firmware mit Diskette, erprobt am Kopierschutz von GET DEXTER)
- **Floppy-Controller µPD765** (`Fdc`, Ports `&FA7E`/`&FB7E`/`&FB7F`) für die eingelegte `.dsk`: SEEK, RECALIBRATE, SENSE INTERRUPT STATUS,
  SENSE DRIVE STATUS, READ ID, READ DATA, READ DELETED DATA, WRITE (DELETED) DATA; ST1/ST2 aus dem Abbild (Datenfehler, Deleted-Data-Marke),
  Ende am Zylinderende wie auf dem CPC; `disk_log` → `fdc`. `DskImage::trackRef`.
- **Obere ROMs nach Nummer** (`load_rom` mit `number`): der ROM-Select-Port wählt sie, wie die Hardware (fehlende Nummer → ROM 0);
  `Memory::selectHighRom`.
- Firmware-Stubs weichen, wo ein Programm eigenen Code über die Sprungleiste kopiert hat (Kopierschutz-Lader laufen dort).

### Hinzugefügt (Analyse von Programmen mit eigenen Restarts, erprobt an GET DEXTER)
- `analyze` mit `rst_calls` (z. B. `["0x08","0x10"]`): diese Restarts kehren wie ein `CALL` zurück, der Codefluss geht hinter dem `RST`
  weiter und die Routine am Restart wird analysiert (die Restarts der CPC-Firmware dagegen beenden den Pfad). `Disassembler::addCallingRestart`.

### Behoben
- Listing: ein mit `ann_constant` als Zahl markierter Operand bekommt keinen „screen at &C000“-Kommentar mehr.
- `MC_SET_INKS`-Stub las HL statt DE und ohne den Rand als erstes Byte.
- CRTC-Voreinstellung ohne R3/R4/R7 (Bildposition für Rasterteilungen falsch).

### Hinzugefügt (Abgleich mit dem Schwesterprojekt [MC68000](https://github.com/PhotonPower/MC68000))
- **Sprungtabellen** (`z80/analysis.hpp`, Befehl `jump_tables`, Feld `jump_tables` in `analyze`): Adresstabellen hinter einem Verteiler
  (`LD HL,tab` … `LD E,(HL)` / `INC HL` / `LD D,(HL)` / `EX DE,HL` / `JP (HL)`, auch `JP (IX)`/`JP (IY)` und `PUSH rr`/`RET`) und Reihen von `JP nn`,
  in die ein berechnetes `JP (HL)` springt, werden erkannt und verfolgt, bis nichts Neues mehr auftaucht; als `pointers` markierte Bereiche ebenso;
  Zeilen von Codeadressen hinter einem bloßen Datenverweis werden vorgeschlagen (`followed:false`). Die Fälle sind eigene Routinen (`via_table`),
  das Listing zeigt die Tabelle mit Kommentar und `DW sub_…`. Auch `z80tool dis` folgt den Tabellen. Der Disassembler hat eine Datensperre
  (`setDataBarrier`): annotierte Daten und Tabellen werden nie als Code dekodiert.
- **Datentypen** (`data_regions`, `apply:true`): Strings (NUL oder Bit 7 am Ende), Tabellen von String-Zeigern, Zahlentabellen (wortweiser Zugriff,
  auch in der direkt aufgerufenen Routine), 16-Bit-Variablen, Nullbereiche. Das Listing schreibt einen String je Zeile und bricht Datenzeilen nie
  über einem Label um.
- **`references`**: worauf die 16-Bit-Operanden eines Codeabschnitts zeigen (Name, Art, Inhalt, benutzende Befehle); der Auftrag an das Modell
  enthält das als „Referenced from this routine“.
- **Konstanten** (`ann_constant`, Werkzeug `mark_constant`, JSON `constants`): ein 16-Bit-Operand, der eine Zahl ist, bekommt kein `data_xxxx`.
- **Routinen: Tail-Calls und Blöcke**: ein Sprung zum Anfang einer anderen Routine ist ein Aufruf, kein Teil der Routine; `routines` liefert `blocks`,
  `start`, `end`, `bytes`, `instructions`, bei dokumentierten auch die Doku. Die Programm-Einsprungstelle ist eine Routine mit eigenem Kopf.
- **Entpacken** ([docs/unpacking.md](docs/unpacking.md)): `track_writes`, `written`, `run_until_unpacked` (Stopp `unpacked`, `continue`, `ignore`,
  `min_written`), `save_memory`; `Memory::trackWrites`, `Cpu::watchWrittenCode`.
- **CPC-Disk-Abbilder** (`z80/dsk.hpp`, `z80tool dsk info|ls|extract|boot|sector`, Befehl `dsk_ls`, `load_file … name`): Standard- und
  Extended-DSK, AMSDOS-Dateisystem (DATA, SYSTEM, IBM), mehrere Extents, Attribute, Bootsektor. **AMSDOS-Kopf** (`parseAmsdosHeader`,
  `makeAmsdosHeader`): `asm --amsdos/--exec` schreibt ihn, `dis`, `monitor` und `load_file` laden an die Ladeadresse (PC = Einsprung).
- **Assembler-Optimierung** `-O` (Protokoll `optimize`): `JP`/`JP NZ,Z,NC,C` → `JR` in Reichweite; Test mit 3000 Zufallsprogrammen.
- Orchestrator: Werkzeuge **`remove_annotation`** und **`mark_constant`**; `run_routine` mit **`setup`** (Routinen vorher auf derselben Maschine),
  Speicher als `text`, ehrlichem `returned`, Hinweis auf **leeren Speicher**, ASCII-Spalte, Namen der betroffenen Variablen, Hinweisen auf
  Schreibzugriffe hinter das Programmende und in Code, Aufrufstelle der Firmware-Aufrufe; `mark_data kind=text` ohne Länge; erkannte Datenbereiche
  und vorgeschlagene Zeigertabellen werden selbst eingetragen (`--no-auto-data`, `--no-auto-tables`); **`--dry-run`**; `--dsk IMAGE --dsk-file NAME`;
  Auftragstext mit Blöcken („in 2 parts“), Referenzen und „reached through the jump table“; die Vollständigkeitsprüfung verlangt `inputs`/`outputs`/
  `clobbers`/`confidence` und höchstens einen Kommentar je zwei Befehle; `build_parser()`.
- **`agent/z80_drive.py`**: dieselben Werkzeuge von Hand bedient (ein Schritt je Aufruf, Zustand in einem Arbeitsverzeichnis, strenges `finish`,
  Screenshots als Dateien).
- Beispielprogramm `examples/cpc/demo_cpc.asm` mit Integrationstest über die ganze Kette, Anleitung [docs/walkthrough.md](docs/walkthrough.md).
- Server: Protokoll 4; `analyze {from, to}` und der Programmbereich aus `asm`/`load_file`; Listing bis zum Programmende, Namen im ganzen Programm
  (auch außerhalb eines Ausschnitts), `equates:false`; Läufe melden `fetched_from_empty_memory`; `firmware_log` mit `call_site`.
- Tests `analysis`, `unpack`, `dsk`, `optimize`; der Robustheitstest schickt dem Server Tausende zufälliger Befehle.

- Gefunden beim Kommentieren eines echten Spiels (Sorcery, Amstrad CPC):
  - Die Analyse verfolgt **selbst installierte Interrupt-Handler** (`LD HL,x` … `LD (&0039),HL`, auch über DE/BC) als eigene Routinen.
  - Eingebettete Daten hinter einem `CALL` (Inline-Strings) erscheinen im Listing als Text (`DB "TODAY'S GREATEST SORCERER",'S'+&80`).
  - Assembler: **`ORG adresse,ablage`** assembliert für eine Adresse und legt die Bytes an einer anderen ab (Code, den ein Lader verschiebt).
  - `asm --exec` nimmt auch ein Label.
  - **Disk-Abbilder schreiben:** `z80tool dsk new` (leere DATA- oder SYSTEM-Diskette) und `dsk put` (Datei ins AMSDOS-Dateisystem,
    mehrere Extents, Attribute, Ersetzen); `DskImage::createFormatted`, `writeFile`, `deleteFile`, `freeBytes`, `serialize`.

### Geändert
- `max_instructions` höchstens 100 Millionen; `0` (ohne Befehlslimit) nur zusammen mit `max_cycles`.
- `ann_remove` entfernt auch einen Datenbereich und eine Konstanten-Markierung; `name+versatz` ab 10 hexadezimal (`spieler+&26`).
- `call` meldet bei der Rücksprungadresse mit falschem Stapelzeiger einen `note`.

### Behoben (Sorcery)
- Das Listing schrieb hinter Portwerten (`LD BC,PPI_A+&0E`) eine Bildschirmadress-Erklärung.

### Behoben
- `ann_load` stürzte bei einer Projektdatei ab, deren Eintrag kein `address`/`length` hatte (gefunden vom neuen Robustheitstest);
  `Json::asInt` für Zahlen außerhalb des 64-Bit-Bereichs.

### Behoben
- Orchestrator: Ein ungültiger Tool-Aufruf des Modells (Ollama `HTTP 500 invalid character 'x' …`, z. B. nackte `0x0C5A`) beendete
  den ganzen Lauf. Jetzt: Hinweis ans Modell und neue Anfrage; ein Serverfehler bei einer Routine überspringt nur diese;
  erst drei in Folge (oder ein Fehler vor der ersten Antwort) beenden den Lauf. Der Prompt verlangt Hexwerte als Strings.
- Die Ausdehnung einer Routine endete am ersten unbedingten Sprung: bei einer Hauptschleife sah das Modell nur den ersten Teil
  (137 von etwa 250 Bytes). Sie umfasst jetzt alles, was die Sprünge erreichen (Aufrufe ausgenommen).
- Orchestrator: Rundenlimit trotz fast fertiger Arbeit zählte als Fehlschlag; jetzt „partial“, Warnung vor den letzten Runden,
  Hinweis, mehrere Tool-Aufrufe je Antwort zu schreiben; Prompt-Fakten zu CPC-Bildschirmmodi und Tabellen-Basisadressen.
- Kommentare an Adressen mitten in Befehlen wurden stillschweigend verworfen: das Modell-Listing hat jetzt eine Adressspalte,
  `ann_comment` lehnt solche Adressen mit Erklärung ab, `ann_data` lehnt Bereiche über Code ab.
- Ein Benutzerkommentar verdeckte die automatische Hardware-Erklärung; beide werden jetzt gezeigt.
- `run_routine`: Port-Schreibzugriffe nennen den schreibenden Befehl (`pc`) und ob er zur Routine gehört.

### Hinzugefügt
- **Mehrere Durchgänge** im Orchestrator (`--refine-model`, `--report`, …): schnelles Modell zuerst, weitere Modelle
  prüfen und ergänzen nur Routinen mit gemessenen Lücken; Fähigkeitserkennung über Ollama `/api/show`
  (Tools, Vision, Thinking); `confidence`-Feld (high/medium/low) in der Routinendokumentation.
- Listing: Erklärung von Idiomen (`LD SP,nn`, Interrupt-Vektor-Installation, `HALT`, `IM`), Zähler `user_comments`.
- Orchestrator: einmalige Vollständigkeitsprüfung vor `finish_routine`; `run_routine` fasst Änderungen im
  Bildschirmspeicher zusammen und erklärt, dass ein nicht zurückkehrender Einstiegspunkt normal ist.

### Behoben
- Orchestrator: ausufernder „Thinking“-Modus füllte das Kontextfenster (kein Tool-Aufruf mehr). Jetzt
  `think=false` als Standard (mit Rückfall für Modelle ohne Thinking), Abtastung nach Qwen-Empfehlung,
  Token-Limit pro Antwort, Erkennung abgeschnittener Antworten; Prompt erklärt `OUT (C),r` (Port = BC).

### Hinzugefügt
- Echte CPC-6128-Firmware läuft: Kaltstart bis „BASIC 1.1 Ready“ (PPI-Vsync, 300-Hz-Interrupts), Test in der CI
  (ROM wird dort geladen, nicht eingecheckt). Orchestrator: `--boot`, `--irq`, tolerante Argumentverarbeitung,
  Aufrufe als Text, Wiederholungserkennung, Kontextverdichtung.
- **Kommentieren mit lokalem LLM**: `agent/z80_annotate.py` (Ollama, Tool-Calling, Screenshots als Bilder),
  Annotationsmodell (`z80::Annotations`), **verifiziertes** Listing (`renderListing`: Namen, Routinenköpfe,
  Datenregionen, `EQU`-Blöcke; wird neu assembliert und mit dem Speicher verglichen).
- **Amstrad CPC** (`z80::cpc`): 231 Firmware-Symbole (aus zwei Quellen abgeglichen, `tools/gen`), Hardware-Ports
  mit Erklärung der Zugriffe, Videohardware (Gate Array/CRTC), Rendering der Modi 0–2 als PNG (eigener
  Encoder), Bildschirm-Vergleich, `.sna`-Header, ROM-Überlagerung, Firmware-Stubs mit Protokoll.
- Session-Befehle `machine`, `load_sna`, `load_rom`, `video`, `screen`, `screen_diff`, `analyze`, `routines`,
  `listing`, `ann_*`, `firmware_log`, `console`; Checkpoints sichern den Videozustand; `irq_every_cycles`.
- **Monitor** (`z80tool monitor`, Klasse `z80::Monitor`): Disassemblieren, Speicherbefehle, zeilenweises
  Assemblieren, Register, Einzelschritt/Step over/bis Rückkehr/Lauf, Haltepunkte, Watchpoints,
  Checkpoints, Symbole, Dateibefehle; Zahlen standardmäßig hexadezimal.
- `ExpressionEvaluator`: optionaler Hex-Standard; `AssemblerOptions::predefined` (vordefinierte Symbole).
- **Z80-Emulator** (`z80::Cpu`): alle dokumentierten und die verbreiteten undokumentierten Befehle,
  Flags 3/5 und `MEMPTR`, T-States pro Befehl, Interrupts (IM 0/1/2, NMI), Port-Handler.
  Besteht ZEXDOC und ZEXALL (67/67).
- Debugger-Funktionen: Einzelschritt, `run`/`runUntil`/`runUntilReturn`/`stepOver`, Breakpoints,
  Watchpoints, Befehls- und Zyklenlimits, Trace-Hook.
- Zustand: `CpuState`, `Snapshot` (Checkpoints im Speicher, Snapshot-Datei `Z80EMUS1`), CPU-Zustand
  als JSON, Registerzugriff per Name.
- **`z80tool serve`**: JSON-Zeilenprotokoll für LLM-Agenten (Assemblieren, Ausführen, `call`, `diff`,
  Checkpoints, Ein-/Ausgabe, …); Dateizugriff nur mit `--allow-files`.
- `tools/zex_runner.cpp` und CI-Job für ZEXDOC/ZEXALL; Tests `cpu`, `session`.

## [0.1.0]

Erste C++-Version: Portierung der beiden BASIC-Programme (Z80-Assembler, Z80-Disassembler)
in ein gemeinsames Projekt.

### Hinzugefügt
- **Assembler** (`z80::Assembler`): Mehrpass-Auflösung von Labels und Vorwärtsreferenzen,
  Ausdrücke (`+ - * / % << >> & | ^ ~`, `$`, `HIGH()`, `LOW()`), alle dokumentierten Z80-Befehle,
  Pseudobefehle `ORG EQU END DB DW DM DS`, mehrere `ORG`-Segmente, Listing und Symboltabelle.
- **Disassembler** (`z80::Disassembler`): Dekoder für alle dokumentierten Befehle, Ablaufanalyse
  („Kettenmodus“) und linearer Durchlauf, Querverweise (Aufrufer von Sprung-/Callzielen),
  Kommentardatei, Inline-Daten nach CALL (z. B. Strings), Ausgabe als Listing oder als
  wieder assemblierbarer Quelltext.
- **Kommandozeilenwerkzeug** `z80tool` mit den Unterbefehlen `asm` und `dis`; Ein-/Ausgabe von
  Raw-Binaries, CPC-`.sna`-Snapshots und Intel HEX.
- Tests (7 Suiten, u. a. exhaustiver Dekoder↔Assembler-Roundtrip und Property-Test
  `assemble(disassemble(x)) == x`), CI (Linux, macOS, Windows, Sanitizer, Kreuzprüfung gegen
  `z80asm`), Dokumentation in `docs/`.

### Geändert
- Agenten-Protokoll `set`: Setzen von `PC` beendet einen laufenden `HALT`.

### Behoben (gegenüber den BASIC-Originalen)
Siehe [docs/migration-from-basic.md](docs/migration-from-basic.md).
