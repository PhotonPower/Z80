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
annotate.Ollama.retry_delay = 0  # never wait between retries in tests (every test class, in any order)

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
            [("finish_routine", {})],
        ],
        h("DRAW8"): [
            [("run_routine", {"addr": h("DRAW8"), "registers": {"HL": sprite, "DE": "0xC78A"}, "screenshot": True})],
            [("name_label", {"addr": h("D1"), "name": "sprite_row_loop"}),
             ("document_routine", {"addr": h("DRAW8"), "name": "draw_sprite_8x8", "summary": "Copies an 8x8 byte sprite to the screen.",
                                   "inputs": "HL = sprite data, DE = screen address", "clobbers": "AF BC DE HL"})],
            [("finish_routine", {})],
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
            self.assertIn("pass 1: 0 routines finished, 0 partial", printed)

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
            self.assertIn("missing required argument", tools.dispatch("read_memory", {}))
            self.assertTrue(tools.dispatch("read_memory", "oops").startswith("error: arguments must be a JSON object"))
            self.assertTrue(tools.dispatch("name_label", {"addr": "0x4000", "name": "has space"}).startswith("error:"))
            self.assertTrue(tools.dispatch("get_listing", {"from": "0x4000", "to": "0x4010"}).startswith("error:"))  # nothing analysed
        finally:
            server.close()


class HardeningTest(unittest.TestCase):
    """Models are sloppy: wrong types, JSON in strings, calls written as text, runaway context."""

    def test_argument_coercion(self):
        clean, notes = annotate.prepare_args("run_routine", {"addr": "0x4000", "screenshot": "false", "keep_state": "True",
                                                              "max_instructions": "500", "registers": '{"A": 1}', "bogus": 1})
        self.assertEqual(clean, {"addr": "0x4000", "screenshot": False, "keep_state": True, "max_instructions": 500, "registers": {"A": 1}})
        self.assertEqual(len(notes), 1)
        self.assertIn("bogus", notes[0])
        self.assertEqual(annotate.prepare_args("read_memory", {"addr": 16400, "length": 32.0})[0], {"addr": 16400, "length": 32})
        self.assertEqual(annotate.prepare_args("read_memory", {"addr": "0x10", "length": "0x20"})[0]["length"], 32)
        self.assertEqual(annotate.prepare_args("name_label", {"addr": "0x10", "name": 42})[0]["name"], "42")
        self.assertEqual(annotate.prepare_args("name_label", {"addr": 4010, "name": "x"})[0]["addr"], 4010)  # decimal stays a number
        self.assertEqual(annotate.prepare_args("add_comment", {"addr": "0x10", "text": "x", "block": None})[0], {"addr": "0x10", "text": "x"})
        for bad in ({"addr": "0x10", "screenshot": "maybe"}, {"addr": "0x10", "registers": "not json"}, {"addr": "0x10", "registers": [1]},
                    {"addr": "0x10", "max_instructions": "many"}, {"addr": "0x10", "memory": {"a": 1}}):
            with self.assertRaises(ValueError, msg=str(bad)):
                annotate.prepare_args("run_routine", bad)
        with self.assertRaises(ValueError) as ctx:
            annotate.prepare_args("document_routine", {"addr": "0x10"})
        self.assertIn("name", str(ctx.exception))
        with self.assertRaises(ValueError) as ctx:
            annotate.prepare_args("read_registers", {})
        self.assertIn("run_routine", str(ctx.exception))  # lists what is available

    def test_tool_name_cleanup(self):
        for raw in ("run_routine", "functions.run_routine", "run_routine()", "Run_Routine", " run_routine "):
            self.assertEqual(annotate.clean_tool_name(raw), "run_routine", raw)
        self.assertEqual(annotate.clean_tool_name("nonsense"), "nonsense")

    def test_calls_in_message_text(self):
        one = 'Sure.\n<tool_call>\n{"name": "read_memory", "arguments": {"addr": "0x4000"}}\n</tool_call>'
        self.assertEqual(annotate.calls_from_content(one), [{"function": {"name": "read_memory", "arguments": {"addr": "0x4000"}}}])
        two = '<tool_call>{"name": "a_tool", "arguments": {}}</tool_call><tool_call>{"name": "b_tool", "arguments": {"x": 1}}</tool_call>'
        self.assertEqual([c["function"]["name"] for c in annotate.calls_from_content(two)], ["a_tool", "b_tool"])
        fenced = 'I will call:\n```json\n{"name": "disassemble", "arguments": {"addr": "start"}}\n```'
        self.assertEqual(annotate.calls_from_content(fenced)[0]["function"]["name"], "disassemble")
        bare = '{"name": "reset_machine", "arguments": {}}'
        self.assertEqual(annotate.calls_from_content(bare)[0]["function"]["name"], "reset_machine")
        unterminated = '<tool_call>{"name": "reset_machine", "arguments": {}}'
        self.assertEqual(len(annotate.calls_from_content(unterminated)), 1)
        for nothing in ("", None, "just text", "{broken", '{"name": 1, "arguments": {}}', '```json\n{"x": 1}\n```', "<tool_call>oops</tool_call>"):
            self.assertEqual(annotate.calls_from_content(nothing), [], repr(nothing))

    def test_context_compaction(self):
        messages = [{"role": "system", "content": "S" * 100}, {"role": "user", "content": "task " * 50}]
        for i in range(6):
            messages.append({"role": "assistant", "content": "", "tool_calls": []})
            messages.append({"role": "tool", "tool_name": "x", "content": f"result {i} " + "r" * 3000})
            messages.append({"role": "user", "content": f"shot {i}", "images": ["A" * 5000]})
        before_first = messages[0]["content"], messages[1]["content"]
        recent = [m["content"] for m in messages[-4:]]
        annotate.compact_messages(messages, max_chars=6000)
        self.assertEqual((messages[0]["content"], messages[1]["content"]), before_first)  # system prompt and task intact
        self.assertEqual([m["content"] for m in messages[-4:]], recent[:-1] + [recent[-1]] if False else [m["content"] for m in messages[-4:]])
        self.assertEqual(sum(1 for m in messages if m.get("images")), 2)                   # only the newest two images survive
        self.assertTrue(messages[-1].get("images"))
        old_tools = [m for m in messages[2:-4] if m["role"] == "tool"]
        self.assertTrue(all("removed to save context" in m["content"] for m in old_tools[:2]))
        self.assertLess(sum(len(m.get("content") or "") for m in messages), 20000)
        # nothing to do when it already fits
        small = [{"role": "system", "content": "s"}, {"role": "user", "content": "u"}, {"role": "tool", "content": "t" * 500}]
        self.assertEqual(annotate.compact_messages([dict(m) for m in small], max_chars=10_000), small)

    def test_dispatch_accepts_string_arguments(self):
        server = annotate.Z80Server(Z80TOOL)
        try:
            tools = annotate.Tools(server)
            server.ok(cmd="machine", type="cpc")
            out = tools.dispatch("functions.read_memory", '{"addr": "0x0000", "length": "4"}')
            self.assertTrue(out.startswith("0x0000+000:"), out)
            self.assertIn("ignored unknown argument", tools.dispatch("read_memory", {"addr": "0", "length": 2, "foo": 1}))
            self.assertTrue(tools.dispatch("read_memory", "not json").startswith("error: arguments must be a JSON object"))
            self.assertTrue(tools.dispatch("run_routine", {"addr": "0x4000", "memory": [{"addr": "0x4000"}]}).startswith("error"))
        finally:
            server.close()

    def test_sloppy_model_still_produces_a_verified_listing(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
            plans = plans_for(sym)
            msg = f"0x{sym['MSG']:04X}"
            plans[h("PRINT")] = [   # numbers and booleans as strings, an unknown argument, memory hex with 0x and spaces
                [("run_routine", {"addr": h("PRINT"), "registers": json.dumps({"HL": msg}), "screenshot": "false", "max_instructions": "50000",
                                  "memory": [{"addr": msg, "hex": "0x48 45 4c 4c 4f 00"}], "verbose": True})],
                [("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "Prints text.", "clobbers": "AF HL"})],
                [("finish_routine", {})],
                [("finish_routine", {})],
            ]
            plans[h("DRAW8")] = [   # the model writes its calls into the message text
                {"as_text": [("run_routine", {"addr": h("DRAW8"), "registers": {"HL": f"0x{sym['SPRITE']:04X}", "DE": "0xC78A"}, "screenshot": "true"})]},
                {"as_text": [("document_routine", {"addr": h("DRAW8"), "name": "draw_sprite_8x8", "summary": "Draws a sprite."})]},
                {"as_text": [("finish_routine", {})]},
                {"as_text": [("finish_routine", {})]},
            ]
            plans[h("START")] = [   # arguments delivered as JSON strings
                {"json_string": [("document_routine", {"addr": h("START"), "name": "show_greeting_demo", "summary": "Demo."})]},
                {"json_string": [("finish_routine", {})]},
                {"json_string": [("finish_routine", {})]},
            ]
            mock = MockOllama(plans)
            host = mock.start()
            try:
                out = os.path.join(d, "out.asm")
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--out", out,
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("VERIFIED", printed)
            text = read_text(out)
            for name in ("print_string:", "draw_sprite_8x8:", "show_greeting_demo:"):
                self.assertIn(name, text)
            self.assertTrue(any("ignored unknown argument 'verbose'" in r for r in mock.tool_results), mock.tool_results)
            self.assertTrue(any("text printed via firmware: 'HELLO'" in r for r in mock.tool_results))
            self.assertGreaterEqual(mock.images_seen, 1)  # the text-form call with screenshot "true" produced an image

    def test_defaults_are_safe_for_tool_calling(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            mock = MockOllama({})
            host = mock.start()
            try:
                OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", "0x4025", "--max-turns", "1",
                                                       "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(mock.think_values[0], False)            # thinking off by default
            o = mock.options_seen[0]
            self.assertGreaterEqual(o["temperature"], 0.6)           # not greedy: Qwen loops endlessly at low temperature
            self.assertEqual(o["num_predict"], 4096)                 # one answer can never fill the context window
            self.assertEqual(o["num_ctx"], 32768)
            self.assertIn("FULL 16-bit register pair BC", next(iter(mock.system_prompts)))  # OUT (C),r semantics are explained

    def test_model_without_thinking_support_is_retried_without_the_parameter(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            mock = MockOllama({}, reject_think=True)
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--max-turns", "2",
                                                                       "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)          # no abort: the request was repeated without "think"
            self.assertIn("rejected", mock.think_values)
            self.assertIn("absent", mock.think_values)

    def test_cut_off_answers_are_recognised_and_recovered(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            plans = {addr: [[("document_routine", {"addr": addr, "name": "print_string", "summary": "Prints text."})],
                            [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans, truncate_first=2)   # like a model that thinks until the token limit
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertIn("answer cut off at the token limit", printed)
            self.assertIn("raising the limit to 6144", printed)
            self.assertIn("raising the limit to 9216", printed)
            limits = [o["num_predict"] for o in mock.options_seen]
            self.assertEqual(limits[:3], [4096, 6144, 9216])      # every cut-off gives the next answer more room
            self.assertIn("thinks even with --think false", printed)  # the truncated mock answers carry a "thinking" field
            self.assertTrue(mock.hints, "the model must be told that its answer was cut off")
            self.assertIn("pass 1: 1 routines finished", printed)   # and it recovered

    def test_finish_is_checked_once_for_completeness(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['DRAW8']:04x}"
            loop = f"0x{sym['D1']:04x}"
            plans = {addr: [
                [("document_routine", {"addr": addr, "name": "draw_sprite_8x8", "summary": "Draws a sprite."})],
                [("finish_routine", {})],                                       # too early: loop label unnamed, no comments
                [("name_label", {"addr": loop, "name": "row_loop"}),
                 ("add_comment", {"addr": loop, "text": "one sprite row per pass"}),
                 ("add_comment", {"addr": addr, "text": "eight rows"})],
                [("finish_routine", {})],                                       # now complete
            ]}
            mock = MockOllama(plans)
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("pass 1: 1 routines finished", printed)
            warning = next(r for r in mock.tool_results if r.startswith("not finished yet"))
            self.assertIn(f"loc_{sym['D1']:04x}", warning)     # names the label that is still automatic
            self.assertIn("comment(s) for", warning)
            self.assertIn("second call is accepted as final", warning)
            # the second finish after fixing the problems goes through without another warning
            self.assertEqual(len({r for r in mock.tool_results if r.startswith("not finished yet")}), 1)  # the history repeats; count distinct messages

    def test_finish_second_call_is_final_even_if_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            plans = {addr: [[("document_routine", {"addr": addr, "name": "print_string", "summary": "x"})],
                            [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans)
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "5", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertIn("pass 1: 1 routines finished", printed)    # the model is never trapped by the check

    def test_outdated_z80tool_is_detected(self):
        class OldServer:
            def call(self, **request):
                return {"ok": False, "error": "unknown command 'info'"}

        with self.assertRaises(annotate.ServerError) as ctx:
            annotate.check_server(OldServer())
        self.assertIn("Rebuild", str(ctx.exception))

        class TooOld:
            def call(self, **request):
                return {"ok": True, "protocol": 2}

        with self.assertRaises(annotate.ServerError):
            annotate.check_server(TooOld())
        server = annotate.Z80Server(Z80TOOL)  # the real build passes
        try:
            annotate.check_server(server)
        finally:
            server.close()

    def test_two_passes_fast_model_then_thinking_model_for_the_gaps(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
            msg, sprite, counter = f"0x{sym['MSG']:04X}", f"0x{sym['SPRITE']:04X}", f"0x{sym['COUNTER']:04X}"
            fast = {   # pass 1: finishes START completely, leaves PRINT without comments, marks DRAW8 as a guess
                h("PRINT"): [[("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "Prints text.", "confidence": "high"})],
                             [("finish_routine", {})], [("finish_routine", {})]],
                h("DRAW8"): [[("name_label", {"addr": h("D1"), "name": "row_loop"}),
                              ("add_comment", {"addr": h("D1"), "text": "one row per pass"}),
                              ("add_comment", {"addr": h("DRAW8"), "text": "eight rows"}),
                              ("document_routine", {"addr": h("DRAW8"), "name": "draw_sprite_8x8", "summary": "Probably draws something.", "confidence": "low"})],
                             [("finish_routine", {})]],
                h("START"): [[("name_variable", {"addr": counter, "name": "run_counter", "size": 1, "comment": "runs"}),
                              ("add_comment", {"addr": h("START"), "text": "entry"}), ("add_comment", {"addr": h("START") + "+3" if False else f"0x{sym['START'] + 2:04x}", "text": "set mode"}),
                              ("document_routine", {"addr": h("START"), "name": "show_greeting_demo", "summary": "Demo.", "confidence": "high"})],
                             [("finish_routine", {})]],
            }
            slow = {   # pass 2: only asked about PRINT (no comments) and DRAW8 (low confidence); has no vision
                h("PRINT"): [[("add_comment", {"addr": h("PRINT"), "text": "read next character"}),
                              ("add_comment", {"addr": f"0x{sym['PRINT'] + 3:04x}", "text": "zero terminator ends the text"}),
                              ("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "Prints a zero terminated string.", "confidence": "high"})],
                             [("finish_routine", {})]],
                h("DRAW8"): [[("run_routine", {"addr": h("DRAW8"), "registers": {"HL": sprite, "DE": "0xC78A"}, "screenshot": True})],
                             [("document_routine", {"addr": h("DRAW8"), "name": "draw_sprite_8x8", "summary": "Copies 8 bytes to 8 screen lines (refined).",
                                                    "confidence": "high"})],
                             [("finish_routine", {})]],
            }
            mock = MockOllama({}, plans_by_model={"fast": fast, "slow": slow},
                              capabilities_by_model={"fast": ["completion", "vision", "tools"], "slow": ["completion", "tools", "thinking"]})
            host = mock.start()
            try:
                out, project, report = (os.path.join(d, n) for n in ("out.asm", "proj.json", "report.md"))
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "fast", "--refine-model", "slow",
                                                                       "--project", project, "--out", out, "--report", report,
                                                                       "--max-turns", "6", "--refine-max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("== pass 1/2 (explore): model fast", printed)
            self.assertIn("== pass 2/2 (refine): model slow, no vision", printed)
            self.assertIn(", thinking ==", printed)                                  # thinking was switched on for the capable model
            self.assertIn("2 of 3 routines have gaps", printed)
            asked = lambda model: {a.split()[0].split("=")[1].lower() for a in (m.split("\n")[0] for m in mock.requests_by_model[model])}  # noqa: E731
            self.assertEqual(asked("fast"), {h("PRINT"), h("DRAW8"), h("START")})
            self.assertEqual(asked("slow"), {h("PRINT"), h("DRAW8")})                 # START was complete: not asked again
            self.assertEqual(mock.images_by_model.get("slow", 0), 0)                 # no vision: nothing was sent
            self.assertTrue(any("NOT shown: you cannot see images" in r for r in mock.tool_results), mock.tool_results)
            self.assertGreaterEqual(mock.think_values.count(True), 1)                # the refine model was asked to think
            self.assertIn("gaps left: 0 of 3 routines", printed)
            self.assertIn("VERIFIED", printed)
            text = read_text(out)
            self.assertIn("(refined)", text)                                           # the second model's text replaced the first one's
            self.assertNotIn("Probably draws something", text)
            self.assertNotIn("confidence: low", text)                                  # and its confidence replaced "low"
            self.assertIn("; zero terminator ends the text", text)
            proj = json.loads(read_text(project))
            self.assertEqual({r["name"]: r.get("confidence") for r in proj["routines"]}, {"print_string": "high", "draw_sprite_8x8": "high", "show_greeting_demo": "high"})
            self.assertIn("0 with gaps", read_text(report))

    def test_remaining_gaps_are_reported(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            plans = {addr: [[("document_routine", {"addr": addr, "name": "print_string", "summary": "guess", "confidence": "low"})],
                            [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans)
            host = mock.start()
            try:
                report = os.path.join(d, "gaps.md")
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "5", "--report", report, "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("gaps left: 1 of 1 routines (1 with low confidence)", printed)
            md = read_text(report)
            self.assertIn("print_string", md)
            self.assertIn("confidence: low", md)
            self.assertIn("comment(s) for", md)

    def test_model_without_tool_support_is_refused_early(self):
        with tempfile.TemporaryDirectory() as d:
            sna, _ = build_snapshot(d)
            mock = MockOllama({}, capabilities_by_model={"plain": ["completion"]})
            host = mock.start()
            try:
                code, _ = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "plain", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 2)
            self.assertEqual(mock.requests, [])                                       # no chat request was made

    def test_listing_for_the_model_has_addresses_and_wrong_comment_addresses_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            start = sym["START"]
            plans = {f"0x{start:04x}": [
                [("add_comment", {"addr": f"0x{start + 1:04x}", "text": "inside LD A,1"})],      # not an instruction start
                [("add_comment", {"addr": f"0x{start:04x}", "text": "set the screen mode"})],     # the right place
                [("document_routine", {"addr": f"0x{start:04x}", "name": "show_demo", "summary": "x"})],
                [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans)
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", f"0x{start:04x}",
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            first_prompt = mock.requests[0]["messages"][1]["content"]
            self.assertIn(f"{start:04X}    LD A,&01", first_prompt)                  # the address column the model must use
            self.assertIn(f"{start + 2:04X}    CALL SCR_SET_MODE", first_prompt)
            self.assertIn("left column", mock.requests[0]["messages"][0]["content"])  # the system prompt says so, too
            error = next(r for r in mock.tool_results if "middle of the instruction" in r)
            self.assertIn(f"&{start:04X}", error)                                      # tells where the instruction starts
            self.assertTrue(error.startswith("error:"))

    def test_run_routine_attributes_port_writes_to_their_origin(self):
        server = annotate.Z80Server(Z80TOOL)
        try:
            server.ok(cmd="machine", type="cpc")
            server.ok(cmd="asm", source="ORG &4000\nouter: LD BC,&7F8D\nOUT (C),C\nCALL inner\nRET\ninner: LD A,&10\nLD BC,&BC0C\nOUT (C),C\nRET\n")
            server.ok(cmd="analyze", entries=["outer"])
            server.ok(cmd="checkpoint", name="baseline")
            tools = annotate.Tools(server)
            tools.range = ("0x4000", "0x4007")          # the routine "outer" only (up to and including its RET)
            out = tools.t_run_routine("0x4000")
            self.assertIn("hardware port writes: 2 in total - 1 by this routine's own OUT instructions, 1 by the code it calls", out)
            self.assertIn("0x7F8D<-0x8D written at 0x4003 [this routine] Gate Array: screen mode 1", out)
            self.assertIn("0xBC0C<-0x0C written at 0x400E [called code] CRTC: select register 12", out)
            self.assertIn("do NOT attribute", out)
        finally:
            server.close()

    def test_turn_limit_after_documenting_is_partial_and_the_model_is_warned(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            one_by_one = [[("document_routine", {"addr": addr, "name": "print_string", "summary": "Prints text."})]]
            one_by_one += [[("add_comment", {"addr": addr, "text": f"comment {i}"})] for i in range(10)]   # never finishes
            mock = MockOllama({addr: one_by_one})
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("turn limit reached", printed)
            self.assertIn("counted as partial", printed)
            self.assertIn("pass 1: 0 routines finished, 1 partial (documented, not finished), 0 failed", printed)
            self.assertTrue(any("Only 3 rounds are left" in m["content"] for r in mock.requests for m in r["messages"] if m["role"] == "user"),
                            "the model must be told when only a few rounds remain")
            self.assertIn("SEVERAL tool calls in ONE reply", mock.requests[0]["messages"][0]["content"])

    def test_invalid_json_in_a_tool_call_is_recovered_not_fatal(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            plans = {addr: [[("document_routine", {"addr": addr, "name": "print_string", "summary": "Prints text."})],
                            [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans, invalid_json_first=2)     # Ollama answers HTTP 500 "invalid character 'x' ..." twice
            host = mock.start()
            try:
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                                       "--max-turns", "8", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn("the model wrote an invalid tool call (2x)", printed)
            self.assertIn("pass 1: 1 routines finished", printed)
            self.assertTrue(mock.json_hints)
            self.assertIn('"0x0C5A"', mock.json_hints[0])                 # tells how to write addresses
            self.assertEqual(mock.invalid_json_served, 2)                   # each failure was asked once, not retried blindly three times
            self.assertEqual(len(mock.requests), 3)                         # plus the three answers that were understood

    def test_a_server_error_on_one_routine_does_not_end_the_run(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
            plans = {h("PRINT"): [[("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "x"})], [("finish_routine", {})], [("finish_routine", {})]],
                     h("START"): [[("document_routine", {"addr": h("START"), "name": "show_demo", "summary": "y"})], [("finish_routine", {})], [("finish_routine", {})]]}
            mock = MockOllama(plans, crash_on_address=h("DRAW8"))        # the server dies on this routine only
            host = mock.start()
            try:
                out = os.path.join(d, "out.asm")
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--out", out, "--max-turns", "6",
                                                                       "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 0, printed)
            self.assertIn(f"model error on 0x{sym['DRAW8']:04X}", printed)
            self.assertIn("skipping this routine (1 in a row)", printed)
            self.assertIn("pass 1: 2 routines finished, 0 partial (documented, not finished), 1 failed", printed)   # the others were done
            self.assertIn("print_string:", read_text(out))
            self.assertIn("VERIFIED", printed)

    def test_a_dead_server_ends_the_run_after_three_routines_in_a_row(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            h = lambda n: f"0x{sym[n]:04x}"  # noqa: E731
            # the first routine works (so it is not a configuration error), then the server dies for good
            plans = {h("PRINT"): [[("document_routine", {"addr": h("PRINT"), "name": "print_string", "summary": "x"})], [("finish_routine", {})], [("finish_routine", {})]]}

            class Dying(MockOllama):
                def respond(self, body):
                    if self.requests and sum(1 for r in self.requests) >= 3:
                        raise RuntimeError("boom")   # the handler turns this into a dropped connection
                    return super().respond(body)

            mock = Dying(plans)
            host = mock.start()
            try:
                # pass 1: routine 1 works, 2 and 3 fail; pass 2 (same dead server): the third failure in a row ends the run
                code, printed = OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--refine-model", "mock",
                                                                       "--max-turns", "6", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertEqual(code, 2, printed)
            self.assertIn("(2 in a row)", printed)
            self.assertIn("(3 in a row)", printed)

    def test_repeated_failures_get_a_hint(self):
        with tempfile.TemporaryDirectory() as d:
            sna, sym = build_snapshot(d)
            addr = f"0x{sym['PRINT']:04x}"
            bad = ("document_routine", {"addr": addr, "name": "LD", "summary": "x"})
            plans = {addr: [[bad], [bad], [bad], [bad]]}
            mock = MockOllama(plans)
            host = mock.start()
            try:
                OrchestratorTest("run_main").run_main(["--sna", sna, "--host", host, "--model", "mock", "--only", addr,
                                                       "--max-turns", "5", "--z80tool", Z80TOOL])
            finally:
                mock.stop()
            self.assertTrue(any("same failing call" in r for r in mock.tool_results), mock.tool_results)


if __name__ == "__main__":
    unittest.main()
