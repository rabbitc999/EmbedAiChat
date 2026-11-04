"""Python 特有边界测试；共享行为另由 tests/verify.py 验收。"""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from embedai import Engine, McpClient, Tool
from embedai.console import configure
from embedai.json_util import dumps, loads, validate

ROOT = Path(__file__).resolve().parents[2]


class JsonAndHostTests(unittest.TestCase):
    """验证 Python 的 bool/int 类型边界、schema 绕过和可变对象所有权。"""

    def test_strict_json(self):
        for text in ('{"a":1,"a":2}', 'NaN', '1e999', '"\\ud800"', '{} trailing'):
            with self.subTest(text=text), self.assertRaises((ValueError, UnicodeError)):
                loads(text)

    def test_bool_is_not_number_or_integer_enum(self):
        for schema in ({"type": "integer"}, {"type": "number"}, {"enum": [1]}):
            with self.subTest(schema=schema), self.assertRaises(ValueError):
                validate(schema, True)

    def test_unknown_constraints_in_unused_branches(self):
        for schema in ({"anyOf": [{"type": "string"}, {"pattern": "danger"}]},
                       {"type": "object", "properties": {"unused": {"$ref": "external"}}}):
            with self.subTest(schema=schema), self.assertRaises(ValueError):
                validate(schema, "ok" if "anyOf" in schema else {})

    def test_callback_results_and_schema_are_copied(self):
        state = {"value": 1}
        schema = {"type": "object", "additionalProperties": False}
        with Engine() as engine:
            engine.register_tool(Tool("read", "read actual state", schema, lambda _: state))
            engine.allow_tools = True
            schema["type"] = "string"
            result = engine.invoke("read", {})
            state["value"] = 2
            self.assertEqual(result, {"value": 1})
            result["value"] = 99
            self.assertEqual(engine.invoke("read", {}), {"value": 2})

    def test_send_reentry_rejected_before_second_request(self):
        with Engine() as engine:
            engine.config.model = "fixture"

            def transport(*_):
                with self.assertRaisesRegex(ValueError, "already active"):
                    engine.send("reentry")
                return {"choices": [{"message": {"content": "done"}}]}

            engine._http = transport
            self.assertEqual(engine.send("first"), "done")
            self.assertEqual(len(engine.export_history()["messages"]), 2)

    def test_message_events_are_ordered_and_independent(self):
        calls = [{"id": str(i), "type": "function", "function": {"name": name, "arguments": dumps(args)}}
                 for i, (name, args) in enumerate([("assistant_reply", {"text": "1.0"}), ("assistant_end", {})])]
        requests = []
        def transport(*args):
            requests.append(args)
            self.assertEqual(len(requests), 1)
            return {"choices": [{"message": {"content": "", "tool_calls": calls}}]}
        with Engine(transport) as engine:
            engine.config.model = "fixture"
            engine.allow_tools = True
            events = []
            def observe(event):
                events.append(event["type"])
                if event["type"] == "tool":
                    event["audit"]["result"]["ok"] = False
            self.assertEqual(engine.send("run", observe), "1.0")
            self.assertEqual(events, ["tool", "assistant", "tool", "done"])
            self.assertTrue(all(a["result"]["ok"] for a in engine.export_history()["audit"]))
            self.assertIsInstance(engine.export_history()["messages"][1]["content"], str)

    def test_message_callback_failure_preserves_history(self):
        def transport(*_):
            return {"choices": [{"message": {"content": "已发送"}}]}
        with Engine(transport) as engine:
            engine.config.model = "fixture"
            def stop(event):
                raise RuntimeError("UI disconnected")
            with self.assertRaisesRegex(RuntimeError, "UI disconnected"):
                engine.send("first", stop)
            self.assertEqual(engine.export_history()["messages"][-1]["content"], "已发送")
            self.assertEqual(engine.send("second"), "已发送")

    def test_credentials_not_in_repr_or_configuration(self):
        with Engine() as engine, tempfile.TemporaryDirectory() as directory:
            engine.config.api_key = "test-secret"
            self.assertNotIn("test-secret", repr(engine.config))
            self.assertNotIn("test-secret", dumps(engine.export_history()))
            config = Path(directory) / "config.json"
            config.write_text('{"api_key":"test-secret"}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not config credentials"):
                configure(engine, config)

    def test_skill_file_limit_and_frontmatter(self):
        with Engine() as engine, tempfile.TemporaryDirectory() as directory:
            skill = Path(directory) / "demo"
            skill.mkdir()
            entry = skill / "SKILL.md"
            for text in ("no frontmatter", "---\nmissing closing", "a" * 65537):
                entry.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    engine.load_skill(skill)
            entry.write_text("---\nname: demo\n---\nHost instructions", encoding="utf-8")
            self.assertEqual(engine.load_skill(skill), "demo")
            engine.activate_skill("demo")
            with self.assertRaises(ValueError):
                engine.activate_skill("../../unloaded")
            engine.activate_skill("")


class McpBoundaryTests(unittest.TestCase):
    """真实子进程验证协议失败、超时清理、请求限额及注册原子性。"""

    def server(self, source, timeout=1500):
        """启动只用于本地测试的 Python stdio 服务。"""
        return McpClient([sys.executable, "-u", "-c", source], timeout)

    def test_handshake_rejects_bad_version_and_mismatched_id(self):
        for result in ('{"protocolVersion":"unsupported"}', '{"protocolVersion":"2025-11-25"}'):
            bad_id = '999' if "unsupported" not in result else 'r["id"]'
            source = ('import json,sys\nr=json.loads(sys.stdin.readline())\n'
                      f'print(json.dumps({{"jsonrpc":"2.0","id":{bad_id},"result":{result}}}),flush=True)\n')
            with self.subTest(result=result), self.assertRaises(ValueError):
                self.server(source)

    def test_timeout_closes_process_and_prevents_retry(self):
        # 服务完成握手后停止回应，测试实际 RPC 超时及进程清理。
        source = ('import json,sys,time\nr=json.loads(sys.stdin.readline())\n'
                  'print(json.dumps({"jsonrpc":"2.0","id":r["id"],"result":{"protocolVersion":"2025-11-25"}}),flush=True)\n'
                  'time.sleep(60)\n')
        hung = self.server(source)
        try:
            hung.timeout = 0.05
            with self.assertRaises(TimeoutError):
                hung.tools()
            self.assertIsNotNone(hung._process.poll())
            with self.assertRaisesRegex(ValueError, "closed"):
                hung.tools()
        finally:
            hung.close()

    def test_request_limit_closes_session(self):
        with McpClient([sys.executable, str(ROOT / "tests/mock_mcp.py")]) as client:
            with self.assertRaisesRegex(ValueError, "32 KiB"):
                client.call("echo", {"text": "a" * 32768})
            self.assertIsNotNone(client._process.poll())

    def test_attach_is_atomic_on_name_conflict(self):
        with McpClient([sys.executable, str(ROOT / "tests/mock_mcp.py")]) as client, Engine() as engine:
            engine.register_tool(Tool("test_fail", "conflict", {"type": "object"}, lambda _: {}))
            before = engine.tools()
            with self.assertRaisesRegex(ValueError, "conflict"):
                engine.attach_mcp("test", client)
            self.assertEqual(engine.tools(), before)
            self.assertEqual(client.call("echo", {"text": "still alive"})["structuredContent"]["echo"], "still alive")


if __name__ == "__main__":
    unittest.main(verbosity=2)
