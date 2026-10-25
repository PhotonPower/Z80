#!/usr/bin/env python3
"""Annotates a Z80 / Amstrad CPC program with the help of a local LLM (Ollama, e.g. Qwen3-VL).

The model never edits code. It works one subroutine at a time and may

  * read the annotated listing, disassemble and read memory,
  * RUN the routine in the emulator (with chosen registers / memory) and look at what changed:
    registers, memory, firmware calls, hardware port writes, text output and - as an image -
    the screen,
  * give names and comments (labels, routine documentation, variables, data regions).

Everything the model says is stored as names and comments only; the final listing is generated from
the real bytes and verified by assembling it again, so it always describes exactly the same program.

Requires only the Python standard library, a built `z80tool`, and a running Ollama server with a
tool-capable vision model (default `qwen3-vl:8b`; use `--model` for another tag).

Example:
    python3 agent/z80_annotate.py --sna game.sna --rom os6128.rom --project game.z80proj.json \\
        --out game_annotated.asm --lang de
"""
import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

# --------------------------------------------------------------------------------------------
# talking to the emulator


class ServerError(RuntimeError):
    pass


class Z80Server:
    """`z80tool serve` as a subprocess: one JSON line in, one JSON line out."""

    def __init__(self, exe, allow_files=True):
        cmd = [exe, "serve"] + (["--allow-files"] if allow_files else [])
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace", bufsize=1)

    def call(self, **request):
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise ServerError("z80tool serve terminated unexpectedly")
        return json.loads(line)

    def ok(self, **request):
        """Like call(), but raises on {"ok": false}."""
        reply = self.call(**request)
        if not reply.get("ok"):
            raise ServerError(reply.get("error", "unknown error") + (" " + json.dumps(reply["errors"]) if "errors" in reply else ""))
        return reply

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        finally:
            self.proc.stdout.close()


# --------------------------------------------------------------------------------------------
# talking to Ollama


class OllamaError(RuntimeError):
    pass


