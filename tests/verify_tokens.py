"""七种语言的 token 配置与摘要压缩验收；只访问本地 HTTP fixture。"""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = {
    "c": [str(ROOT / "bin/c/EmbedAiChatC.exe")],
    "cpp": [str(ROOT / "bin/cpp/EmbedAiChatCpp.exe")],
    "java": ["java", "-Dfile.encoding=UTF-8", "-cp", str(ROOT / "java/EmbedAiChatJava.jar") + ";" + str(ROOT / "java/lib/gson-2.11.0.jar"), "io.embedai.Console"],
    "go": [str(ROOT / "bin/go/EmbedAiChatGo.exe")],
    "rust": [str(ROOT / "bin/rust/EmbedAiChatRust.exe")],
    "csharp": ["dotnet", str(ROOT / "bin/csharp/EmbedAiChatCSharp.dll")],
    "python": [sys.executable, "-X", "utf8", str(ROOT / "python/console.py")],
}


class Handler(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.requests.append(body)
        if body["model"] in ("tool-loop", "tool-batch"):
            anthropic = self.path.endswith("/messages")
            completed = sum(1 for m in body["messages"] if m.get("role") == "tool")
            completed += sum(1 for m in body["messages"] if isinstance(m.get("content"), list)
                             for block in m["content"] if block.get("type") == "tool_result")
            total = 12 if body["model"] == "tool-loop" else 17
            count = 1 if body["model"] == "tool-loop" else 17
            if completed < total:
                calls = [(f"call-{completed + i}", completed + i + 1) for i in range(count)]
                response = ({"content": [{"type": "tool_use", "id": ident, "name": "host_set_value", "input": {"value": value}} for ident, value in calls]} if anthropic else
                            {"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": [{"id": ident, "type": "function", "function": {"name": "host_set_value", "arguments": json.dumps({"value": value})}} for ident, value in calls]}}]})
            else:
                response = ({"content": [{"type": "text", "text": "done"}]} if anthropic else
                            {"choices": [{"message": {"role": "assistant", "content": "done"}}]})
            data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        summary = "Summarize this historical" in json.dumps(body["messages"])
        if summary and body["model"] == "summary-failure":
            self.send_error(503)
            return
        text = "compressed context" if summary else "done"
        response = ({"content": [{"type": "text", "text": text}]} if self.path.endswith("/messages") else
                    {"choices": [{"message": {"role": "assistant", "content": text}}]})
        data = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class TokenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def run_case(self, language, provider="openai", **settings):
        Handler.requests.clear()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            history, saved, config = [directory / name for name in ("history.json", "saved.json", "config.json")]
            repeats = settings.pop("history_repeats", 60)
            send_count = settings.pop("send_count", 1)
            archive = {"version": 1, "messages": [{"role": "user" if i % 2 == 0 else "assistant", "content": "history " * repeats} for i in range(12)], "summary": "", "covered": 0, "audit": []}
            history.write_text(json.dumps(archive), encoding="utf-8")
            cfg = {"provider": provider, "base_url": f"http://127.0.0.1:{self.server.server_port}/v1", "model": "fixture", "context_budget": 4000, "max_tokens": 317}
            cfg.update(settings)
            config.write_text(json.dumps(cfg), encoding="utf-8")
            result = subprocess.run(COMMANDS[language] + ["--config", str(config)], input=f"/load {history}\n" + "hello\n" * send_count + f"/save {saved}\n/exit\n", text=True, encoding="utf-8", capture_output=True, timeout=30, cwd=ROOT)
            self.assertTrue(saved.exists(), result.stdout + result.stderr)
            return result, archive, json.loads(saved.read_text(encoding="utf-8")), list(Handler.requests)

    def test_threshold_and_output_limit(self):
        for language in COMMANDS:
            for provider in ("openai", "anthropic"):
                # This history uses ~55%: default 90 leaves it intact, 40 compresses.
                for settings, compressed in (({}, False), ({"history_repeats": 115}, True), ({"compression_threshold": 40}, True), ({"compression_threshold": 40, "auto_compact": False}, False)):
                    with self.subTest(language=language, provider=provider, settings=settings):
                        result, original, saved, requests = self.run_case(language, provider, **settings)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertFalse(result.stderr, result.stderr)
                        self.assertEqual(saved["messages"][:12], original["messages"])
                        self.assertEqual(len(saved["messages"]), 14)
                        self.assertEqual(saved["covered"], 9 if compressed else 0)
                        self.assertEqual(len(requests), 2 if compressed else 1)
                        self.assertTrue(all(body["max_tokens"] == 317 for body in requests))
                        if compressed:
                            self.assertEqual(saved["summary"], "compressed context")
                            self.assertNotIn("tools", requests[0])

    def test_summary_failure_keeps_original_history(self):
        for language in COMMANDS:
            with self.subTest(language=language):
                result, original, saved, requests = self.run_case(language, compression_threshold=40, model="summary-failure")
                self.assertTrue(result.stderr)
                self.assertEqual(saved["messages"][:12], original["messages"])
                self.assertEqual(saved["covered"], 0)
                self.assertEqual(saved["summary"], "")
                self.assertEqual(len(requests), 1)

    def test_rolling_summary_has_separate_budget(self):
        for language in COMMANDS:
            for provider in ("openai", "anthropic"):
                with self.subTest(language=language, provider=provider):
                    result, original, saved, requests = self.run_case(language, provider, max_tokens=1500, max_summary_tokens=600, compression_threshold=10, send_count=2)
                    self.assertFalse(result.stderr, result.stderr)
                    self.assertEqual([body["max_tokens"] for body in requests], [600, 1500, 600, 1500])
                    self.assertIn("compressed context", json.dumps(requests[2]["messages"]))
                    self.assertIn("Prioritize current goals", json.dumps(requests[2]["messages"]))
                    self.assertEqual(saved["messages"][:12], original["messages"])
                    self.assertEqual(len(saved["messages"]), 16)
                    self.assertEqual(saved["covered"], 11)

    def test_invalid_limits_send_no_http_request(self):
        for language in COMMANDS:
            for setting in ({"max_summary_tokens": 0}, {"max_summary_tokens": 1000001}, {"max_tokens": 0}, {"max_tokens": 1000001}, {"compression_threshold": 0}, {"compression_threshold": 101}):
                with self.subTest(language=language, setting=setting), tempfile.TemporaryDirectory() as directory:
                    Handler.requests.clear()
                    config = Path(directory) / "config.json"
                    config.write_text(json.dumps({"model": "fixture", "base_url": f"http://127.0.0.1:{self.server.server_port}/v1", **setting}), encoding="utf-8")
                    result = subprocess.run(COMMANDS[language] + ["--config", str(config), "--ask", "hello"], capture_output=True, text=True, encoding="utf-8", timeout=30, cwd=ROOT)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertEqual(Handler.requests, [])

    def test_tools_continue_without_default_count_limits(self):
        for language in COMMANDS:
            for provider in ("openai", "anthropic"):
                for model, expected in (("tool-loop", 12), ("tool-batch", 17)):
                    with self.subTest(language=language, provider=provider, model=model):
                        result, _, saved, requests = self.run_case(language, provider, model=model, allow_tools=True, context_budget=16000)
                        self.assertFalse(result.stderr, result.stderr)
                        self.assertEqual(len(saved["audit"]), expected)
                        self.assertTrue(all(item["result"]["ok"] for item in saved["audit"]))
                        self.assertEqual(saved["messages"][-1]["content"], "done")
                        self.assertEqual(len(requests), 13 if model == "tool-loop" else 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
