# Monitor

`z80tool monitor` ist ein klassischer Maschinensprache-Monitor mit Eingabeschleife: Speicher
ansehen und ändern, disassemblieren, Zeilen assemblieren, Programme im Emulator einzeln oder bis
zu einem Haltepunkt ausführen. Alles basiert auf den Bausteinen der Bibliothek (`z80::Cpu`,
Dekoder, Assembler); die Klasse `z80::Monitor` hat keine eigene Ein-/Ausgabe, sodass sie sich auch
direkt testen oder in andere Oberflächen einbauen lässt.

```sh
z80tool monitor                          # leerer Speicher
z80tool monitor spiel.sna                # CPC-Snapshot laden
z80tool monitor prog.bin --org 4000      # Raw-Binary an Adresse 4000 (hex) laden
z80tool monitor --no-files               # Dateibefehle sperren
z80tool monitor < befehle.txt            # Skript: ohne Banner und Prompt
```

Ohne Zeileneditor (kein `readline`-Verlauf); mit `rlwrap z80tool monitor` bekommt man Pfeiltasten
und Verlauf.

## Zahlen

**Argumente sind hexadezimal**, wie bei klassischen Monitoren: `d 4000`, `e 100 3E 05`.

| Schreibweise | Bedeutung |
|---|---|
| `4000`, `FF`, `BEEF` | hexadezimal (auch wenn mit einem Buchstaben begonnen wird) |
| `100.` | **dezimal** (Punkt dahinter) |
| `&FF`, `$FF`, `0xFF`, `0FFh`, `%1010` | mit Präfix/Suffix wie im Assembler |
| `start`, `loop+3` | Symbole und Ausdrücke (`+ - * / & \| ^ << >>`, Klammern) |
| `@HL`, `@PC`, `@SP` | Registerinhalt |

Ein bekanntes Symbol hat immer Vorrang: gibt es ein Symbol `DEAD`, ist `DEAD` dieses Symbol und nicht
die Zahl 0xDEAD (`sym -dead` entfernt es wieder).

**Im Assemble-Modus** (Befehl `a`) gilt die Syntax des Assemblers, nicht die des Monitors:
`LD A,10` ist dezimal, hex schreibt man `&0A` oder `0Ah` ([assembler-syntax.md](assembler-syntax.md)).

## Befehle

| Befehl | Wirkung |
|---|---|
| `d [adr [n]]` | disassemblieren (n Befehle, Standard 12; ohne Adresse: Fortsetzung). `>` markiert den PC, Marken stehen als `NAME:` davor |
| `m [adr [n]]` | Speicher als Hex + ASCII (n Bytes, Standard 128; ohne Adresse: Fortsetzung) |
| `e adr b…` | Bytes schreiben; auch Text: `e 200 "Hallo" 0` |
| `f von bis b…` | füllen (Muster wird wiederholt) |
| `c von bis ziel` | vergleichen (zeigt bis zu 32 Unterschiede) |
| `t von bis ziel` | kopieren (überlappend korrekt) |
| `h von bis b…` | suchen: Bytes oder `"Text"` (bis zu 64 Treffer) |
| `a [adr]` | **Zeilen assemblieren**; leere Zeile beendet. Labels (`loop:`) und frühere Symbole sind verwendbar |
| `r [name=wert …]` | Register anzeigen / setzen (`r pc=4000 a=7 hl=@sp`); PC setzen beendet einen `HALT` |
| `n [anzahl]` | Einzelschritt(e); bei 2–20 Schritten wird jeder Befehl gezeigt |
| `o` | Step over: ein `CALL`/`RST` läuft bis zur Rückkehr |
| `u` | läuft, bis die aktuelle Routine zurückkehrt |
| `g [adr]` | ab `adr` (oder PC) laufen lassen bis Haltepunkt / `HALT` / Limit |
| `ru adr` | laufen, bis `PC == adr` |
| `b [adr…]` | Haltepunkte setzen; ohne Argument: Haltepunkte und Watchpoints auflisten |
| `w adr [r\|w\|rw]` | Watchpoint auf Lesen/Schreiben/beides |
| `bc [adr…]` | Haltepunkte (und Watchpoints) löschen; ohne Argument alle |
| `limit n` | höchstens n Befehle pro Lauf (Standard 10 000 000) – schützt vor Endlosschleifen |
| `sym [name wert \| -name]` | Symbole anzeigen, setzen, löschen |
| `symload datei` / `symsave datei` | Symboldateien (Format der Assembler-Ausgabe `-s`) |
| `asm datei [org]` | Quelltext-Datei assemblieren und in den Speicher laden; Symbole werden übernommen |
| `l datei [adr]` | Binärdatei an `adr` laden; `.sna` ohne Adresse ersetzt das RAM |
| `sv datei von bis` | Speicherbereich speichern |
| `ck name` / `rb name` | Checkpoint (CPU + RAM) anlegen / zurückspielen |
| `savestate datei` / `loadstate datei` | Snapshot-Datei (`Z80EMUS1`) |
| `int [byte]`, `nmi` | Interrupt / NMI auslösen |
| `input b…`, `io` | Bytes für `IN` bereitstellen; `OUT`-Protokoll anzeigen |
| `reset [mem]` | CPU zurücksetzen (mit `mem` auch RAM löschen) |
| `?`, `q` | Hilfe, Beenden |

