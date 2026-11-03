"""MCP 结果回传、模型继续调用和结束回答验收。

默认跨七语言使用本地 HTTP。--live-deepseek 运行 Python 真实 API 两场景，
--live-languages 运行全部或指定语言的真实 API 两场景，
密钥从环境 DEEPSEEK_API_KEY 或 stdin 读取，不写文件、不打印认证信息。
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from verify_tokens import COMMANDS, ROOT


def tool_results(body):
    values = []
    for message in body["messages"]:
        if message.get("role") == "tool":
            values.append((message["tool_call_id"], json.loads(message["content"])))
        elif isinstance(message.get("content"), list):
            for block in message["content"]:
                if block.get("type") == "tool_result":
                    values.append((block["tool_use_id"], json.loads(block["content"])))
    return values


class Handler(BaseHTTPRequestHandler):
    errors = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            content_types = self.headers.get_all("Content-Type") or []
            assert len(content_types) == 1 and content_types[0].split(";")[0].strip().lower() == "application/json", "JSON Content-Type must be sent exactly once"
            results = tool_results(body)
            anthropic = self.path.endswith("/messages")
            scenario = body["model"]
            if not results:
                name = "flow_fail" if scenario == "failure" else "flow_issue_ticket"
                args = {}
            elif scenario == "chain" and len(results) == 1:
                assert results[0][0] == "call-0" and results[0][1]["ok"]
                name = "flow_confirm_ticket"
                args = {"ticket": results[0][1]["result"]["structuredContent"]["ticket"]}
            else:
                assert results[-1][0] == f"call-{len(results)-1}"
                if scenario == "failure":
                    assert not results[-1][1]["ok"]
                    assert results[-1][1]["result"]["isError"]
                    text = "FAILED_AS_EXPECTED"
                else:
                    assert results[-1][1]["ok"]
                    text = results[-1][1]["result"]["structuredContent"]["receipt" if scenario == "chain" else "ticket"]
                response = {"content": [{"type": "text", "text": text}]} if anthropic else {"choices": [{"message": {"role": "assistant", "content": text}}]}
                self.respond(response)
                return
            ident = f"call-{len(results)}"
            response = ({"content": [{"type": "tool_use", "id": ident, "name": name, "input": args}]} if anthropic else
                        {"choices": [{"message": {"role": "assistant", "content": None, "tool_calls": [{"id": ident, "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}]}}]})
            self.respond(response)
        except Exception as exc:
            self.errors.append(str(exc))
            self.send_error(500)

    def respond(self, body):
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class FeedbackTests(unittest.TestCase):
    def test_c_mcp_pagination_manual_calls_and_duplicate_connection(self):
        argv = json.dumps([sys.executable, str(ROOT / "tests/mock_mcp.py")])
        commands = (f"/mcp t {argv}\n/mcp t {argv}\n/allow on\n"
                    '/call t_echo {"text":"分页工具回读"}\n/call t_fail {}\n/tools\n/exit\n')
        result = subprocess.run(COMMANDS["c"], input=commands, text=True, encoding="utf-8", capture_output=True, timeout=10, cwd=ROOT)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr.count("MCP connection/discovery failed"), 1)
        self.assertIn("分页工具回读", result.stdout)
        self.assertIn('"isError":true', result.stdout)
        definitions = json.loads(next(line for line in result.stdout.splitlines() if line.startswith("[")))
        self.assertEqual({tool["name"] for tool in definitions if tool["name"].startswith("t_")}, {"t_echo", "t_fail"})

    def test_c_mcp_discovery_is_atomic_and_timeout_is_bounded(self):
        for flag in ("--invalid-schema", "--cycle"):
            with self.subTest(flag=flag):
                argv = json.dumps([sys.executable, str(ROOT / "tests/mock_mcp_workflow.py"), flag])
                commands = f"/mcp flow {argv}\n/tools\n/exit\n"
                result = subprocess.run(COMMANDS["c"], input=commands, text=True, encoding="utf-8", capture_output=True, timeout=10, cwd=ROOT)
                self.assertIn("MCP connection/discovery failed", result.stderr)
                definitions = json.loads(next(line for line in result.stdout.splitlines() if line.startswith("[")))
                self.assertFalse(any(tool["name"].startswith("flow_") for tool in definitions))
                self.assertIn("host_set_value", [tool["name"] for tool in definitions])
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "config.json"
            config.write_text(json.dumps({"model": "fixture", "mcp": [{"prefix": "flow", "command": [sys.executable, str(ROOT / "tests/mock_mcp_workflow.py"), "--hang"], "timeout_ms": 100}]}), encoding="utf-8")
            start = time.monotonic()
            result = subprocess.run(COMMANDS["c"] + ["--config", str(config)], text=True, encoding="utf-8", capture_output=True, timeout=5, cwd=ROOT)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("MCP connection/discovery failed", result.stderr)
            self.assertLess(time.monotonic() - start, 3)

    def test_results_drive_next_model_decision(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for language, command in COMMANDS.items():
                for provider in ("openai", "anthropic"):
                    for scenario in ("single", "chain", "failure"):
                        with self.subTest(language=language, provider=provider, scenario=scenario), tempfile.TemporaryDirectory() as directory:
                            config = Path(directory) / "config.json"
                            config.write_text(json.dumps({"provider": provider, "base_url": f"http://127.0.0.1:{server.server_port}/v1", "model": scenario, "allow_tools": True, "mcp": [{"prefix": "flow", "command": [sys.executable, str(ROOT / "tests/mock_mcp_workflow.py")]}]}), encoding="utf-8")
                            result = subprocess.run(command + ["--config", str(config), "--ask", "execute workflow"], text=True, encoding="utf-8", capture_output=True, timeout=35, cwd=ROOT)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertFalse(result.stderr, result.stderr)
                            history = json.loads(result.stdout.strip().splitlines()[-1])["history"]
                            audit = history["audit"]
                            self.assertEqual(len(audit), 2 if scenario == "chain" else 1)
                            if scenario == "failure":
                                self.assertEqual(history["messages"][-1]["content"], "FAILED_AS_EXPECTED")
                            else:
                                data = audit[-1]["result"]["result"]["structuredContent"]
                                self.assertEqual(history["messages"][-1]["content"], data["receipt" if scenario == "chain" else "ticket"])
                            if scenario == "chain":
                                self.assertEqual(json.loads(audit[1]["arguments"])["ticket"], audit[0]["result"]["result"]["structuredContent"]["ticket"])
            self.assertFalse(Handler.errors, Handler.errors)
        finally:
            server.shutdown()
            server.server_close()


def live_deepseek():
    sys.path.insert(0, str(ROOT / "python"))
    from embedai import Engine, McpClient, post_json

    key = os.environ.get("DEEPSEEK_API_KEY") or sys.stdin.readline().strip()
    if not key:
        raise ValueError("Missing API key in DEEPSEEK_API_KEY or stdin")
    for scenario in ("single", "chain"):
        requests = []

        def transport(url, headers, body):
            # 只保留请求正文用于断言；不保存或输出 headers / key。
            requests.append(body)
            return post_json(url, headers, body)

        with Engine(transport) as engine, McpClient([sys.executable, str(ROOT / "tests/mock_mcp_workflow.py")]) as mcp:
            engine.config.base_url = "https://api.deepseek.com/v1"
            engine.config.model = "deepseek-chat"
            engine.config.api_key = key
            engine.config.max_tokens = 512
            engine.allow_tools = True
            engine.attach_mcp("flow", mcp)
            prompt = ("调用 flow_issue_ticket 一次，然后直接输出工具实际返回的 ticket，不要继续调用其他工具。" if scenario == "single" else
                      "先调用 flow_issue_ticket 获取 ticket。收到真实结果后，再将其中 ticket 原样传给 flow_confirm_ticket。收到确认结果后，直接输出实际返回的 receipt。不要编造 ticket 或 receipt，不要调用其他工具。")
            answer = engine.send(prompt)
            audit = engine.export_history()["audit"]
            expected = ["flow_issue_ticket"] + (["flow_confirm_ticket"] if scenario == "chain" else [])
            assert [item["name"] for item in audit] == expected, "Unexpected tool sequence"
            assert all(item["result"]["ok"] for item in audit), "MCP tool failed"
            field = "receipt" if scenario == "chain" else "ticket"
            actual = audit[-1]["result"]["result"]["structuredContent"][field]
            assert actual in answer, "Final answer did not include actual MCP result"
            assert len(requests) == len(expected) + 1, "Unexpected model request count"
            assert len(tool_results(requests[-1])) == len(expected), "Missing feedback to model"
            if scenario == "chain":
                assert json.loads(audit[1]["arguments"])["ticket"] == audit[0]["result"]["result"]["structuredContent"]["ticket"]
            assert key not in json.dumps(engine.export_history()), "Credential leaked to archive"
            print(json.dumps({"scenario": scenario, "model": engine.config.model, "model_requests": len(requests), "mcp_calls": expected, "actual_result_verified": True, "feedback_verified": True, "answer": answer}, ensure_ascii=False), flush=True)


def live_all_languages(languages=None):
    key = os.environ.get("DEEPSEEK_API_KEY") or sys.stdin.readline().strip()
    if not key:
        raise ValueError("Missing API key")
    for language, command in COMMANDS.items():
        if languages and language not in languages:
            continue
        for scenario in ("single", "chain"):
            with tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                config, saved = directory / "config.json", directory / "history.json"
                config.write_text(json.dumps({"provider": "openai", "base_url": "https://api.deepseek.com/v1", "model": "deepseek-chat", "max_tokens": 512, "max_rounds": 0, "allow_tools": True, "mcp": [{"prefix": "flow", "command": [sys.executable, str(ROOT / "tests/mock_mcp_workflow.py")]}]}), encoding="utf-8")
                prompt = ("调用 flow_issue_ticket 一次，然后直接输出工具实际返回的 ticket，不要继续调用其他工具。" if scenario == "single" else
                          "先调用 flow_issue_ticket 获取 ticket。收到真实结果后，再将其中 ticket 原样传给 flow_confirm_ticket。收到确认结果后，直接输出实际返回的 receipt。不要编造 ticket 或 receipt，不要调用其他工具。")
                result = subprocess.run(command + ["--config", str(config)], input=f"/key\n{key}\n{prompt}\n/save {saved}\n/exit\n", capture_output=True, text=True, encoding="utf-8", timeout=180, cwd=ROOT)
                if result.returncode or result.stderr or not saved.exists():
                    print(json.dumps({"language": language, "scenario": scenario, "exit_code": result.returncode, "error": result.stderr.replace(key, "[redacted]")[:500]}, ensure_ascii=False), flush=True)
                    raise RuntimeError(f"{language}/{scenario}: console or API failure (details suppressed)")
                history = json.loads(saved.read_text(encoding="utf-8"))
                assert key not in result.stdout + result.stderr + json.dumps(history), "Credential appeared in output"
                expected = ["flow_issue_ticket"] + (["flow_confirm_ticket"] if scenario == "chain" else [])
                audit = history["audit"]
                assert [item["name"] for item in audit] == expected, f"{language}: unexpected MCP sequence"
                assert all(item["result"]["ok"] for item in audit), f"{language}: MCP failure"
                actual = audit[-1]["result"]["result"]["structuredContent"]["receipt" if scenario == "chain" else "ticket"]
                assert actual in history["messages"][-1]["content"], f"{language}: final result mismatch"
                if scenario == "chain":
                    assert json.loads(audit[1]["arguments"])["ticket"] == audit[0]["result"]["result"]["structuredContent"]["ticket"], f"{language}: feedback mismatch"
                print(json.dumps({"language": language, "scenario": scenario, "model": "deepseek-chat", "mcp_calls": expected, "actual_result_verified": True, "feedback_verified": True}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--live-deepseek", action="store_true")
    parser.add_argument("--live-languages", action="store_true")
    parser.add_argument("--languages", nargs="+", choices=list(COMMANDS))
    args = parser.parse_args()
    if args.live_deepseek or args.live_languages:
        try:
            if args.live_languages:
                live_all_languages(args.languages)
            else:
                live_deepseek()
        except Exception as exc:
            # 不打印异常正文，避免第三方错误意外包含凭据。
            print(f"Live test failed: {type(exc).__name__}", file=sys.stderr)
            sys.exit(1)
    else:
        unittest.main(argv=[__file__], verbosity=2)
