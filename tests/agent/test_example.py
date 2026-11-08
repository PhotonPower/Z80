"""The example program of the whole chain: assembler -> emulator with firmware stubs -> analysis -> orchestrator tools (dry run, run_routine,
mark_constant, mark_data, completeness check).

Run:  Z80TOOL=build/z80tool python3 -m unittest discover -s tests/agent -v
"""
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mock_ollama import MockOllama  # noqa: E402

spec = importlib.util.spec_from_file_location("z80_annotate", os.path.join(ROOT, "agent", "z80_annotate.py"))
annotate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(annotate)

Z80TOOL = os.environ.get("Z80TOOL") or os.path.join(ROOT, "build", "z80tool")
SOURCE = os.path.join(ROOT, "examples", "cpc", "demo_cpc.asm")
EXPECTED_OUTPUT = "Z80 demo\nHello from the Z80 example\nsum=150\none two three\n"

MAIN, PRINT, DISPATCH, TABLE, CMD_HELLO, CMD_SUM, PRINT_DEC = "0x4000", "0x401C", "0x4025", "0x4032", "0x4038", "0x403E", "0x4074"
BANNER, RUNS = "0x40A5", "0x40FD"


class ExampleProgramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(Z80TOOL):
            raise unittest.SkipTest(f"z80tool not found at {Z80TOOL} (set Z80TOOL)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = os.path.join(cls.tmp.name, "demo_cpc.bin")
        r = subprocess.run([Z80TOOL, "asm", SOURCE, "-o", cls.bin], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def server(self, analyse=True):
        s = annotate.Z80Server(Z80TOOL)
        s.ok(cmd="machine", type="cpc")
        s.ok(cmd="load_file", file=self.bin, addr="0x4000")
        if analyse:
            s.ok(cmd="analyze", entries=[MAIN], reset=True)
        return s

    def tools(self, s, routine=PRINT):
        s.ok(cmd="checkpoint", name="baseline")
        t = annotate.Tools(s)
        r = next(x for x in s.ok(cmd="routines")["routines"] if x["addr"] == routine)
        t.range, t.blocks = annotate.Annotator.hull(r), annotate.Annotator.blocks(r)
        t.program = (0x4000, 0x40FF)
        return t

    def test_runs_in_the_emulator(self):
        s = self.server(analyse=False)
        try:
            r = s.ok(cmd="call", addr=MAIN)
            self.assertTrue(r["returned"])
            self.assertNotIn("fetched_from_empty_memory", r)                    # the stubbed firmware is not empty memory
            self.assertEqual(s.ok(cmd="console")["text"], EXPECTED_OUTPUT)
            calls = s.ok(cmd="firmware_log")["calls"]
            self.assertTrue(all(c["name"] == "TXT_OUTPUT" for c in calls))
            self.assertEqual({c.get("call_site") for c in calls}, {"0x401F", "0x409E"})   # print and print_dec call it
        finally:
            s.close()

    def test_analysis_finds_the_structure(self):
        s = self.server()
        try:
            tables = s.ok(cmd="jump_tables")["tables"]
            self.assertEqual(len(tables), 1)
            self.assertEqual((tables[0]["kind"], tables[0]["table"], tables[0]["entry_count"], tables[0]["followed"]), ("pointers", TABLE, 3, True))
            routines = s.ok(cmd="routines")["routines"]
            self.assertEqual(len(routines), 7)    # main, print, dispatch, print_dec and the three cases of the jump table
            self.assertEqual(sorted(r["addr"] for r in routines if r.get("via_table")), [CMD_HELLO, CMD_SUM, "0x4062"])
            regions = s.ok(cmd="data_regions", apply=True)
            kinds = sorted(r["kind"] for r in regions["regions"] if r.get("applied"))
            # five strings with a NUL, the table of three string pointers (whose strings count among the six texts), two tables of numbers
            # read word by word, and the 16-bit variable `runs`
            self.assertEqual(kinds, ["pointers"] + ["text"] * 6 + ["words"] * 3)
            s.ok(cmd="analyze", entries=[MAIN], reset=True)
            listing = s.ok(cmd="listing", verify=True)
            self.assertTrue(listing["verified"], listing["verify_detail"])
            text = listing["text"]
            self.assertIn('DB "Hello from the Z80 example",&0D,&00', text)
            self.assertIn("jump table (3 entries)", text)
            self.assertIn("DW sub_4038,sub_403E,sub_4062", text)
            self.assertIn("DW data_40D8,data_40DD,data_40E2", text)                  # the string table names its strings
            self.assertIn("DW &2710,&03E8,&0064,&000A", text)                         # the powers of ten print_dec walks through
            self.assertIn("JP sub_401C", text)                                        # the tail call
        finally:
            s.close()

    def test_run_routine_with_text_memory_and_setup(self):
        s = self.server()
        try:
            t = self.tools(s)
            text = t.t_run_routine(PRINT, registers={"HL": "0x5000"}, memory=[{"addr": "0x5000", "text": "Hi"}])
            self.assertIn("returned: True", text)
            self.assertIn("text printed via firmware: 'Hi'", text)
            self.assertIn("from 0x401F", text)                                        # the CALL TXT_OUTPUT of print
            self.assertIn("memory changed outside the screen RAM: nothing", text)     # the caller's string is not a change of the routine
            # the program entry as setup: its output and its change of `runs` are not part of the report; the second run changes runs again
            self.assertEqual(t.dispatch("name_variable", {"addr": RUNS, "name": "run_count", "size": 2}), "ok: variable run_count")
            text = t.t_run_routine(MAIN, setup=[{"addr": MAIN}])
            self.assertIn(f"setup {MAIN}: returned", text)
            self.assertIn(f"{RUNS} len 1: 01 -> 02", text)
            self.assertIn("[run_count]", text)
            self.assertEqual(text.count("Z80 demo"), 1)                               # only the second run's text
            # a setup that does not return, and a bad setup entry
            self.assertIn("did NOT return", t.t_run_routine(PRINT, setup=[{"addr": "0x5000"}], max_instructions=500))
            self.assertTrue(t.dispatch("run_routine", {"addr": PRINT, "setup": [{"nope": 1}]}).startswith("error"))
        finally:
            s.close()

    def test_run_routine_into_empty_memory(self):
        s = self.server()
        try:
            text = self.tools(s).t_run_routine("0x5000", max_instructions=1000)
            self.assertIn("EMPTY memory", text)
            self.assertIn("first at 0x5000", text)
            self.assertIn("did not return", text)
        finally:
            s.close()

    def test_mark_constant_mark_data_and_remove(self):
        s = self.server()
        try:
            t = self.tools(s)
            self.assertEqual(t.dispatch("mark_constant", {"addr": MAIN}), "ok: constant marked")
            self.assertEqual(s.ok(cmd="ann_list")["annotations"]["constants"], [MAIN])
            self.assertIn("first byte", t.dispatch("mark_constant", {"addr": "0x4001"}))        # inside LD HL,banner
            self.assertIn("no 16-bit operand", t.dispatch("mark_constant", {"addr": PRINT}))    # LD A,(HL)
            self.assertEqual(t.dispatch("mark_constant", {"addr": MAIN, "remove": True}), "ok: constant mark removed")
            # a text needs no length: it ends with its NUL
            self.assertEqual(t.dispatch("mark_data", {"addr": BANNER, "kind": "text", "name": "banner"}), "ok: data region marked (10 bytes)")
            refs = {r["addr"]: r for r in s.ok(cmd="references", **{"from": MAIN, "to": "0x401B"})["references"]}
            self.assertEqual((refs[BANNER]["name"], refs[BANNER]["kind"], refs[BANNER]["length"]), ("banner", "text", 10))
            self.assertTrue(t.dispatch("mark_data", {"addr": "0x40E9", "kind": "words"}).startswith("error: length is required"))
            self.assertIn("contains code", t.dispatch("mark_data", {"addr": MAIN, "kind": "bytes", "length": 4}))
            self.assertEqual(t.dispatch("remove_annotation", {"addr": BANNER}), "ok: removed")
            self.assertEqual(t.dispatch("remove_annotation", {"addr": BANNER}), "nothing was annotated at that address")
            self.assertEqual(s.ok(cmd="ann_list")["annotations"]["data"], [])
        finally:
            s.close()

    def test_completeness_and_documentation(self):
        s = self.server()
        try:
            t = self.tools(s, routine=MAIN)
            r = next(x for x in s.ok(cmd="routines")["routines"] if x["addr"] == MAIN)
            t.current = MAIN
            joined = " | ".join(t.completeness_problems(annotate.Annotator.blocks(r)))
            self.assertIn("&40A5", joined)                                            # LD HL,banner: an address without a name
            self.assertIn("&40FD", joined)                                            # LD HL,(runs)
            self.assertEqual(t.documentation_problems(), [])                          # not documented yet: another message comes first
            t.dispatch("document_routine", {"addr": MAIN, "name": "run_demo", "summary": "x"})
            problems = t.documentation_problems()
            self.assertEqual(len(problems), 2)
            self.assertIn("without inputs, outputs, clobbers", problems[0])
            self.assertIn("without `confidence`", problems[1])
            t.dispatch("document_routine", {"addr": MAIN, "name": "run_demo", "summary": "x", "inputs": "none", "outputs": "none",
                                            "clobbers": "AF BC DE HL", "confidence": "high"})
            self.assertEqual(t.documentation_problems(), [])
            # an excerpt for reading has no prelude of EQU lines but names everything
            excerpt = t.dispatch("get_listing", {"from": MAIN, "to": "0x401B"})
            self.assertNotIn(" EQU ", excerpt)
            self.assertIn("CALL sub_401C", excerpt)
        finally:
            s.close()

    def test_helpers(self):
        self.assertEqual(annotate.ascii_of("486920"), ' "Hi "')
        self.assertEqual(annotate.ascii_of("576F726CE4"), ' "World"')               # bit 7 of the last character is ignored
        self.assertEqual(annotate.ascii_of("000102030405"), "")
        self.assertEqual(annotate.ascii_of("zz"), "")
        objects = [(0x1000, 2, "ptr"), (0x1002, 16, "buffer")]
        self.assertEqual(annotate.touched_names(objects, 0x1000, 2), "  [ptr]")
        self.assertEqual(annotate.touched_names(objects, 0x1001, 3), "  [ptr+1, buffer]")
        self.assertEqual(annotate.touched_names(objects, 0x2000, 2), "")

        class Fake:  # a server that only knows the routines
            def ok(self, **req):
                assert req["cmd"] == "routines"
                return {"routines": [{"name": "sub_4000", "addr": "0x4000", "blocks": [["0x4000", "0x4040"]]},
                                     {"name": "sub_4040", "addr": "0x4040", "blocks": [["0x4040", "0x4080"]]}]}

        t = annotate.Tools(Fake())
        t.program = (0x4000, 0x4100)
        notes = t.write_notes([{"addr": "0x40FE", "len": 4}, {"addr": "0x4044", "len": 2}, {"addr": "0x4090", "len": 2}])
        self.assertEqual(len(notes), 2)
        self.assertIn("beyond the end of the loaded program (0x40FF)", notes[0])
        self.assertIn("inside the code of sub_4040", notes[1])
        self.assertEqual(t.write_notes([{"addr": "0x5000", "len": 4}], caller_ranges=[(0x5000, 16)]), [])   # the caller's own buffer

        class Listing:  # a server that only answers the listing of a routine
            def __init__(self, instructions, comments):
                self.reply = {"text": "", "instructions": instructions, "user_comments": comments}

            def ok(self, **req):
                return self.reply

        def problems(instructions, comments):
            tools = annotate.Tools(Listing(instructions, comments))
            tools.blocks = [("0x1000", "0x1100")]
            return tools.completeness_problems()

        self.assertEqual(problems(2, 1), [])        # two instructions: one comment is enough
        self.assertEqual(len(problems(2, 0)), 1)
        self.assertEqual(len(problems(4, 1)), 1)    # four instructions: two
        self.assertEqual(problems(4, 2), [])
        self.assertEqual(len(problems(30, 2)), 1)   # about one per ten: three
        self.assertEqual(problems(30, 3), [])

    def test_dry_run_contacts_no_model(self):
        mock = MockOllama({})
        url = mock.start()
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                code = annotate.main(["--bin", self.bin, "--org", "0x4000", "--dry-run", "--host", url, "--z80tool", Z80TOOL])
        finally:
            mock.stop()
        text = out.getvalue()
        self.assertEqual(code, 0, text)
        self.assertEqual(mock.requests, [])
        self.assertIn("== dry run: no model is contacted ==", text)
        self.assertIn("10 data regions marked", text)
        self.assertIn("7 routines, 7 still without documentation", text)
        self.assertIn(f"jump table (pointers) at {TABLE}: 3 entries", text)
        self.assertIn(f"via table {TABLE}", text)
        self.assertIn("calls outside the analysed code: TXT_OUTPUT x2", text)
        self.assertIn("without --dry-run", text)
        self.assertNotIn("WARNING", text)

    def test_dry_run_warns_when_the_context_is_too_small(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = annotate.main(["--bin", self.bin, "--org", "0x4000", "--dry-run", "--num-ctx", "4096", "--z80tool", Z80TOOL])
        self.assertEqual(code, 0)
        self.assertIn("WARNING: the largest routine", out.getvalue())

    def test_no_auto_data(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            annotate.main(["--bin", self.bin, "--org", "0x4000", "--dry-run", "--no-auto-data", "--z80tool", Z80TOOL])
        self.assertNotIn("data regions marked", out.getvalue())
        self.assertIn("data not yet annotated:", out.getvalue())


if __name__ == "__main__":
    unittest.main()
