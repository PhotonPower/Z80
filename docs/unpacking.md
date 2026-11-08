# Gepackte Programme untersuchen

Viele CPC-Programme sind **gepackt** (gecruncht): Der Anfang ist ein kleines Entpackprogramm, dahinter liegen die gepackten Daten. Beim Start entpackt
es das eigentliche Programm in den Speicher und springt hinein. Untersucht werden soll das **entpackte** Programm; dessen Listing ist das Ziel.
Der Entpacker selbst wird dabei **nicht verstanden oder nachgebaut**: Er läuft einfach im Emulator.

## 1. Das Programm von der Diskette holen

Ein Disk-Abbild (`.dsk`, „MV - CPCEMU“ oder „EXTENDED CPC DSK“) enthält entweder das **AMSDOS-Dateisystem** (Formate DATA, SYSTEM/VENDOR, IBM)
oder ist kopiergeschützt bzw. hat einen eigenen Lader.

```sh
z80tool dsk info    spiel.dsk                 # Format, Spuren, Sektor-IDs der Spur 0, Zahl der Dateien
z80tool dsk ls      spiel.dsk [-a]            # Dateien mit Größe und AMSDOS-Kopf (Typ, Lade-, Einsprungadresse); -a auch Systemdateien
z80tool dsk extract spiel.dsk SPIEL.BIN       # herauskopieren (der AMSDOS-Kopf wird entfernt; --keep-header behält ihn)
z80tool dsk boot    spiel.dsk -o boot.bin     # Bootsektor einer SYSTEM-Diskette (Spur 0, Sektor &41: |CPM lädt ihn nach &0100)
z80tool dsk sector  spiel.dsk 0 0xC1          # ein beliebiger Sektor als Hexdump
```

Der Server kann dasselbe: `{"cmd":"dsk_ls","file":"spiel.dsk"}` und `{"cmd":"load_file","file":"spiel.dsk","name":"SPIEL.BIN"}` (lädt an die
Adresse aus dem AMSDOS-Kopf und setzt PC auf den Einsprung). Eine Diskette, deren Spur 0 keine der üblichen Sektor-IDs hat (`dsk info` sagt „no
AMSDOS format recognised“), hat einen eigenen Lader: Dann steht das Programm in bestimmten Sektoren, die der Bootcode liest; `dsk sector` zeigt sie.
Laufwerke emuliert der Emulator nicht.

## 2. Bis zum entpackten Programm laufen lassen

```sh
z80tool serve --allow-files
{"cmd":"machine","type":"cpc"}
{"cmd":"load_file","file":"spiel.dsk","name":"SPIEL.BIN"}
{"cmd":"run_until_unpacked"}
```

`run_until_unpacked` verfolgt alle Speicherstellen, die das **Programm schreibt**, und hält an, sobald ein Befehl aus **geschriebenem Speicher**
geholt werden soll, sofern der zusammenhängende geschriebene Bereich um ihn mindestens `min_written` Byte groß ist (Standard 1024). Das ist das
Merkmal eines Entpackers: Er schreibt das Programm und springt hinein. Die Antwort enthält `stop: "unpacked"`, den PC (meist der **Einsprung des
entpackten Programms**) und `region` (der geschriebene Bereich, in dem der PC liegt).

- **Mehrstufige Packer** kopieren zuerst ihren Entpacker an eine andere Stelle und laufen von dort. Dieser kleine Bereich zählt wegen der Schwelle
  nicht; ist das entpackte Programm kleiner als 1024 Byte, stellt man `min_written` kleiner ein (z. B. 64) und ruft
  `{"cmd":"run_until_unpacked","min_written":64,"continue":true}` auf, um den nächsten Fund zu bekommen: `continue` überspringt den zuletzt
  gefundenen Bereich.
- `ignore:[[von,bis],…]` nimmt weitere Bereiche aus. Die Sprungleiste der Firmware-Stubs (&B900–&BDFF) zählt nie.
- `max_instructions` (Standard 50 Millionen, höchstens 100 Millionen je Aufruf): Ein Entpacker braucht grob 30 bis 100 Befehle je entpacktes Byte.
- `written` zeigt die größten geschriebenen Bereiche jederzeit, `track_writes` schaltet die Verfolgung ein oder aus.
- Entpacker, die die Firmware brauchen (Bildschirm, Diskette), laufen mit den Stubs oft durch, weil die Aufrufe nur protokolliert werden; mit den
  echten ROMs (`load_rom`) laufen sie wie auf der Hardware.

## 3. Den entpackten Speicher sichern und analysieren

```sh
{"cmd":"save_memory","addr":"0x0400","len":30000,"file":"entpackt.bin"}
```

Adresse und Länge stehen in der Antwort von `run_until_unpacked` (`region`). Dann wird der Speicherauszug **an seiner Adresse** als Rohabbild
geladen, mit dem Einsprung als Einstiegspunkt:

```sh
python3 agent/z80_drive.py --work arbeit init --bin entpackt.bin --org 0x0400 --entry 0x0400
python3 agent/z80_annotate.py --bin entpackt.bin --org 0x0400 --entry 0x0400 --dry-run
```

Ab hier ist es ein gewöhnliches Programm (siehe [walkthrough.md](walkthrough.md) und [reverse-engineering.md](reverse-engineering.md)).

## Grenzen

- **Kein Kopierschutz, keine Spurlader:** Ein Programm, das beim Start selbst Sektoren der Diskette liest, läuft hier nicht: Der Emulator kennt
  keinen Floppy-Controller.
- **Packer, die auf Hardware warten** (Rasterzeilen über den PPI-Vsync, Interrupts), brauchen `irq_every_cycles` (CPC: 13312); exaktes CRTC-Timing
  gibt es nicht.
- **Der Fund ist eine Heuristik:** „Code aus geschriebenem Speicher“ trifft auch Programme, die Code zur Laufzeit erzeugen oder sich selbst
  verschieben. Im Zweifel `written` ansehen und den größten Bereich nehmen.
- Nur 64 KiB: Programme, die die zusätzlichen Bänke der CPC 6128 benutzen, lassen sich so nicht vollständig untersuchen.
