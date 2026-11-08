"""agent/z80_drive.py: the annotation tools driven by hand, one command per process, the state in a work directory.

Run:  Z80TOOL=build/z80tool python3 -m unittest discover -s tests/agent -v
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
Z80TOOL = os.environ.get("Z80TOOL") or os.path.join(ROOT, "build", "z80tool")
DRIVE = os.path.join(ROOT, "agent", "z80_drive.py")
SOURCE = os.path.join(ROOT, "examples", "cpc", "demo_cpc.asm")


class DriveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(Z80TOOL):
            raise unittest.SkipTest(f"z80tool not found at {Z80TOOL} (set Z80TOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = os.path.join(cls.tmp.name, "demo_cpc.bin")
        r = subprocess.run([Z80TOOL, "asm", SOURCE, "-o", cls.bin], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        cls.work = os.path.join(cls.tmp.name, "work")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def drive(self, *args, work=None, expect=0):
        r = subprocess.run([sys.executable, DRIVE, "--work", work or self.work, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, f"{args}: {r.stdout}\n{r.stderr}")
        return r.stdout, r.stderr

    def init(self, work=None, binary=None):
        return self.drive("init", "--fresh", "--bin", binary or self.bin, "--org", "0x4000", "--z80tool", Z80TOOL, work=work)

    def test_a_whole_session(self):
        """init -> status -> show -> run_routine -> annotate -> finish -> listing: what a model does in a loop, one command per process."""
        out, _ = self.init()
        self.assertIn("7 routines", out)
        out, _ = self.drive("status")
        self.assertIn("7 routines, processing order", out)
        self.assertIn("0 of 7 finished.", out)
        self.assertIn("Next: `show 0x401C`", out)

        out, _ = self.drive("show", "0x401C")
        self.assertIn("ROUTINE_ADDR=0x401C", out)
        self.assertIn("Called from: 0x4003, 0x403B, 0x4041, 0x405F, 0x406D", out)   # three CALLs and the two tail calls (JP print)
        self.assertIn("TXT_OUTPUT (firmware)", out)
        self.assertNotIn(" EQU ", out)                                         # no prelude of names in the excerpt a model reads
        out_main, _ = self.drive("show", "0x4000")
        self.assertIn("Referenced from this routine", out_main)                # what the operands point at, with the contents
        self.assertIn('0x40A5 data_40A5 (text, 10 bytes): "Z80 demo\\r"', out_main)
        self.assertIn("CALL sub_401C", out_main)                               # names outside the excerpt
        out, _ = self.drive("show", "0x401C", "--system")
        self.assertIn("expert Z80 and Amstrad CPC reverse engineer", out)

        out, _ = self.drive("tool", "run_routine", json.dumps({"addr": "0x401C", "registers": {"HL": "0x5000"}, "memory": [{"addr": "0x5000", "text": "Hi"}]}))
        self.assertIn("returned: True", out)
        self.assertIn("'Hi'", out)
        out, _ = self.drive("tool", "run_routine", json.dumps({"addr": "0x401C", "registers": {"HL": "0x5000"}, "screenshot": True,
                                                               "memory": [{"addr": "0x5000", "text": "Hi"}]}))
        self.assertIn("screenshot written to", out)
        self.assertTrue(os.path.exists(os.path.join(self.work, "shot_1.png")))

        self.drive("tool", "document_routine", json.dumps({"addr": "0x401C", "name": "print_string", "summary": "Prints the zero-terminated text at HL.",
                                                           "inputs": "HL = text", "confidence": "high"}))
        # strict: sent back while anything is missing
        out, _ = self.drive("finish", "0x401C")
        self.assertIn("not finished yet", out)
        self.assertIn("document_routine without outputs, clobbers", out)
        self.assertIn("comment(s)", out)
        with open(os.path.join(self.work, "state.json")) as f:
            self.assertEqual(json.load(f)["warned"], ["0x401C"])
        self.drive("tool", "add_comment", json.dumps({"addr": "0x401D", "text": "a zero ends the text"}))
        self.drive("tool", "add_comment", json.dumps({"addr": "0x401F", "text": "one character to the screen"}))
        out, _ = self.drive("finish", "0x401C")                                # still not: the contract is incomplete
        self.assertIn("document_routine without outputs, clobbers", out)
        self.drive("tool", "document_routine", json.dumps({"addr": "0x401C", "name": "print_string", "summary": "Prints the zero-terminated text at HL.",
                                                           "inputs": "HL = text", "outputs": "HL = the terminator", "clobbers": "AF", "confidence": "high"}))
        out, _ = self.drive("finish", "0x401C")
        self.assertIn("ok: routine finished", out)
        out, _ = self.drive("status")
        self.assertIn("1 of 7 finished.", out)
        self.assertIn("finished, confidence high", out)
        self.assertIn("Next: `show 0x4025`", out)
        out, _ = self.drive("report")
        self.assertIn("6 of 7 routines are not finished or still have gaps.", out)

        out, _ = self.drive("listing", "--out", os.path.join(self.tmp.name, "out.asm"))
        self.assertIn("VERIFIED", out)
        with open(os.path.join(self.tmp.name, "out.asm")) as f:
            text = f.read()
        self.assertIn("print_string:", text)
        self.assertIn("JP print_string", text)                                 # the tail calls use the name
        self.assertIn("; a zero ends the text", text)

    def test_accept_gaps_is_the_orchestrators_rule(self):
        work = os.path.join(self.tmp.name, "work_gaps")
        self.init(work=work)
        self.drive("tool", "document_routine", json.dumps({"addr": "0x4025", "name": "dispatch_command", "summary": "x"}), work=work)
        out, _ = self.drive("finish", "0x4025", "--accept-gaps", work=work)
        self.assertIn("not finished yet", out)
        self.assertIn("a second call is accepted as final", out)
        out, _ = self.drive("finish", "0x4025", "--accept-gaps", work=work)
        self.assertIn("ok: routine finished", out)
        out, _ = self.drive("finish", "0x4025", work=work)                    # the strict rule still sees what is missing
        self.assertIn("not finished yet", out)

    def test_tail_calls_blocks_and_the_jump_table_in_the_task_text(self):
        """A jump to another routine is a tail call (not part of the routine); a shared exit far away is a part of its own, so the text says
        'in 2 parts' and shows two listings; a case of a jump table says how it is reached."""
        src = os.path.join(self.tmp.name, "tail.asm")
        with open(src, "w") as f:
            f.write("        ORG &4000\nmain:   CALL ra\n        CALL rb\n        RET\nra:     LD A,1\n        OR A\n        JR Z,shared\n        JP rb\n"
                    "rb:     LD B,2\n        RET\n        DS 64,&C9\nshared: LD C,9\n        RET\n")
        binary = os.path.join(self.tmp.name, "tail.bin")
        r = subprocess.run([Z80TOOL, "asm", src, "-o", binary], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        work = os.path.join(self.tmp.name, "work_tail")
        self.init(work=work, binary=binary)
        out, _ = self.drive("status", work=work)
        self.assertIn("3 routines", out)
        out, _ = self.drive("show", "0x4007", work=work)                       # ra: its own 8 bytes and the shared exit far behind
        self.assertIn("size: 11 bytes in 2 parts", out)
        self.assertIn("part 1 of 2", out)
        self.assertIn("part 2 of 2", out)
        self.assertIn("other code lies in between", out)
        self.assertIn("sub_400F (0x400F, not documented yet)", out)              # the tail call is listed as a callee
        out, _ = self.drive("show", "0x400F", work=work)                       # rb: called by the CALL of main and by the JP of ra
        self.assertIn("Called from: 0x4003, 0x400C", out)
        # a case of the jump table in the example: reached through the table, print is a callee but not a part of it
        out, _ = self.drive("show", "0x4038")
        self.assertIn("reached through the jump table at 0x4032 (no direct call)", out)
        self.assertIn("size: 6 bytes", out)
        self.assertIn("(0x401C", out)

    def test_errors_are_plain(self):
        _, err = self.drive("status", work=os.path.join(self.tmp.name, "empty"), expect=1)
        self.assertIn("run `init` first", err)
        _, err = self.drive("init", work=os.path.join(self.tmp.name, "w2"), expect=1)
        self.assertIn("init needs the program", err)
        _, err = self.drive("init", "--bin", self.bin, "--project", "x.json", work=os.path.join(self.tmp.name, "w3"), expect=1)
        self.assertIn("--project is set by the work directory", err)
        r = subprocess.run([sys.executable, DRIVE, "--work", os.path.join(self.tmp.name, "w4"), "init", "--bin", self.bin, "--bogus"], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)                                   # an option nobody knows is refused right away
        w5 = os.path.join(self.tmp.name, "w5")
        self.init(work=w5)
        _, err = self.drive("show", "0x9999", work=w5, expect=1)
        self.assertIn("no routine at 0x9999", err)
        _, err = self.drive("tool", "run_routine", "{not json", work=w5, expect=1)
        self.assertIn("not valid JSON", err)
        out, _ = self.drive("tool", "no_such_tool", "{}", work=w5, expect=1)
        self.assertIn("unknown tool", out)
        _, err = self.drive("tool", "finish_routine", "{}", work=w5, expect=1)
        self.assertIn("needs the routine", err)
        out, _ = self.drive("tool", "document_routine", json.dumps({"addr": "0x401C", "name": "HL", "summary": "x"}), work=w5, expect=1)
        self.assertIn("error", out)                                           # a register name is not a name
        out, _ = self.drive("finish", "0x401C", work=w5, expect=1)
        self.assertIn("document_routine for this routine first", out)         # not documented yet


if __name__ == "__main__":
    unittest.main()
