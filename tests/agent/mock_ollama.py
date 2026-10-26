"""A scripted stand-in for Ollama's /api/chat endpoint, for testing the orchestrator without a model.

It behaves like a model that analyses one routine after the other according to a plan:
`plans[address]` is a list of turns; every turn is a list of (tool_name, arguments) calls.
The mock records what it was sent, so tests can check that images and tool errors arrived.
"""
import json
import re
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


class MockOllama:
    def __init__(self, plans, fail_with=None, reject_think=False, truncate_first=0, plans_by_model=None, capabilities_by_model=None):
        self.plans = plans          # {"0x4025": [[("run_routine", {...})], [...]]}
        self.fail_with = fail_with  # HTTP status to answer with instead
        self.reject_think = reject_think        # answer 400 "does not support thinking" when the request has a think parameter
        self.truncate_first = truncate_first    # the first N answers are cut off (done_reason "length", empty content)
        self.plans_by_model = plans_by_model or {}            # {model name: plans}: different "models" behave differently
        self.capabilities_by_model = capabilities_by_model or {}  # {model name: ["vision", "tools", ...]}
        self.requests_by_model = {}
        self.images_by_model = {}
        self.think_values = []
        self.options_seen = []
        self.hints = []
        self.requests = []
        self.images_seen = 0
        self.tool_errors = []
        self.tool_results = []
        self.system_prompts = set()
        self._server = None

    # -- the "model" --
    def respond(self, body):
        messages = body["messages"]
        self.requests.append(body)
        self.think_values.append(body.get("think", "absent"))
        self.options_seen.append(body.get("options", {}))
        self.hints += [x["content"] for x in messages if x["role"] == "user" and "cut off" in x["content"]]
        self.system_prompts.add(messages[0]["content"])
        first_user = messages[1]["content"]
        m = re.search(r"ROUTINE_ADDR=(0x[0-9A-Fa-f]+)", first_user)
        addr = m.group(1).lower() if m else "?"
        turn = sum(1 for x in messages if x["role"] == "assistant")
        images = [x for x in messages if x.get("images")]
        self.images_seen = max(self.images_seen, len(images))
        for x in messages:
            if x["role"] == "tool":
                self.tool_results.append(x["content"])
                if x["content"].startswith("error"):
                    self.tool_errors.append(x["content"])
        model = body.get("model")
        self.requests_by_model.setdefault(model, []).append(first_user)
        self.images_by_model[model] = max(self.images_by_model.get(model, 0), len(images))
        plan = self.plans_by_model.get(model, self.plans).get(addr, [])
        if turn >= len(plan):
            return {"role": "assistant", "content": "I am done."}
        entry = plan[turn]
        if isinstance(entry, dict):  # {"as_text": [...]} / {"json_string": [...]}: sloppy ways to call tools
            if "as_text" in entry:  # template mismatch: the call is written into the message instead of tool_calls
                text = "".join("<tool_call>\n" + json.dumps({"name": n, "arguments": a}) + "\n</tool_call>" for n, a in entry["as_text"])
                return {"role": "assistant", "content": "Let me do that.\n" + text}
            calls = [{"function": {"name": n, "arguments": json.dumps(a)}} for n, a in entry["json_string"]]  # arguments as a string
            return {"role": "assistant", "content": "", "tool_calls": calls}
        calls = [{"function": {"name": n, "arguments": a}} for n, a in entry]
        return {"role": "assistant", "content": "", "tool_calls": calls}

    # -- HTTP plumbing --
    def start(self):
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length))
                if self.path.endswith("/api/show"):  # what can this model do?
                    caps = outer.capabilities_by_model.get(body.get("model"), ["completion", "vision", "tools"])
                    data = json.dumps({"capabilities": caps}).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                if outer.reject_think and "think" in body:
                    outer.think_values.append("rejected")
                    self.send_response(400)
                    self.end_headers()
                    self.wfile.write(b'{"error":"\\"mock\\" does not support thinking"}')
                    return
                if outer.fail_with:
                    self.send_response(outer.fail_with)
                    self.end_headers()
                    self.wfile.write(b"model not found")
                    return
                reply = {"model": body["model"], "message": outer.respond(body), "done": True}
                if outer.truncate_first > 0:
                    outer.truncate_first -= 1
                    reply = {"model": body["model"], "done": True, "done_reason": "length",
                             "message": {"role": "assistant", "content": "", "thinking": "Okay, let's tackle this ... " * 50}}
                data = json.dumps(reply).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):  # silence
                pass

        self._server = HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        return f"http://127.0.0.1:{self._server.server_address[1]}"

    def stop(self):
        if self._server:
            self._server.shutdown()
            self._server.server_close()
