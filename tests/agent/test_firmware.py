"""Runs the real Amstrad CPC 6128 firmware in the emulator (needs a 32 KiB ROM image in $CPC_ROM).

The ROM is not part of this repository. CI downloads it for this test; locally set, e.g.
    CPC_ROM=cpc6128.rom Z80TOOL=build/z80tool python3 -m unittest tests/agent/test_firmware.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # works from any working directory

import test_orchestrator as base  # noqa: F401  (puts mock_ollama and the orchestrator module on the path)

ROM = os.environ.get("CPC_ROM")


@unittest.skipUnless(ROM and os.path.exists(ROM), "set CPC_ROM to a 32 KiB CPC 6128 ROM image")
class FirmwareTest(unittest.TestCase):
    def setUp(self):
        self.server = base.annotate.Z80Server(base.Z80TOOL)
        s = self.server
        s.ok(cmd="machine", type="cpc")
        s.ok(cmd="load_rom", file=ROM, which="both")
        s.ok(cmd="set", regs={"PC": 0, "SP": "0xBFFF"})

    def tearDown(self):
        self.server.close()

    def test_cold_start_reaches_basic_and_draws_text(self):
        s = self.server
        blank = s.ok(cmd="screen", name="blank")
        self.assertEqual(blank["mode"], 1)
        r = s.ok(cmd="run", irq_every_cycles=13312, max_instructions=30_000_000)
        self.assertEqual(r["stop"], "instruction_limit")        # BASIC sits in its input loop
        self.assertTrue(r["regs"]["IFF1"])                         # the firmware enabled interrupts
        d = s.ok(cmd="screen_diff", a="blank")
        self.assertGreater(d["changed_pixels"], 3000)               # the banner and the prompt were drawn
        self.assertLessEqual(d["text_cells"]["row0"], 1)            # starting in the first text rows
        self.assertLess(d["changed_percent"], 20)
        self.assertEqual(s.ok(cmd="firmware_log")["calls"], [])     # the real ROM runs, nothing is stubbed

    def test_txt_output_draws_a_character_and_the_diff_finds_it(self):
        s = self.server
        s.ok(cmd="run", irq_every_cycles=13312, max_instructions=30_000_000)
        s.ok(cmd="checkpoint", name="booted")
        s.ok(cmd="screen", name="before")
        r = s.ok(cmd="call", addr="0xBB5A", regs={"A": 0x5A}, max_instructions=200000)   # TXT_OUTPUT('Z')
        self.assertTrue(r["returned"])
        d = s.ok(cmd="screen_diff", a="before")
        self.assertGreater(d["changed_pixels"], 100)
        self.assertLess(d["changed_pixels"], 2000)                  # one character plus the moved cursor
        self.assertLessEqual(d["bbox"]["x1"] - d["bbox"]["x0"], 40)
        shot = s.ok(cmd="screen", name="zoom", crop=d["text_cells"], zoom=4)
        self.assertIn("png_base64", shot)
        s.ok(cmd="rollback", name="booted")                         # the experiment can be undone completely
        self.assertEqual(s.ok(cmd="screen_diff", a="before")["changed_pixels"], 0)

    def test_rom_is_invisible_to_the_ram_view(self):
        s = self.server
        rom_view = s.ok(cmd="read", addr=0, len=3, view="cpu")["hex"]
        ram_view = s.ok(cmd="read", addr=0, len=3)["hex"]
        self.assertEqual(rom_view, "01897F")                        # LD BC,&7F89: the 6128 firmware starts by configuring the Gate Array
        self.assertEqual(ram_view, "000000")


if __name__ == "__main__":
    unittest.main()
