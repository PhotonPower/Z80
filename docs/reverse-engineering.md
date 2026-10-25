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
| `name_label`, `document_routine`, `name_variable`, `mark_data`, `add_comment` | **Namen und Kommentare** festhalten |
| `finish_routine` | Routine abschließen |

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
| `--sna DATEI` / `--bin DATEI --org 0x4000` | Eingabe |
| `--entry ADR` (mehrfach) | Einsprungpunkte (Standard: PC aus dem Snapshot) |
| `--inline-string ADR` (mehrfach) | Routinen, hinter deren `CALL` ein Text steht (letztes Zeichen Bit 7) |
| `--rom DATEI:lower\|upper\|both` | echte Firmware benutzen statt der Stubs (siehe unten) |
| `--project DATEI` | Annotationen; wird geladen, falls vorhanden, und nach **jeder Routine** gespeichert → Abbrechen und Fortsetzen ist möglich |
| `--out DATEI` | fertiges, verifiziertes Listing |
| `--only ADR` | nur diese Routine bearbeiten (zum Ausprobieren) |
| `--model`, `--host`, `--think true\|false`, `--num-ctx`, `--max-turns` | Modell und Grenzen |
| `--lang` | Sprache der Kommentare |
| `--log DATEI` | jede Modellnachricht und jedes Tool-Ergebnis (JSON-Zeilen), zum Nachvollziehen |

Rückgabewert: `0`, wenn das Listing verifiziert ist; `1`, wenn nicht; `2` bei Verbindungsproblemen.

Ablauf: Analyse ab den Einsprungpunkten → Routinen **von den Blättern zur Wurzel** (zuerst, was nichts
Undokumentiertes aufruft), je Routine eine eigene Unterhaltung mit dem Listing, den Namen der bereits
dokumentierten Aufgerufenen und den Werkzeugen → Annotationen speichern → am Ende das verifizierte Listing.
Eine zweite Ausführung mit demselben `--project` fragt das Modell nur noch nach noch offenen Routinen.

## Was im Listing automatisch passiert

- **Firmware**: `CALL &BB5A` → `CALL TXT_OUTPUT`, dazu der `EQU`-Block `TXT_OUTPUT EQU &BB5A`
  (231 Einträge der Sprungleiste, siehe `tools/gen/gen_cpc_data.py`).
- **Hardware**: `LD BC,&7F8D` + `OUT (C),C` → `LD BC,GATE_ARRAY+&8D` mit dem Kommentar
  *Gate Array: screen mode 1, lower ROM off, upper ROM off*. Erkannt werden Gate Array, CRTC, ROM-Auswahl,
  Drucker, PPI und Disc-Ports, auch bei `LD B,n` / `LD A,n` / `OUT (C),A` (der Wert in `A` wird verfolgt).
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
Zeichensatz), Tastaturabfragen liefern nichts.

Mit `--rom os6128.rom:lower` (und z. B. `basic.rom:upper`) läuft die echte Firmware. Die ROMs blendet
das Gate Array wie auf der Hardware ein und aus. Du brauchst dafür deine eigenen ROM-Abbilder.

## Qwen3-VL über Ollama – Hinweise

- Das Skript ist gegen die dokumentierte Ollama-Schnittstelle (`/api/chat` mit `tools`, Bilder in
  `images`) geschrieben und gegen einen **Mock-Server** getestet (`tests/agent`). **Mit einem echten
  Qwen3-VL habe ich es noch nicht ausprobiert** – bitte rechne mit etwas Feinschliff an
  Prompt und Parametern.
- Größere Modelle machen deutlich weniger Fehler bei Tool-Aufrufen und Hardware-Wissen. Der Lauf
  braucht je Routine mehrere Runden; `--max-turns` begrenzt sie.
- `--num-ctx` (Standard 16384) muss zum Modell passen; Bilder verbrauchen Kontext (ein zugeschnittener
  Screenshot ist klein, ein Vollbild nicht).
- Wenn das Modell lange „denkt“ und die Tool-Aufrufe darunter leiden, probiere `--think false`.
- Beobachte `--log`: Dort siehst du, welche Tests das Modell gemacht und was es daraus geschlossen hat.

## Grenzen

- Nur flache 64 KiB (128K-Banking der CPC 6128 fehlt), nur Modi 0–2, kein Blinken, kein CRTC-Timing,
  keine Tastatur/Joystick/Sound/Floppy.
- Interrupts werden nur auf Wunsch periodisch ausgelöst (`irq_every_cycles`, ca. 13 300 T-States
  für 300 Hz); der Orchestrator nutzt das derzeit nicht.
- Die Analyse folgt Sprüngen statisch; Code, der nur über berechnete Sprünge (`JP (HL)`) erreicht wird,
  muss als weiterer `--entry` angegeben werden.
- Die Qualität der Namen und Kommentare hängt am Modell. Das Verfahren stellt nur sicher, dass sie das
  Programm nicht verändern und dass das Modell seine Behauptungen am laufenden Code prüfen *kann*.

## Eigene Anbindung

Alles, was der Orchestrator tut, geht über das JSON-Protokoll von `z80tool serve`
([agent-protocol.md](agent-protocol.md)); ein anderes Framework (MCP, eigene Schleife) kann dieselben
Befehle verwenden.
