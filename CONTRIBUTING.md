# Mitarbeit

## Bauen und testen

```sh
cmake -S . -B build -DZ80_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Voraussetzungen: CMake ≥ 3.16 und ein C++20-Compiler (GCC ≥ 11, Clang ≥ 14, MSVC 2022).

Weitere CMake-Optionen:

| Option                  | Standard | Bedeutung                                              |
|-------------------------|----------|--------------------------------------------------------|
| `Z80_BUILD_TESTS`       | `ON`     | Unit-Tests bauen                                        |
| `Z80_WARNINGS_AS_ERRORS`| `OFF`    | Compilerwarnungen als Fehler (in der CI auf Linux `ON`) |
| `Z80_SANITIZE`          | `OFF`    | AddressSanitizer + UBSan (GCC/Clang)                    |
| `Z80_BUILD_TOOLS`       | `OFF`    | Entwicklerwerkzeuge (`z80_dump_forms`, `z80_zex`)       |

## Regeln

- Neue Funktionalität bekommt Tests in `tests/`. Fehlerkorrekturen bekommen zuerst einen Test,
  der den Fehler zeigt.
- Änderungen am Dekoder oder Assembler müssen den exhaustiven Roundtrip-Test
  (`tests/test_roundtrip.cpp`) bestehen; im Zweifel zusätzlich die Kreuzprüfung
  (`tools/crosscheck_z80asm.py`, siehe [docs/testing.md](docs/testing.md)).
- Änderungen am Emulator müssen ZEXDOC und ZEXALL weiter bestehen (`z80_zex`, siehe
  [docs/testing.md](docs/testing.md)); die CI prüft das.
- Änderungen am Orchestrator (`agent/`) müssen `tests/agent` bestehen (läuft in CTest, wenn Python 3 vorhanden ist).
- Die Kernbibliothek (`include/z80`, `src`) hat keine Ein-/Ausgabe auf der Konsole und keine
  globalen Zustände; Benutzerinteraktion gehört nach `app/`.
- Quelltext, Bezeichner und Kommentare sind Englisch, die Dokumentation unter `docs/` Deutsch.
- Formatierung: siehe `.clang-format` / `.editorconfig`.
