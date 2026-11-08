#!/usr/bin/env python3
"""Drives the annotation tools by hand - for an agent (or a person) that plays the part of the model.

`z80_annotate.py` lets a local model call the tools in a loop. Here the caller is somebody who works through a shell: every command starts the
emulator, rebuilds the state from the project file, does ONE thing and ends. The same tools, the same reports and the same completeness check as
in the orchestrator are used, so the result is the same kind of annotated, verified listing.

    python3 agent/z80_drive.py --work W init --bin prog.bin --org 0x4000  # once; more options as for z80_annotate.py (--sna, --entry, --rom ...)
    python3 agent/z80_drive.py --work W status                          # routines in processing order, what is open, what to do next
    python3 agent/z80_drive.py --work W show 0x4025                     # the task text a model would get for this routine (add --system for the instructions)
    python3 agent/z80_drive.py --work W tool run_routine '{"addr":"0x4025","setup":[{"addr":"0x4000"}],"registers":{"HL":"0x4100"}}'
    python3 agent/z80_drive.py --work W tool document_routine '{"addr":"0x4025","name":"print_string","summary":"..."}'
    python3 agent/z80_drive.py --work W tool name_label '{"addr":"0x402A","name":"next_char"}'
    python3 agent/z80_drive.py --work W finish 0x4025                   # checks the listing of the routine (strict; --accept-gaps: a second call is final)
    python3 agent/z80_drive.py --work W listing --out annotated.asm     # the verified listing (assembled again, compared byte for byte)
    python3 agent/z80_drive.py --work W report                          # what is still missing

The tools are those of the orchestrator (see TOOLS in z80_annotate.py): get_listing, disassemble, read_memory, run_routine, reset_machine,
screenshot, remove_annotation, name_label, document_routine, name_variable, mark_data, add_comment, mark_constant, finish_routine. The annotations are kept
in W/project.json after every command; screenshots (run_routine with "screenshot": true, or the screenshot tool) are written to W/shot_N.png.
"""
import argparse
import base64
import contextlib
import importlib.util
import io
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("z80_annotate", os.path.join(HERE, "z80_annotate.py"))
ann = importlib.util.module_from_spec(_spec)
sys.modules["z80_annotate"] = ann
_spec.loader.exec_module(ann)

ANNOTATING = {"name_label", "document_routine", "name_variable", "mark_data", "add_comment", "mark_constant", "finish_routine", "remove_annotation"}


class DriveError(Exception):
    pass


def paths(work):
    return os.path.join(work, "config.json"), os.path.join(work, "state.json"), os.path.join(work, "project.json")


def load_state(work):
    _, state_path, _ = paths(work)
    if os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            return json.load(f)
    return {"finished": [], "warned": []}