class Ollama:
    retry_delay = 2  # seconds, multiplied by the attempt number

    def __init__(self, host, model, timeout=900, think=None, num_ctx=16384, temperature=0.2):
        self.url = host.rstrip("/") + "/api/chat"
        self.model, self.timeout, self.think = model, timeout, think
        self.options = {"num_ctx": num_ctx, "temperature": temperature}

    def chat(self, messages, tools):
        body = {"model": self.model, "messages": messages, "tools": tools, "stream": False, "options": self.options}
        if self.think is not None:
            body["think"] = self.think
        data = json.dumps(body).encode()
        last = None
        for attempt in range(3):
            try:
                req = urllib.request.Request(self.url, data=data, headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    return json.loads(resp.read())
            except urllib.error.HTTPError as e:  # the server answered: do not retry client errors
                detail = e.read().decode(errors="replace")[:500]
                if 400 <= e.code < 500:
                    raise OllamaError(f"Ollama returned HTTP {e.code}: {detail}") from e
                last = OllamaError(f"Ollama returned HTTP {e.code}: {detail}")
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                last = OllamaError(f"cannot reach Ollama at {self.url}: {e}")
            time.sleep(self.retry_delay * (attempt + 1))
        raise last


# --------------------------------------------------------------------------------------------
# tool definitions (what the model sees)


def _fn(name, description, properties, required=()):
    return {"type": "function", "function": {"name": name, "description": description,
                                              "parameters": {"type": "object", "properties": properties, "required": list(required)}}}


ADDR = {"type": "string", "description": 'Address as a hexadecimal string such as "0x4010", or a label name. Plain numbers in JSON are decimal.'}

TOOLS = [
    _fn("get_listing", "Annotated listing of an address range, as currently named and commented.",
        {"from": ADDR, "to": ADDR}, ["from", "to"]),
    _fn("disassemble", "Disassemble `count` instructions starting at an address.",
        {"addr": ADDR, "count": {"type": "integer"}}, ["addr"]),
    _fn("read_memory", "Hex dump of memory (RAM view).", {"addr": ADDR, "length": {"type": "integer"}}, ["addr"]),
    _fn("run_routine",
        "Run the routine at `addr` in the emulator like a function call and report what it did: registers afterwards, "
        "memory it changed, firmware calls, hardware port writes, text output and how the screen changed. The machine is "
        "restored afterwards unless keep_state is true. Use this to TEST your hypotheses instead of guessing.",
        {"addr": ADDR,
         "registers": {"type": "object", "description": 'Initial registers, e.g. {"A": 5, "HL": "0x4100"}'},
         "memory": {"type": "array", "description": 'Memory to set up first: [{"addr": "0x4100", "hex": "48454C4C4F00"}]',
                    "items": {"type": "object", "properties": {"addr": ADDR, "hex": {"type": "string"}}}},
         "max_instructions": {"type": "integer", "description": "default 200000"},
         "screenshot": {"type": "boolean", "description": "attach an image of the screen (zoomed to the changed area) after the run"},
         "keep_state": {"type": "boolean", "description": "keep the changes (then call reset_machine when done)"}},
        ["addr"]),
    _fn("screenshot", "Image of the current screen. Optional crop box in text cells (80 columns x 25 rows) and zoom.",
        {"col0": {"type": "integer"}, "row0": {"type": "integer"}, "col1": {"type": "integer"}, "row1": {"type": "integer"},
         "zoom": {"type": "integer"}}),
    _fn("reset_machine", "Restore the machine to the state it had when the analysis started.", {}),
    _fn("name_label", "Give an address (code or data) a meaningful name, e.g. a loop or entry point.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case, ASCII, unique"}}, ["addr", "name"]),
    _fn("document_routine", "Name and document the subroutine. Required before finish_routine.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case verb phrase, e.g. draw_sprite"},
         "summary": {"type": "string", "description": "what it does, 1-3 sentences"},
         "inputs": {"type": "string", "description": 'registers/memory read, e.g. "HL = source, DE = screen address"'},
         "outputs": {"type": "string", "description": "results and return values"},
         "clobbers": {"type": "string", "description": 'registers destroyed, e.g. "AF BC"'}},
        ["addr", "name", "summary"]),
    _fn("name_variable", "Name a memory location the code reads/writes (variable, flag, pointer, buffer, table).",
        {"addr": ADDR, "name": {"type": "string"}, "size": {"type": "integer", "description": "bytes, default 1"},
         "comment": {"type": "string", "description": "meaning and unit"}}, ["addr", "name"]),
    _fn("mark_data", "Declare a range as data (not code) and how to display it.",
        {"addr": ADDR, "length": {"type": "integer"}, "kind": {"type": "string", "enum": ["bytes", "words", "text", "pointers"]},
         "name": {"type": "string"}, "comment": {"type": "string"}}, ["addr", "length", "kind"]),
    _fn("add_comment", "Comment for one line of code (appears behind the instruction), or a block comment above it.",
        {"addr": ADDR, "text": {"type": "string"}, "block": {"type": "boolean"}}, ["addr", "text"]),
    _fn("finish_routine", "Call when the routine is fully documented and commented.", {"summary": {"type": "string"}}),
]

# --------------------------------------------------------------------------------------------
# helpers


def _coerce(value, schema):
    """Converts a model-supplied value to the JSON type the schema asks for (models are sloppy about this)."""
    kind = schema.get("type")
    if kind == "boolean":
        if isinstance(value, str):
            low = value.strip().lower()
            if low in ("true", "yes", "1"):
                return True
            if low in ("false", "no", "0", ""):
                return False
            raise ValueError(f"expected true or false, got {value!r}")
        return bool(value)
    if kind == "integer":
        if isinstance(value, bool):
            raise ValueError("expected a number")
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, str):
            try:
                return int(value.strip(), 0)
            except ValueError:
                raise ValueError(f"expected a number, got {value!r}") from None
        if isinstance(value, int):
            return value
        raise ValueError(f"expected a number, got {value!r}")
    if kind in ("object", "array"):
        if isinstance(value, str):  # JSON written as a string
            try:
                value = json.loads(value)
            except json.JSONDecodeError:
                raise ValueError(f"expected {kind} (JSON), got text") from None
        if kind == "object" and not isinstance(value, dict) or kind == "array" and not isinstance(value, list):
            raise ValueError(f"expected {kind}")
        return value
    if kind == "string":
        if schema is ADDR and isinstance(value, (int, float)) and not isinstance(value, bool):
            return int(value)  # a JSON number is a decimal address; it must not turn into a "hex" string
        return value if isinstance(value, str) else str(value)
    return value


