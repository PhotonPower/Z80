"""End-to-end test of agent/z80_annotate.py against a scripted mock of Ollama and the real z80tool.

Run:  Z80TOOL=build/z80tool python3 -m unittest discover -s tests/agent -v
"""
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mock_ollama import MockOllama  # noqa: E402

spec = importlib.util.spec_from_file_location("z80_annotate", os.path.join(ROOT, "agent", "z80_annotate.py"))
annotate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annotate)

Z80TOOL = os.environ.get("Z80TOOL") or os.path.join(ROOT, "build", "z80tool")

DEMO = """\
SCR_SET_MODE EQU &BC0E
SCR_SET_INK  EQU &BC32
TXT_OUTPUT   EQU &BB5A
        ORG &4000
start:  LD A,1
        CALL SCR_SET_MODE
        LD A,1
        LD B,26
        LD C,26
        CALL SCR_SET_INK
        LD HL,msg
        CALL print
        LD DE,&C78A
        LD HL,sprite
        CALL draw8
        LD A,(counter)
        INC A
        LD (counter),A
        RET
print:  LD A,(HL)
        OR A
        RET Z
        CALL TXT_OUTPUT
        INC HL
        JR print
draw8:  LD B,8
d1:     LD A,(HL)
        LD (DE),A
        INC HL
        LD A,D
        ADD A,8
        LD D,A
        DJNZ d1
        RET
msg:    DB "HELLO",0
sprite: DB &FF,&81,&BD,&A5,&A5,&BD,&81,&FF
        ORG &A000
counter: DB 0
"""


def build_snapshot(directory):
    """A CPC snapshot containing the demo program, PC at the start."""
    src = os.path.join(directory, "demo.asm")
    sym = os.path.join(directory, "demo.sym")
    sna = os.path.join(directory, "demo.sna")
    with open(src, "w") as f:
        f.write(DEMO)
    header = bytearray(0x100)
    header[0:8] = b"MV - SNA"
    header[0x10] = 3
    header[0x21:0x23] = (0xBFFF).to_bytes(2, "little")  # SP
    header[0x23:0x25] = (0x4000).to_bytes(2, "little")  # PC
    header[0x25] = 1
    header[0x40] = 0x8D                                   # mode 1, ROMs off
    header[0x43 + 1], header[0x43 + 6], header[0x43 + 9], header[0x43 + 12] = 40, 25, 7, 0x30
    header[0x6B] = 64
    with open(sna, "wb") as f:
        f.write(bytes(header) + bytes(0x10000))
    out = subprocess.run([Z80TOOL, "asm", src, "--sna", sna, "-s", sym], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    symbols = {}
    with open(sym) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                symbols[parts[0]] = int(parts[1], 16)
    return sna, symbols


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def plans_for(sym):
    h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
    msg, sprite, counter = f"0x{sym['MSG']:04X}", f"0x{sym['SPRITE']:04X}", f"0x{sym['COUNTER']:04X}"
    return {
        h("PRINT"): [
            [("run_routine", {"addr": h("PRINT"), "registers": {"HL": msg}})],
            [("document_routine", {"addr": h("PRINT"), "name": "LD", "summary": "bad name on purpose"})],  # rejected: reserved word
            [("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "Prints a zero terminated string.",
                                   "inputs": "HL = text", "outputs": "HL behind the terminator", "clobbers": "AF HL"})],
            [("finish_routine", {"summary": "done"})],
        ],
        h("DRAW8"): [
            [("run_routine", {"addr": h("DRAW8"), "registers": {"HL": sprite, "DE": "0xC78A"}, "screenshot": True})],
            [("name_label", {"addr": h("D1"), "name": "sprite_row_loop"}),
             ("document_routine", {"addr": h("DRAW8"), "name": "draw_sprite_8x8", "summary": "Copies an 8x8 byte sprite to the screen.",
                                   "inputs": "HL = sprite data, DE = screen address", "clobbers": "AF BC DE HL"})],
            [("finish_routine", {})],
        ],
        h("START"): [
            [("run_routine", {"addr": h("START"), "screenshot": True, "max_instructions": 100000})],
            [("name_variable", {"addr": counter, "name": "run_counter", "size": 1, "comment": "how often the demo ran"}),
             ("mark_data", {"addr": msg, "length": 6, "kind": "text", "name": "greeting"}),
             ("mark_data", {"addr": sprite, "length": 8, "kind": "bytes", "name": "face_sprite"}),
             ("add_comment", {"addr": h("START"), "text": "program entry"}),
             ("document_routine", {"addr": h("START"), "name": "show_greeting_demo", "summary": "Sets mode 1, prints HELLO and draws a face."})],
            [("finish_routine", {})],
        ],
    }


class OrchestratorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(Z80TOOL):
            raise unittest.SkipTest(f"z80tool not found at {Z80TOOL} (set Z80TOOL)")
        annotate.Ollama.retry_delay = 0  # do not wait between retries in tests

    def run_main(self, argv):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = annotate.main(argv)
        return code, buffer.getvalue()

    def test_full_run_with_scripted_model(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            mock = MockOllama(plans_for(sym))
            host = mock.start()
            try:
                out, project, log = (os.path.join(d, n) for n in ("out.asm", "proj.json", "log.jsonl"))
                code, printed = self.run_main(["--sna", sna, "--host", host, "--model", "mock", "--project", project, "--out", out,
                                               "--log", log, "--max-turns", "8", "--lang", "German", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("VERIFIED", printed)
            listing = read_text(out)
            for expected in ("print_string:", "draw_sprite_8x8:", "show_greeting_demo:", "sprite_row_loop:", "greeting:",
                             "face_sprite:", "LD A,(run_counter)", "CALL print_string", "CALL TXT_OUTPUT", "CALL SCR_SET_MODE",
                             "; program entry", "in:       HL = sprite data, DE = screen address", "run_counter"):
                self.assertIn(expected, listing)
            self.assertNotIn("(not yet documented)", listing)
            # the project file holds the same knowledge and can be loaded again
            proj = json.loads(read_text(project))
            self.assertEqual({r["name"] for r in proj["routines"]}, {"print_string", "draw_sprite_8x8", "show_greeting_demo"})
            # what the model experienced
            self.assertGreaterEqual(mock.images_seen, 1, "screenshots must reach the model as images")
            self.assertTrue(any("reserved" in e for e in mock.tool_errors), mock.tool_errors)
            self.assertTrue(any("text printed via firmware: 'HELLO'" in r for r in mock.tool_results), mock.tool_results)
            self.assertTrue(any("screen: " in r and "pixels changed" in r for r in mock.tool_results))
            self.assertTrue(any("columns 10-10" in r for r in mock.tool_results))  # the sprite is in text column 10
            self.assertTrue(any("German" in p for p in mock.system_prompts))
            # bottom-up order: callees are documented before the entry routine
            lines = [json.loads(x) for x in read_text(log).splitlines() if '"tool": "document_routine"' in x]
            names = [x["args"]["name"] for x in lines if x["result"].startswith("ok")]
            self.assertEqual(names[-1], "show_greeting_demo")

            # second run: everything is documented already, the model is not asked again
            mock2 = MockOllama({})
            host2 = mock2.start()
            try:
                code, printed = self.run_main(["--sna", sna, "--host", host2, "--model", "mock", "--project", project, "--out", out,
                                               "--z80tool", Z80TOOL])
            finally:
                mock2.stop()
            self.assertEqual(code, 0)
            self.assertEqual(len(mock2.requests), 0)
            self.assertIn("0 routines documented", printed)

    def test_model_that_never_finishes_leaves_the_listing_valid(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            mock = MockOllama({})  # answers every request with plain text and no tool call
            host = mock.start()
            try:
                out = os.path.join(d, "out.asm")
                code, printed = self.run_main(["--sna", sna, "--host", host, "--model", "mock", "--out", out, "--max-turns", "4",
                                               "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)  # no annotations, but the listing is still exact
            self.assertIn("VERIFIED", printed)
            self.assertIn("sub_", read_text(out))  # automatic names remain

    def test_unreachable_or_failing_ollama_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            sna, _ = build_snapshot(d)
            code, _ = self.run_main(["--sna", sna, "--host", "http://127.0.0.1:9", "--model", "mock", "--z80tool", Z80TOOL])
            self.assertEqual(code, 2)
            mock = MockOllama({}, fail_with=404)
            host = mock.start()
            try:
                code, _ = self.run_main(["--sna", sna, "--host", host, "--model", "no-such-model", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 2)

    def test_address_normalisation(self):
        n = annotate.norm_addr
        self.assertEqual(n("4010"), "0x4010")
        self.assertEqual(n("0x4010"), "0x4010")
        self.assertEqual(n("&4010"), "&4010")
        self.assertEqual(n(16400), 16400)
        self.assertEqual(n("print_string"), "print_string")
        self.assertEqual(n("10"), "10")        # short decimal-looking strings stay decimal
        self.assertEqual(n("BB5A"), "0xBB5A")
        with self.assertRaises(ValueError):
            n("")
        with self.assertRaises(ValueError):
            n(True)

    def test_tool_arguments_are_validated(self):
        server = annotate.Z80Server(Z80TOOL)
        try:
            tools = annotate.Tools(server)
            self.assertTrue(tools.dispatch("nonexistent", {}).startswith("error: unknown tool"))
            self.assertTrue(tools.dispatch("read_memory", {}).startswith("error: bad arguments"))
            self.assertTrue(tools.dispatch("read_memory", "oops").startswith("error: arguments must be an object"))
            self.assertTrue(tools.dispatch("name_label", {"addr": "0x4000", "name": "has space"}).startswith("error:"))
            self.assertTrue(tools.dispatch("get_listing", {"from": "0x4000", "to": "0x4010"}).startswith("error:"))  # nothing analysed
        finally:
            server.close()


if __name__ == "__main__":
    unittest.main()