def save_state(work, state):
    with open(paths(work)[1], "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)


class Workspace:
    """The emulator with the program loaded and the analysis done, built from the files in the work directory."""

    def __init__(self, work, verbose=False, accept_gaps=False):
        config_path, _, project_path = paths(work)
        if not os.path.exists(config_path):
            raise DriveError(f"{work} is not a work directory: run `init` first")
        with open(config_path, encoding="utf-8") as f:
            argv = json.load(f)["argv"]
        self.work = work
        self.args = ann.build_parser().parse_args(argv)
        self.args.project = project_path
        self.args.accept_gaps = accept_gaps
        self.server = ann.Z80Server(ann.find_z80tool(self.args.z80tool))
        self.worker = ann.Annotator(self.server, [], self.args)
        log = io.StringIO()
        try:
            with contextlib.redirect_stdout(log):
                self.worker.setup()
        except ann.ServerError as e:
            self.server.close()
            raise DriveError(str(e)) from e
        if verbose:
            print(log.getvalue(), end="")
        self.tools = self.worker.tools            # the same tools (and the program range the setup found) as the orchestrator
        self.tools.strict = not accept_gaps
        self.state = load_state(work)

    def routines(self):
        return self.server.ok(cmd="routines")["routines"]

    def routine(self, addr):
        wanted = ann.to_int(ann.norm_addr(addr)) if isinstance(ann.norm_addr(addr), str) and str(ann.norm_addr(addr)).startswith("0x") else int(ann.norm_addr(addr))
        for r in self.routines():
            if ann.to_int(r["addr"]) == wanted:
                return r
        raise DriveError(f"no routine at {addr}; `status` lists them")

    def close(self):
        self.server.close()


def cmd_init(a):
    os.makedirs(a.work, exist_ok=True)
    config_path, state_path, project_path = paths(a.work)
    rest = list(a.rest)
    if not any(x in rest for x in ("--sna", "--bin", "--dsk")):
        raise DriveError("init needs the program: --sna FILE, --bin FILE --org ADDR or --dsk IMAGE --dsk-file NAME (further options as for z80_annotate.py)")
    if "--project" in rest:
        raise DriveError("--project is set by the work directory (W/project.json)")
    ann.build_parser().parse_args(rest + ["--project", project_path])        # reject unknown options now
    for i, key in enumerate(rest[:-1]):                                     # file names must stay valid from any directory
        if key in ("--sna", "--bin", "--dsk"):
            rest[i + 1] = os.path.abspath(rest[i + 1])
        elif key == "--rom":
            path, sep, which = rest[i + 1].partition(":")
            rest[i + 1] = os.path.abspath(path) + sep + which
    with open(config_path, "w", encoding="utf-8") as f:
        json.dump({"argv": rest}, f, indent=1)
    if a.fresh and os.path.exists(project_path):
        os.remove(project_path)
    save_state(a.work, {"finished": [], "warned": []})
    ws = Workspace(a.work, verbose=True)
    try:
        n = len(ws.routines())
    finally:
        ws.close()
    print(f"work directory {a.work} is ready: {n} routines. Next: `status`.")


def routine_states(ws):
    todo = {r["addr"] for r in ws.server.ok(cmd="routines", undocumented_only=True)["routines"]}
    finished = {ann.to_int(x) for x in ws.state["finished"]}
    rows = []
    for r in ws.routines():
        if r["addr"] in todo:
            kind = "open"
        elif ann.to_int(r["addr"]) in finished:
            kind = "finished"
        else:
            kind = "documented, not finished"
        rows.append((r, kind))
    return rows


def cmd_status(a):
    ws = Workspace(a.work)
    try:
        rows = routine_states(ws)
        print(f"{len(rows)} routines, processing order (leaves first); a routine is done when it is documented AND finish_routine accepted it:")
        nxt = None
        for i, (r, kind) in enumerate(rows, 1):
            if kind != "finished" and nxt is None:
                nxt = r
            conf = f", confidence {r['confidence']}" if r.get("confidence") else ""
            print(f'  {i}. {r["name"]:<22} {r["addr"]}  {ann.Annotator.own_bytes(r):>5} B  {kind}{conf}')
        done = sum(1 for _, k in rows if k == "finished")
        print(f"{done} of {len(rows)} finished.")
        if nxt:
            print(f'Next: `show {nxt["addr"]}`')
        else:
            print("All routines are finished. `listing --out FILE` writes the verified listing, `report` shows the gaps that are left.")
    finally:
        ws.close()


def cmd_show(a):
    ws = Workspace(a.work)
    try:
        r = ws.routine(a.addr)
        if a.system:
            print(ann.SYSTEM_PROMPT.format(lang=ws.args.lang))
            print("=" * 100)
        gaps = ws.worker.routine_gaps(r) if r["documented"] else None
        print(ws.worker.routine_prompt(r, gaps or None))
    finally:
        ws.close()


def cmd_tool(a):
    ws = Workspace(a.work, accept_gaps=getattr(a, "accept_gaps", False))
    try:
        try:
            args = json.loads(a.args) if a.args else {}
        except json.JSONDecodeError as e:
            raise DriveError(f"the arguments are not valid JSON: {e}") from e
        name = ann.clean_tool_name(a.name)
        if a.routine or name == "finish_routine":
            addr = a.routine or args.get("addr")
            if name == "finish_routine":
                args.pop("addr", None)       # the routine is not an argument of the tool, it is the context of the call
            if not addr:
                raise DriveError("finish_routine needs the routine: --routine ADDR (or use the `finish` command)")
            r = ws.routine(addr)
            ws.tools.current, ws.tools.range = r["addr"], ann.Annotator.hull(r)
            ws.tools.blocks = ann.Annotator.blocks(r)
            ws.tools.finish_warned = r["addr"] in ws.state["warned"]
        elif name == "run_routine" and "addr" in args:
            try:
                r = ws.routine(args["addr"])
                ws.tools.range = ann.Annotator.hull(r)
                ws.tools.blocks = ann.Annotator.blocks(r)
            except DriveError:
                pass
        result = ws.tools.dispatch(name, args)
        print(result)
        for image in ws.tools.images:                # a person or an agent in a shell looks at screenshots as files
            n = 1
            while os.path.exists(os.path.join(a.work, f"shot_{n}.png")):
                n += 1
            path = os.path.join(a.work, f"shot_{n}.png")
            with open(path, "wb") as f:
                f.write(base64.b64decode(image))
            print(f"(screenshot written to {path})")
        ws.tools.images.clear()
        if name in ANNOTATING and not result.startswith("error"):
            ws.worker.save()
        if name == "finish_routine" and ws.tools.current:
            if ws.tools.finished:
                if ws.tools.current not in ws.state["finished"]:
                    ws.state["finished"].append(ws.tools.current)
                save_state(a.work, ws.state)
            elif result.startswith("not finished yet") and ws.tools.current not in ws.state["warned"]:
                ws.state["warned"].append(ws.tools.current)
                save_state(a.work, ws.state)
        return 1 if result.startswith("error") else 0
    finally:
        ws.close()


def cmd_finish(a):
    a.name, a.args, a.routine = "finish_routine", json.dumps({"addr": a.addr, "summary": a.summary or ""}), a.addr
    return cmd_tool(a)


def cmd_listing(a):
    ws = Workspace(a.work)
    try:
        r = ws.server.ok(cmd="listing", verify=True, max_chars=10_000_000)
        if a.out:
            with open(a.out, "w", encoding="utf-8") as f:
                f.write(r["text"])
            print(f"written to {a.out}")
        else:
            print(r["text"])
        print("listing " + ("VERIFIED (assembles again to the identical bytes)" if r["verified"] else f'NOT verified: {r.get("verify_detail", "?")}'))
        return 0 if r["verified"] else 1
    finally:
        ws.close()


def cmd_report(a):
    ws = Workspace(a.work)
    try:
        rows = routine_states(ws)
        total = 0
        for r, kind in rows:
            gaps = ws.worker.routine_gaps(r)
            if kind != "finished" or gaps:
                total += 1
                print(f'{r["name"]} ({r["addr"]}): {kind}')
                for g in gaps:
                    print(f"    - {g}")
        print(f"{total} of {len(rows)} routines are not finished or still have gaps.")
    finally:
        ws.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", required=True, help="work directory (config, project file and state live there)")
    sub = p.add_subparsers(dest="command", required=True)
    i = sub.add_parser("init", help="prepare a work directory; the remaining options are those of z80_annotate.py")
    i.add_argument("--fresh", action="store_true", help="forget an existing project file")
    sub.add_parser("status", help="routines in processing order and what to do next")
    s = sub.add_parser("show", help="the task text for one routine")
    s.add_argument("addr")
    s.add_argument("--system", action="store_true", help="print the instructions the model gets as well")
    t = sub.add_parser("tool", help="call one tool of the orchestrator")
    t.add_argument("name")
    t.add_argument("args", nargs="?", default="{}", help="the arguments as one JSON object")
    t.add_argument("--routine", help="the routine this call belongs to (needed by finish_routine)")
    f = sub.add_parser("finish", help="finish_routine for a routine: accepted only if nothing is missing (see --accept-gaps)")
    f.add_argument("addr")
    f.add_argument("--summary")
    f.add_argument("--accept-gaps", action="store_true", help="the orchestrator's rule: the first call may send you back, the second is final")
    l = sub.add_parser("listing", help="the verified listing")
    l.add_argument("--out")
    sub.add_parser("report", help="what is not finished or has gaps")
    a, extra = p.parse_known_args(argv)
    if a.command == "init":
        a.rest = extra            # every other option is one of z80_annotate.py (--bin, --sna, --entry, --org, --rom, ...)
    elif extra:
        p.error("unrecognized arguments: " + " ".join(extra))
    try:
        return {"init": cmd_init, "status": cmd_status, "show": cmd_show, "tool": cmd_tool, "finish": cmd_finish, "listing": cmd_listing, "report": cmd_report}[a.command](a) or 0
    except DriveError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