def prepare_args(tool_name, args):
    """Validates and normalises tool arguments against the tool's schema.

    Returns (clean_args, notes). Unknown arguments are dropped with a note; wrong types are converted when
    that is unambiguous. Raises ValueError with a message the model can act on.
    """
    spec = next((t["function"] for t in TOOLS if t["function"]["name"] == tool_name), None)
    if spec is None:
        raise ValueError(f"unknown tool '{tool_name}'. Available tools: " + ", ".join(t["function"]["name"] for t in TOOLS))
    props = spec["parameters"]["properties"]
    required = spec["parameters"].get("required", [])
    clean, notes = {}, []
    for key, value in args.items():
        if key not in props:
            notes.append(f"ignored unknown argument '{key}' (valid: {', '.join(props) or 'none'})")
            continue
        if value is None:
            continue
        try:
            clean[key] = _coerce(value, props[key])
        except ValueError as e:
            raise ValueError(f"argument '{key}': {e}") from None
    missing = [k for k in required if k not in clean]
    if missing:
        raise ValueError(f"missing required argument(s): {', '.join(missing)}. Valid arguments: {', '.join(props)}")
    return clean, notes


def clean_tool_name(name):
    """'functions.run_routine', 'run_routine()' and 'Run_Routine' all mean run_routine."""
    name = (name or "").strip().rstrip("()").split(".")[-1]
    known = {t["function"]["name"] for t in TOOLS}
    if name in known:
        return name
    return name.lower() if name.lower() in known else name


_TOOL_CALL_TAG = re.compile(r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)", re.S)


def calls_from_content(content):
    """Some chat templates make the model write the call as text instead of using `tool_calls`.

    Understands <tool_call>{"name": ..., "arguments": {...}}</tool_call> and a bare JSON object (optionally in a
    ```json fence) with the same keys. Returns a list of {"function": {...}} dicts (empty if there is none).
    """
    if not content:
        return []
    candidates = _TOOL_CALL_TAG.findall(content)
    if not candidates:
        fenced = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", content, re.S)
        candidates = fenced or ([content.strip()] if content.strip().startswith("{") else [])
    calls = []
    for text in candidates:
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            continue
        for item in obj if isinstance(obj, list) else [obj]:
            if isinstance(item, dict) and isinstance(item.get("name"), str) and "arguments" in item:
                calls.append({"function": {"name": item["name"], "arguments": item["arguments"]}})
    return calls


