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
    def __init__(self, plans, fail_with=None):
        self.plans = plans          # {"0x4025": [[("run_routine", {...})], [...]]}
        self.fail_with = fail_with  # HTTP status to answer with instead
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
        plan = self.plans.get(addr, [])
        if turn >= len(plan):
            return {"role": "assistant", "content": "I am done."}
        calls = [{"function": {"name": n, "arguments": a}} for n, a in plan[turn]]
        return {"role": "assistant", "content": "", "tool_calls": calls}

    # -- HTTP plumbing --
    def start(self):
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length))
                if outer.fail_with:
                    self.send_response(outer.fail_with)
                    self.end_headers()
                    self.wfile.write(b"model not found")
                    return
                data = json.dumps({"model": body["model"], "message": outer.respond(body), "done": True}).encode()
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
