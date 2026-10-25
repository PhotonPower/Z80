# Assembler-Syntax

## Zeilenformat

```
[Label[:]]   [Befehl [Operanden]]   [; Kommentar]
```

- Groß-/Kleinschreibung ist egal (Befehle, Register, Labels, Hexziffern).
- Ein Wort in der **ersten Spalte**, das kein Befehl ist, definiert ein Label; der Doppelpunkt ist
  optional. Mit Doppelpunkt darf das Label an beliebiger Stelle beginnen: `loop: DJNZ loop`.
- Befehle dürfen eingerückt sein oder in Spalte 1 stehen.
- Kommentare beginnen mit `;` (außer innerhalb von `"…"` und `'x'`).
- Operanden werden mit Komma getrennt; Leerzeichen um das Komma sind erlaubt.

## Labels

- Zeichen: Buchstaben, Ziffern, `_ . @ ?`; das erste Zeichen ist keine Ziffer. Die Länge ist
  unbegrenzt (das BASIC-Original kürzte auf 6 Zeichen).
- Nicht erlaubt sind Registernamen (`A B C D E H L I R AF BC DE HL SP IX IY`), Bedingungen
  (`NZ Z NC PO PE P M`) und Mnemonics (`SUB`, `AND`, `OR`, … – ein Label namens `sub` wird
  abgelehnt).
- Vorwärtsreferenzen sind erlaubt; der Assembler läuft so oft über den Quelltext, bis alle
  Symbole stabil sind (`EQU` darf ebenfalls vorwärts verwendet werden).
- Doppelt definierte Labels sind ein Fehler.

## Zahlen und Ausdrücke

| Schreibweise            | Beispiel                | Hinweis                                              |
|-------------------------|-------------------------|------------------------------------------------------|
| dezimal                 | `255`                   |                                                      |
| hexadezimal             | `&FF` `&HFF` `$FF` `#FF` `0xFF` `0FFh` | `…h` muss mit einer Ziffer beginnen (`0FFh`, nicht `FFh`) |
| binär                   | `%1010` `0b1010` `1010b` |                                                      |
| Zeichen                 | `'A'`                   |                                                      |
| aktuelle Adresse        | `$`                     | Adresse des **Befehlsanfangs**; `$` + Hexziffer ist dagegen eine Hexzahl (`$FF`) |

Operatoren (Präzedenz von niedrig nach hoch): `|`  `^`  `&`  `<< >>`  `+ -`  `* / %`  unär `+ - ~`,
dazu Klammern und die Funktionen `HIGH(x)`, `LOW(x)`.

> **`&` doppeldeutig:** Am Anfang eines Operanden ist `&` das Hex-Präfix (`&FF`), zwischen zwei
> Operanden das bitweise UND. Schreibe `A & &FF` mit Leerzeichen, nicht `A&FF`.

Die Wertebereiche werden geprüft: 8-Bit-Operanden `-128…255`, 16-Bit `-32768…65535`,
Index-Verschiebung `-128…127`.

## Befehle

Alle **dokumentierten** Z80-Befehle (siehe Zilog Z80 CPU User Manual). Besonderheiten:

- Akkumulator-Arithmetik darf mit oder ohne `A,` geschrieben werden: `ADD A,B` = `ADD B`,
  `SUB B`, `AND &0F`, `CP (HL)`.
- Indizierte Adressierung: `(IX+5)`, `(IY-3)`, `(IX)`; `JP (IX)`, `ADD IX,BC`, `LD IX,&1234`, …
- Relative Sprünge nehmen eine **Zieladresse**: `JR loop`, `DJNZ $`, `JR NZ,$+5`. Die Entfernung
  wird geprüft (`-128…+127` ab der Folgeadresse); sonst Fehler `relative jump out of range`.
  `JR` kennt nur die Bedingungen `NZ Z NC C`.
- `RST` nimmt die Zieladresse: `RST &38`, `RST 0` (nur `0, 8, 16, …, 56`).
- `EX AF,AF'` – der Apostroph gehört zum Registernamen.
- `IN A,(n)`, `IN r,(C)`, `OUT (n),A`, `OUT (C),r`, `IM 0|1|2`.

**Nicht** unterstützt (undokumentiert): `SLL`/`SLI`, `IXH IXL IYH IYL`, `IN F,(C)`, `OUT (C),0`,
Kopien der Rotationsbefehle mit Zielregister (`RLC (IX+d),B`).

## Pseudobefehle

| Befehl                         | Wirkung                                                                      |
|--------------------------------|------------------------------------------------------------------------------|
| `ORG adr`                      | Programmzähler setzen; mehrfach erlaubt (jedes `ORG` beginnt ein neues Segment) |
| `label EQU wert`               | Konstante definieren                                                         |
| `DB`, `DEFB` `w1,w2,"text",…`  | Bytes; Strings in `"…"` werden zeichenweise eingesetzt                         |
| `DW`, `DEFW` `w1,w2,…`         | 16-Bit-Wörter, niederwertiges Byte zuerst                                      |
| `DM`, `DEFM` `"text"`          | wie `DB` (für Texte)                                                         |
| `DS`, `DEFS` `anzahl[,fuell]`  | `anzahl` Bytes reservieren, gefüllt mit `fuell` (Standard `0`)                 |
| `END`                          | Assemblierung beenden; Zeilen danach werden ignoriert                          |

## Beispiel

```asm
; Text ausgeben
TXT_OUTPUT  EQU &BB5A
            ORG &4000

start:      LD   HL,message
next:       LD   A,(HL)
            OR   A
            RET  Z
            CALL TXT_OUTPUT
            INC  HL
            JR   next

message:    DB   "Hello, Z80!",13,10,0
```

## Fehlermeldungen

Format `datei:zeile: error: meldung`, gefolgt von der betroffenen Zeile. Häufige Meldungen:

| Meldung                                   | Ursache                                                       |
|-------------------------------------------|---------------------------------------------------------------|
| `unknown mnemonic 'xyz'`                  | Befehl unbekannt oder in eingerückter Zeile ein Label ohne `:` |
| `invalid operands for LD`                 | Operandenkombination existiert nicht (z. B. `LD (HL),(HL)`)     |
| `undefined symbol 'NAME'`                 | Label/Konstante nirgends definiert                              |
| `duplicate label 'x'`                     | Label zweimal definiert                                         |
| `'x' is a reserved word …`                | Label heißt wie Register, Bedingung oder Befehl                 |
| `value out of 8-bit range: 300`           | Operand passt nicht in ein Byte                                 |
| `relative jump out of range (200)`        | `JR`/`DJNZ`-Ziel zu weit weg                                    |