def compact_messages(messages, max_chars, keep_images=2):
    """Keeps the conversation within a size budget.

    Images are the first thing to go (only the newest `keep_images` stay), then the oldest tool results are
    shortened. The system prompt, the task and the most recent exchange are never touched.
    """
    with_images = [m for m in messages if m.get("images")]
    for m in with_images[:-keep_images] if keep_images else with_images:
        m["content"] = (m.get("content") or "") + " [image removed to save context]"
        del m["images"]

    def size():
        return sum(len(m.get("content") or "") + sum(len(i) for i in m.get("images", [])) // 20 for m in messages)

    for m in messages[2:-4]:
        if size() <= max_chars:
            break
        if m["role"] == "tool" and len(m["content"]) > 200:
            m["content"] = m["content"][:150] + " ... [older tool output removed to save context]"
    return messages


def norm_addr(value):
    """Normalise an address argument given by the model. Strings of 3-4 bare hex digits are hex."""
    if isinstance(value, bool):
        raise ValueError("an address is required")
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if not isinstance(value, str) or not value.strip():
        raise ValueError("an address is required")
    v = value.strip()
    if re.fullmatch(r"[0-9A-Fa-f]{3,4}", v) and not re.fullmatch(r"[0-9]{1,3}", v):
        return "0x" + v
    return v


def trim(text, limit):
    return text if len(text) <= limit else text[:limit] + f"\n... [{len(text) - limit} more characters cut]"


# --------------------------------------------------------------------------------------------
# tool implementations


class Tools:
    def __init__(self, server, irq=0):
        self.s = server
        self.irq = irq
        self.images = []           # base64 PNGs waiting to be shown to the model
        self.finished = False

    def reset_routine(self):
        self.images.clear()
        self.finished = False

    def dispatch(self, name, args):
        name = clean_tool_name(name)
        if isinstance(args, str):
            try:
                args = json.loads(args) if args.strip() else {}
            except json.JSONDecodeError:
                return "error: arguments must be a JSON object"
        if not isinstance(args, dict):
            return "error: arguments must be an object"
        try:
            clean, notes = prepare_args(name, args)
            fn = getattr(self, "t_" + name, None)
            if fn is None:
                return f"error: unknown tool '{name}'"
            result = fn(**clean)
            return result + ("\n(" + "; ".join(notes) + ")" if notes else "")
        except TypeError as e:
            return f"error: bad arguments for {name}: {e}"
        except (ServerError, ValueError) as e:
            return f"error: {e}"

    # ---- reading ----
    def t_get_listing(self, **a):
        r = self.s.ok(cmd="listing", verify=False, max_chars=24000, **{"from": norm_addr(a["from"]), "to": norm_addr(a["to"])})
        return r["text"] + ("\n[listing truncated]" if r.get("truncated") else "")

    def t_disassemble(self, addr, count=16):
        r = self.s.ok(cmd="dis", addr=norm_addr(addr), count=max(1, min(int(count), 200)))
        return "\n".join(f'{l["addr"]}  {l["bytes"]:<10} {l["text"]}' + (f'   <{l["label"]}>' if "label" in l else "") for l in r["lines"])

    def t_read_memory(self, addr, length=64):
        r = self.s.ok(cmd="read", addr=norm_addr(addr), len=max(1, min(int(length), 1024)))
        h = r["hex"]
        rows = [f'{r["addr"]}+{i // 2:03X}: ' + " ".join(h[i + j:i + j + 2] for j in range(0, min(32, len(h) - i), 2)) for i in range(0, len(h), 32)]
        return "\n".join(rows) + f'\nascii: {r["ascii"]}'

    # ---- running ----
    def t_run_routine(self, addr, registers=None, memory=None, max_instructions=200000, screenshot=False, keep_state=False):
        s = self.s
        s.ok(cmd="rollback", name="baseline")
        for m in memory or []:
            if not isinstance(m, dict) or "addr" not in m or "hex" not in m:
                raise ValueError('each memory entry needs "addr" and "hex"')
            digits = re.sub(r"0x|[\s,]", "", str(m["hex"]), flags=re.I)
            s.ok(cmd="write", addr=norm_addr(m["addr"]), hex=digits)
        regs = {k: (norm_addr(v) if isinstance(v, str) else v) for k, v in (registers or {}).items()}
        s.ok(cmd="firmware_log", clear=True)
        s.ok(cmd="console", clear=True)
        s.ok(cmd="io")
        s.ok(cmd="screen", name="_before")
        limit = max(1, min(int(max_instructions), 5_000_000))  # a runaway loop must not stall the whole analysis
        reply = s.call(cmd="call", addr=norm_addr(addr), regs=regs, max_instructions=limit, irq_every_cycles=self.irq)
        if not reply.get("ok"):
            return f"error: {reply.get('error')}"
        out = [f'stop: {reply["stop"]}  returned: {reply.get("returned")}  instructions: {reply["instructions"]}  T-states: {reply["cycles"]}']
        r = reply["regs"]
        out.append("registers after: " + "  ".join(f"{k}={r[k]}" for k in ("AF", "BC", "DE", "HL", "IX", "IY", "SP")) + f'  flags={r["flags"]}')
        if reply["stop"] != "address":
            out.append("NOTE: the routine did not return normally (check the stop reason).")

        sp = int(r["SP"], 16)
        diff = s.ok(cmd="diff", name="baseline")
        shown = [x for x in diff["ranges"] if not (sp - 0x40 <= int(x["addr"], 16) < sp + 0x40)]
        if shown:
            out.append(f'memory changed ({diff["changed_bytes"]} bytes; stack area omitted):')
            for x in shown[:12]:
                out.append(f'  {x["addr"]} len {x["len"]}: {x["before"][:32]} -> {x["after"][:32]}')
        else:
            out.append("memory changed: nothing outside the stack")

        fw = s.ok(cmd="firmware_log")["calls"]
        if fw:
            out.append("firmware calls (stubbed): " + "; ".join(f'{c["name"]}(A={c["A"]},BC={c["BC"]},DE={c["DE"]},HL={c["HL"]})' for c in fw[:12]) + (" ..." if len(fw) > 12 else ""))
        text = s.ok(cmd="console")["text"]
        if text:
            out.append(f"text printed via firmware: {text!r}")
        ports = s.ok(cmd="io")["output"]
        if ports:
            out.append("hardware port writes: " + "; ".join(f'{p["port"]}<-{p["value"]}' + (f' ({p["what"]})' if "what" in p else "") for p in ports[:12]) + (" ..." if len(ports) > 12 else ""))

        d = s.ok(cmd="screen_diff", a="_before")
        if d.get("changed_pixels", 0) == 0:
            out.append("screen: unchanged")
        else:
            c = d["text_cells"]
            out.append(f'screen: {d["changed_pixels"]} pixels changed ({d["changed_percent"]:.2f}%), '
                       f'area columns {c["col0"]}-{c["col1"]}, rows {c["row0"]}-{c["row1"]} of 80x25 text cells')
        if screenshot:
            if d.get("changed_pixels", 0) > 0:
                c = d["text_cells"]
                crop = {"col0": max(c["col0"] - 2, 0), "row0": max(c["row0"] - 1, 0), "col1": min(c["col1"] + 2, 79), "row1": min(c["row1"] + 1, 24)}
                shot = s.ok(cmd="screen", name="_after", crop=crop)
                out.append(f"(image attached: columns {crop['col0']}-{crop['col1']}, rows {crop['row0']}-{crop['row1']}, zoom {shot['zoom']}x)")
            else:
                shot = s.ok(cmd="screen", name="_after")
                out.append("(image attached: full screen)")
            self.images.append(shot["png_base64"])
        if not keep_state:
            s.ok(cmd="rollback", name="baseline")
        else:
            out.append("state kept; call reset_machine to restore.")
        return "\n".join(out)

    def t_screenshot(self, col0=None, row0=None, col1=None, row1=None, zoom=None):
        req = {"cmd": "screen", "name": "_view"}
        if None not in (col0, row0, col1, row1):
            req["crop"] = {"col0": col0, "row0": row0, "col1": col1, "row1": row1}
        if zoom:
            req["zoom"] = int(zoom)
        shot = self.s.ok(**req)
        self.images.append(shot["png_base64"])
        return f'(image attached: {shot["width"]}x{shot["height"]} px, screen mode {shot["mode"]})'

    def t_reset_machine(self):
        self.s.ok(cmd="rollback", name="baseline")
        return "machine restored"

    # ---- annotating ----
    def t_name_label(self, addr, name):
        self.s.ok(cmd="ann_label", addr=norm_addr(addr), name=name)
        return f"ok: {name}"

    def t_document_routine(self, addr, name, summary, inputs="", outputs="", clobbers=""):
        a = norm_addr(addr)
        self.s.ok(cmd="ann_routine", addr=a, name=name, summary=summary, inputs=inputs, outputs=outputs, clobbers=clobbers)
        return f"ok: routine {name} documented"

    def t_name_variable(self, addr, name, size=1, comment=""):
        self.s.ok(cmd="ann_variable", addr=norm_addr(addr), name=name, size=int(size), comment=comment)
        return f"ok: variable {name}"

    def t_mark_data(self, addr, length, kind, name="", comment=""):
        self.s.ok(cmd="ann_data", addr=norm_addr(addr), length=int(length), kind=kind, name=name, comment=comment)
        return "ok: data region marked"

    def t_add_comment(self, addr, text, block=False):
        self.s.ok(cmd="ann_comment", addr=norm_addr(addr), text=text, block=bool(block))
        return "ok"

    def t_finish_routine(self, summary=""):
        still_open = {r["addr"].lower() for r in self.s.ok(cmd="routines", undocumented_only=True)["routines"]}
        if self.current and self.current.lower() in still_open:
            return "error: call document_routine for this routine first (name, summary, inputs, outputs, clobbers)."
        self.finished = True
        return "ok: routine finished"

    current = None  # address (hex string) of the routine being processed


# --------------------------------------------------------------------------------------------
# the annotation workflow

SYSTEM_PROMPT = """You are an expert Z80 and Amstrad CPC reverse engineer. You document ONE subroutine at a time so that the final
assembler listing reads like the original source code, with meaningful names and helpful comments.

How you work:
1. Read the listing of the routine. Firmware calls (e.g. TXT_OUTPUT, SCR_SET_MODE) and hardware ports (GATE_ARRAY, CRTC_SELECT, PPI_*)
   already appear by name; the comments next to OUT instructions explain the hardware effect. Routines called by this one that are already
   documented appear with their real names.
2. Form a hypothesis, then TEST it: use run_routine with suitable registers/memory, and ask for a screenshot when the routine draws
   something. Compare registers, memory changes, port writes and the screen change with what you expected. Do not guess when you can run it.
3. Record what you learned with the annotation tools: document_routine (name, summary, inputs, outputs, clobbers), name_variable for
   every memory location that has a meaning, mark_data for tables/strings/sprites, name_label for loops and branch targets,
   add_comment for the non-obvious lines.
4. Call finish_routine when done.

Naming: snake_case ASCII, unique, describing purpose not address. Routines start with a verb (draw_sprite, wait_for_vsync),
variables are nouns (player_x, lives_left), flags end in a state (sound_enabled). Never reuse register names, mnemonics or firmware
names. Comments explain WHY and meaning, not what the instruction obviously does; keep them short. Mention units (pixels, bytes,
characters) and valid ranges when you know them. If you are not sure, say so in the comment ("probably ...") instead of inventing details.
Screenshots show the CPC screen scaled to 640 pixels width; the "text cells" in tool results are 80 columns x 25 rows.
Write all summaries and comments in {lang}.
"""


class Annotator:
    def __init__(self, server, ollama, args, log=None):
        self.s, self.llm, self.a, self.log = server, ollama, args, log
        self.tools = Tools(server, irq=getattr(args, 'irq', 0))

    def say(self, text):
        print(text, flush=True)

    def log_event(self, **ev):
        if self.log:
            self.log.write(json.dumps(ev, default=str) + "\n")
            self.log.flush()

    # ---- setup ----
    def setup(self):
        a, s = self.a, self.s
        s.ok(cmd="machine", type="cpc")
        for rom in a.rom or []:
            path, _, which = rom.partition(":")
            s.ok(cmd="load_rom", file=path, which=which or "lower")
        if a.boot:
            if not a.rom:
                raise ServerError("--boot needs a firmware ROM (--rom FILE:both)")
            s.ok(cmd="set", regs={"PC": 0, "SP": "0xBFFF"})
            r = s.ok(cmd="run", irq_every_cycles=13312, max_instructions=a.boot_instructions)
            self.say(f'firmware booted ({r["instructions"]} instructions, stop: {r["stop"]})')
        if a.sna:
            r = s.ok(cmd="load_sna", file=a.sna)
            self.say(f'snapshot loaded, PC={r["regs"]["PC"]}, screen mode {r["video"]["mode"]}')
        else:
            s.ok(cmd="load_file", file=a.bin, addr=a.org)
        if a.project and os.path.exists(a.project):
            n = s.ok(cmd="ann_load", file=a.project)["labels"]
            self.say(f"project loaded ({n} labels)")
        entries = a.entry or ([a.org] if a.bin else [s.ok(cmd="regs")["regs"]["PC"]])
        extra = {"inline_strings": a.inline_string} if a.inline_string else {}
        r = s.ok(cmd="analyze", entries=entries, **extra)
        self.say(f'analysed {r["instructions"]} instructions, {r["from"]}-{r["to"]}')
        s.ok(cmd="checkpoint", name="baseline")

    # ---- one routine ----
    def routine_prompt(self, r):
        addr = r["addr"]
        end = f'0x{int(addr, 16) + max(r["size"], 1) - 1:04X}'
        listing = self.s.ok(cmd="listing", verify=False, max_chars=14000, **{"from": addr, "to": end})
        callees = []
        for c in r["calls"]:
            if "name" in c:
                callees.append(f'  {c["name"]} ({c["addr"]})')
            elif "firmware" in c:
                callees.append(f'  {c["firmware"]} (firmware)')
            else:
                callees.append(f'  sub_{c["addr"][2:]} ({c["addr"]}, not documented yet)')
        callers = ", ".join(r["callers"]) or "none (entry point)"
        text = (f'ROUTINE_ADDR={addr}  current name: {r["name"]}  size: {r["size"]} bytes  ends with: {r["ends_with"]}\n'
                f'Called from: {callers}\nCalls:\n' + ("\n".join(callees) if callees else "  nothing") +
                f"\n\nListing:\n```\n{listing['text']}\n```\n{'[listing truncated; use get_listing for the rest]' if listing.get('truncated') else ''}\n"
                "Analyse this routine, test it, annotate it and call finish_routine.")
        return text

    def process_routine(self, r):
        t = self.tools
        t.reset_routine()
        t.current = r["addr"]
        messages = [{"role": "system", "content": SYSTEM_PROMPT.format(lang=self.a.lang)},
                    {"role": "user", "content": self.routine_prompt(r)}]
        nudges = 0
        repeats = {}
        try:
            for turn in range(self.a.max_turns):
                compact_messages(messages, max_chars=self.a.num_ctx * 3)
                resp = self.llm.chat(messages, TOOLS)
                msg = resp.get("message", {})
                self.log_event(routine=r["addr"], turn=turn, message=msg)
                echoed = {k: v for k, v in msg.items() if k in ("content", "tool_calls")}
                echoed["role"] = "assistant"
                messages.append(echoed)
                calls = msg.get("tool_calls") or calls_from_content(msg.get("content"))
                if not calls:
                    nudges += 1
                    if nudges > 2:
                        self.say(f'  model stopped without finishing {r["addr"]}')
                        return False
                    messages.append({"role": "user", "content": "Continue with tool calls. When the routine is documented, call finish_routine."})
                    continue
                for call in calls:
                    fn = call.get("function", {})
                    name, args = clean_tool_name(fn.get("name", "")), fn.get("arguments", {})
                    result = t.dispatch(name, args)
                    key = (name, json.dumps(args, sort_keys=True, default=str))
                    if result.startswith("error"):
                        repeats[key] = repeats.get(key, 0) + 1
                        if repeats[key] >= 3:
                            result += "\nYou have made this same failing call several times. Change your approach."
                    self.log_event(routine=r["addr"], tool=name, args=args, result=trim(result, 2000))
                    messages.append({"role": "tool", "tool_name": name, "content": trim(result, 6000)})
                if t.images:
                    messages.append({"role": "user", "content": "Screenshot(s) from the last tool call:", "images": list(t.images)})
                    t.images.clear()
                if t.finished:
                    return True
            self.say(f'  turn limit reached for {r["addr"]}')
            return False
        finally:
            t.current = None

    # ---- everything ----
    def run(self):
        s = self.s
        done = failed = 0
        for round_ in range(3):  # documenting a routine can reveal that others are still open
            routines = s.ok(cmd="routines", undocumented_only=True)["routines"]
            if self.a.only:
                routines = [r for r in routines if r["addr"].lower() in {x.lower() for x in self.a.only}]
            routines = [r for r in routines if r["addr"] not in getattr(self, "_tried", set())]
            if not routines:
                break
            self._tried = getattr(self, "_tried", set()) | {r["addr"] for r in routines}
            for i, r in enumerate(routines, 1):
                self.say(f'[{i}/{len(routines)}] {r["name"]} at {r["addr"]} ({r["size"]} bytes)')
                ok = self.process_routine(r)
                done += ok
                failed += not ok
                if self.a.project:
                    s.ok(cmd="ann_save", file=self.a.project)
        listing = s.ok(cmd="listing", verify=True, max_chars=10_000_000)
        if self.a.out:
            with open(self.a.out, "w", encoding="utf-8") as f:
                f.write(listing["text"])
        self.say(f'finished: {done} routines documented, {failed} left open; listing '
                 + ("VERIFIED (re-assembles to the identical bytes)" if listing["verified"] else f'NOT verified: {listing["verify_detail"]}')
                 + (f" -> {self.a.out}" if self.a.out else ""))
        return listing


def find_z80tool(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "build", "z80tool"), os.path.join(here, "..", "build", "Release", "z80tool.exe")):
        if os.path.exists(cand):
            return cand
    found = shutil.which("z80tool")
    if not found:
        sys.exit("z80tool not found: build the project or pass --z80tool")
    return found


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--sna", help="Amstrad CPC snapshot (.sna) of the running program")
    src.add_argument("--bin", help="raw binary (use with --org)")
    p.add_argument("--org", default="0x4000", help="load address of --bin")
    p.add_argument("--entry", action="append", help='entry point (repeatable), default: PC of the snapshot, e.g. "0x4000"')
    p.add_argument("--inline-string", action="append", help="routine that is followed by an inline string (bit-7 terminated), repeatable")
    p.add_argument("--rom", action="append", help="ROM image as FILE:lower|upper|both (without it the firmware calls are stubbed)")
    p.add_argument("--boot", action="store_true", help="cold-start the firmware from the ROM first (for --bin; the snapshot already contains a running machine)")
    p.add_argument("--boot-instructions", type=int, default=30_000_000, help="instructions to run while booting")
    p.add_argument("--irq", type=int, default=0, metavar="CYCLES",
                   help="raise an interrupt every CYCLES T-states while routines run (CPC: 13312 = 300 Hz); 0 = never")
    p.add_argument("--project", help="annotation file (JSON); loaded if present, saved after every routine")
    p.add_argument("--out", help="write the final annotated listing here")
    p.add_argument("--only", action="append", help="process only this routine address (repeatable)")
    p.add_argument("--model", default="qwen3-vl:8b", help="Ollama model tag (must support tools and images)")
    p.add_argument("--host", default=os.environ.get("OLLAMA_HOST", "http://localhost:11434"))
    p.add_argument("--think", choices=["true", "false"], help="enable/disable the model's thinking mode (default: model default)")
    p.add_argument("--num-ctx", type=int, default=16384)
    p.add_argument("--lang", default="English", help='language of summaries and comments, e.g. "German"')
    p.add_argument("--max-turns", type=int, default=20, help="tool-call rounds per routine")
    p.add_argument("--log", help="write every model message and tool result to this JSON-lines file")
    p.add_argument("--z80tool", help="path to the z80tool executable")
    a = p.parse_args(argv)
    if not a.host.startswith("http"):
        a.host = "http://" + a.host

    server = Z80Server(find_z80tool(a.z80tool))
    log = open(a.log, "a", encoding="utf-8") if a.log else None
    try:
        llm = Ollama(a.host, a.model, think=None if a.think is None else a.think == "true", num_ctx=a.num_ctx)
        worker = Annotator(server, llm, a, log)
        worker.setup()
        listing = worker.run()
        return 0 if listing["verified"] else 1
    except (ServerError, OllamaError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        server.close()
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