Fehler erscheinen als `? Meldung` und ändern den Zustand nicht (bei mehreren Argumenten wie
`r a=1 zz=2` kann der erste Teil schon gesetzt sein).

## Beispiel

Eingabe (als Skript):

```
sym twice 400D
a 4000
start: LD B,5
XOR A
loop: ADD A,B
DJNZ loop
LD (&4010),A
CALL twice
HALT
twice: ADD A,A
RET

d start 5
b twice
g start
u
m 4010 1
q
```

Ausgabe:

```
ok
assemble mode: type assembler lines (numbers like &1F or 31), empty line to finish
  4000  06 05        LD B,&05
  4002  AF           XOR A
  4003  80           ADD A,B
  4004  10 FD        DJNZ LOOP
  4006  32 10 40     LD (&4010),A
  4009  CD 0D 40     CALL TWICE
  400C  76           HALT
  400D  87           ADD A,A
  400E  C9           RET
START:
  4000  06 05        LD B,&05
  4002  AF           XOR A
LOOP:
  4003  80           ADD A,B
  4004  10 FD        DJNZ LOOP
  4006  32 10 40     LD (&4010),A
ok
stopped: breakpoint at 400D  [14 instructions, 121 T-states]
 PC=400D SP=FFFD  AF=0F08 BC=0000 DE=0000 HL=0000 IX=0000 IY=0000
 AF'=0000 BC'=0000 DE'=0000 HL'=0000  I=00 R=0E  IM0 DI  ....X...
  400D  87           ADD A,A
stopped: returned  [2 instructions, 14 T-states]
 PC=400C SP=FFFF  AF=1E18 BC=0000 DE=0000 HL=0000 IX=0000 IY=0000
 AF'=0000 BC'=0000 DE'=0000 HL'=0000  I=00 R=10  IM0 DI  ...HX...
  400C  76           HALT
4010  0F                                                |.|
```

Der Ablauf: Programm eintippen, Haltepunkt auf die Routine, laufen lassen (A = 15 nach der
Schleife), mit `u` bis zur Rückkehr (A = 0x1E), das Ergebnis im Speicher ansehen.

## Grenzen

- Beim zeilenweisen Assemblieren sind **Vorwärtsreferenzen nicht möglich** (eine Zeile kennt nur
  Symbole, die schon definiert sind). Der Monitor weist darauf hin; mit `sym name adr` legt man
  die Adresse vorher fest, oder man assembliert eine ganze Datei mit `asm`.
- Eine Zeile mit nur einem Wort ohne `:` im Assemble-Modus ist ein Fehler (Tippfehlerschutz),
  obwohl der Assembler sie als Label akzeptieren würde.
- Es gibt keine Befehlsverlauf-/Zeilenbearbeitung im Programm selbst (siehe `rlwrap`).
- Die Ausführung kennt nur flache 64 KiB; Geräte und Firmware sind nicht nachgebildet
  ([emulator.md](emulator.md#genauigkeit)).
