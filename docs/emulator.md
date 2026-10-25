# Z80-Emulator

Der Emulator (`z80::Cpu`) ist ein befehlsgenauer Z80-Kern mit Debugger-Funktionen. Er ist als
Baustein für den Monitor und für Agenten gedacht, die Z80-Code ausführen und untersuchen.

## Genauigkeit

- **ZEXDOC und ZEXALL bestehen** (je 67 Testgruppen, 0 Fehler; ca. 5,8 Milliarden Befehle, rund
  eine Minute). ZEXALL prüft zusätzlich die undokumentierten Flag-Bits 3 und 5; die interne
  `MEMPTR`/`WZ`-Logik ist umgesetzt.
- Alle dokumentierten und die verbreiteten **undokumentierten Befehle** werden ausgeführt:
  `SLL`, `IXH/IXL/IYH/IYL`, `IN F,(C)`, `OUT (C),0`, die Register-Kopien der indizierten
  `CB`-Befehle, `ED`-Aliase (`NEG`, `RETN`, `IM`), doppelte Präfixe.
- Zeitverhalten: **T-States pro Befehl** nach Zilog-Tabelle (z. B. `LD A,(IX+d)` = 19,
  `LDIR` = 21/16, `DJNZ` = 13/8). Keine Wait-States, keine Speicher-Contention, kein
  Zyklus-genaues Bus-Timing innerhalb eines Befehls.
- Nicht nachgebildet: das `Q`-Flag-Verhalten von `SCF/CCF`, Sonderfälle der Flags bei wiederholten
  Block-Ein-/Ausgabebefehlen und der Interrupt-Sonderfall bei `LD A,I`/`LD A,R`. (ZEXALL besteht
  trotzdem.) Interrupt-Modus 0 unterstützt nur `RST`-Befehle auf dem Datenbus.

Der Test ist reproduzierbar (`tools/zex_runner.cpp`, siehe [testing.md](testing.md)).

## Aufbau

```cpp
#include "z80/cpu.hpp"

z80::Memory memory;            // 64 KiB, gehört dem Aufrufer
z80::Cpu cpu(memory);          // Power-on: PC=0, SP=0xFFFF, AF=0xFFFF, Interrupts aus

memory.load(0x4000, code);
cpu.regs().pc = 0x4000;

cpu.step();                    // ein Befehl, liefert T-States
auto r = cpu.run();            // bis Breakpoint / Halt / Limit
```

Die `Cpu` besitzt den Speicher nicht; mehrere Instanzen können sich einen `Memory` teilen
oder ihn austauschen (`setMemory`).

### Registersatz

`Registers` enthält `main` und `alt` (Reihenfolge `B C D E H L F A`), `ix iy sp pc wz`, `i r im`,
`iff1 iff2 halted`. Zugriff über Hilfsfunktionen (`af()`, `setHl()`, …) oder per Name:

```cpp
z80::setRegister(cpu.regs(), "HL", 0x4000);   // "A".."L", "AF" … "AF'", "IXH", "IFF1", "PC", …
auto a = z80::getRegister(cpu.regs(), "A");   // std::optional<uint32_t>
```

### Ausführen

| Aufruf | Verhalten |
|---|---|
| `step()` | ein Befehl (oder Interrupt-Annahme / Leerlauf bei `HALT`); gibt T-States zurück |
| `stepInstruction()` | wie `step()`, aber mit `RunResult` (meldet `Halted`/`Watchpoint`) |
| `run(limits)` | bis Breakpoint, Watchpoint, `HALT` ohne Aufweckmöglichkeit oder Limit |
| `runUntil(addr, limits)` | bis `PC == addr` (Befehl dort noch **nicht** ausgeführt) |
| `runUntilReturn(limits)` | bis die aktuelle Routine zurückkehrt (nach dem `RET`, das über den Start-`SP` hinaus abräumt; verschachtelte Aufrufe zählen nicht) |
| `stepOver(limits)` | ein Befehl; bei `CALL`/`RST` bis zur Rückkehr (Breakpoints in der Routine halten an) |

`RunLimits` begrenzt Befehle (Standard 1 000 000) und T-States (Standard unbegrenzt). **Ein Limit
zu setzen ist wichtig, wenn fremder Code läuft**, sonst läuft eine Endlosschleife ewig.

`RunResult` enthält `reason` (`Stepped`, `Breakpoint`, `Address`, `Returned`, `Halted`,
`InstructionLimit`, `CycleLimit`, `Watchpoint`), die Anzahl Befehle/Zyklen des Aufrufs, `pc` und bei
`Watchpoint` die Adresse und Art des Zugriffs. Steht `PC` beim Start auf einem Breakpoint, wird
dieser übersprungen (sonst käme man nie weiter).

### Debug-Hilfen

- `addBreakpoint/removeBreakpoint/clearBreakpoints`
- `addWatchpoint(adresse, WatchKind::Read|Write|Access)`: hält **nach** dem Befehl an, der zugriff
- `setTrace(callback)`: wird vor jedem Befehl gerufen
- `setPortHandlers(in, out)`: `IN`/`OUT` und Block-E/A; ohne Handler liefert `IN` `0xFF`

### Interrupts

`requestInterrupt(byte)` setzt die INT-Leitung (wird bei `IFF1` und nach der Schutzfrist von `EI`
angenommen; IM 0: `RST`-Opcode, IM 1: `0x38`, IM 2: Vektor aus `I`·256+Byte). `requestNmi()` springt
nach `0x66`. Ein `HALT` wird dadurch beendet.

## Zustand speichern und laden

```cpp
#include "z80/snapshot.hpp"

z80::Snapshot cp = z80::captureSnapshot(cpu);     // CpuState + 64 KiB, im Speicher
...
z80::restoreSnapshot(cpu, cp);                    // zurück zum Checkpoint

auto bytes = z80::serializeSnapshot(cp);          // Dateiformat "Z80EMUS1"
auto back  = z80::deserializeSnapshot(bytes);     // wirft bei beschädigter Datei

z80::Json j = z80::stateToJson(cpu.state());      // nur Prozessor, lesbar
cpu.setState(z80::stateFromJson(j));
```

- **Checkpoints** sind der Weg, Code „auszuprobieren“: sichern, ausführen, ansehen, zurückrollen.
- Ein zurückgespielter Snapshot setzt die Ausführung **bit-identisch** fort (getestet mit
  Zufallscode).
- Dateiformat `Z80EMUS1`: 8 Byte Kennung, Registerblock (little-endian), Zähler, danach 65 536 Byte
  Speicher; Gesamtgröße 65 591 Byte. Eine ungültige Kennung, falsche Größe oder ein Interrupt-Modus
  > 2 wird abgelehnt.
- JSON: 16-Bit-Werte als `"0x1234"`, beim Einlesen sind auch Zahlen und `"&1234"`, `"$1234"` erlaubt.

## Für Agenten

`z80tool serve` stellt all das als zeilenweises JSON-Protokoll bereit:
[agent-protocol.md](agent-protocol.md).
