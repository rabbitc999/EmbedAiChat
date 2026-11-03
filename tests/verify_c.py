"""C console + real libcurl adapter acceptance against a local HTTP fixture."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = ROOT / "bin/c/EmbedAiChatC.exe"


class Model(BaseHTTPRequestHandler):
    errors = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            anthropic = self.path.endswith("/messages")
            messages = body["messages"]
            completed = any(m["role"] == "tool" or
                            isinstance(m.get("content"), list) and
                            any(b.get("type") == "tool_result" for b in m["content"])
                            for m in messages)
            if body["model"] == "fail-after-edit" and completed:
                self.send_error(503)
                return
            assert (self.headers.get("x-api-key") if anthropic else
                    self.headers.get("Authorization")) in ("session-secret", "Bearer session-secret")
            if not completed:
                tools = body["tools"]
                names = {t["name"] if anthropic else t["function"]["name"] for t in tools}
                assert {"host_set_value", "host_set_title", "skill_list", "skill_activate"} <= names
                if anthropic:
                    response = {"content": [{"type": "tool_use", "id": "edit-1",
                                             "name": "host_set_value", "input": {"value": 42}}]}
                else:
                    response = {"choices": [{"message": {"role": "assistant", "content": None,
                                "tool_calls": [{"id": "edit-1", "type": "function", "function": {
                                    "name": "host_set_value", "arguments": '{"value":42}'}}]}}]}
            else:
                if anthropic:
                    block = messages[-1]["content"][0]
                    assert block["tool_use_id"] == "edit-1"
                    result = json.loads(block["content"])
                else:
                    assert messages[-1]["tool_call_id"] == "edit-1"
                    result = json.loads(messages[-1]["content"])
                assert result == {"ok": True, "result": {"value": 42}}, result
                response = ({"content": [{"type": "text", "text": "**验证成功**"}]} if anthropic else
                            {"choices": [{"message": {"role": "assistant", "content": "**验证成功**"}}]})
            data = json.dumps(response, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as exc:
            self.errors.append(repr(exc))
            self.send_error(500)


class Acceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Model)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def run_console(self, provider, commands, directory, model="fixture"):
        config = Path(directory) / "config.json"
        config.write_text(json.dumps({"provider": provider, "model": model, "allow_tools": True,
                                     "base_url": f"http://127.0.0.1:{self.server.server_port}/v1"}), encoding="utf-8")
        p = subprocess.run([str(EXECUTABLE), "--config", str(config)], input="\n".join(commands) + "\n",
                           capture_output=True, encoding="utf-8", cwd=ROOT, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn("session-secret", p.stdout + p.stderr)
        return p

    def test_both_protocols_and_archive(self):
        for provider in ("openai", "anthropic"):
            with self.subTest(provider=provider), tempfile.TemporaryDirectory() as directory:
                archive = Path(directory) / "history.json"
                p = self.run_console(provider, ["/key", "session-secret", "你好", "/host", f"/save {archive}",
                                                "/clear", f"/load {archive}", "/audit", "/exit"], directory)
                self.assertFalse(p.stderr, p.stderr)
                self.assertIn("**验证成功**", p.stdout)
                a = json.loads(archive.read_text(encoding="utf-8"))
                self.assertEqual(a["version"], 1)
                self.assertEqual(a["messages"][-1]["content"], "**验证成功**")
                self.assertEqual(a["audit"][0]["result"], {"ok": True, "result": {"value": 42}})
                self.assertNotIn("session-secret", archive.read_text(encoding="utf-8"))
        self.assertFalse(Model.errors, Model.errors)

    def test_final_http_failure_retains_audit_without_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "failed.json"
            p = self.run_console("openai", ["/key", "session-secret", "change", "/host", f"/save {archive}", "/exit"],
                                 directory, "fail-after-edit")
            self.assertIn("HTTP transport/status failure", p.stderr)
            a = json.loads(archive.read_text(encoding="utf-8"))
            self.assertEqual(len(a["audit"]), 1)
            self.assertEqual(a["audit"][0]["result"]["result"]["value"], 42)
            self.assertEqual(len(a["messages"]), 1)

    def test_permissions_and_atomic_import(self):
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "bad.json"
            bad.write_text('{"version":1,"messages":[],"audit":[],"summary":"","covered":99}', encoding="utf-8")
            archive = Path(directory) / "after.json"
            p = self.run_console("openai", ["/allow off", '/call host_set_value {"value":42}', "/allow on",
                                           '/call host_set_value {"value":101}', '/call host_set_value {"value":7}',
                                           f"/load {bad}", "/host", f"/save {archive}", "/exit"], directory)
            self.assertIn("Tool calls are disabled", p.stderr)
            self.assertIn("schema validation failed", p.stderr)
            self.assertIn("Invalid history archive", p.stderr)
            self.assertIn('"value":7', p.stdout)
            self.assertEqual(json.loads(archive.read_text())["messages"], [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, default=EXECUTABLE)
    args, remainder = parser.parse_known_args()
    EXECUTABLE = args.executable.resolve()
    unittest.main(argv=[__file__, *remainder])
