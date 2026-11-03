"""有界 stdio JSON-RPC MCP 客户端；不通过 shell，不自动重试操作。"""
import os
import queue
import signal
import subprocess
import threading

from .json_util import MAX_BYTES, clone, dumps, loads, require


class McpClient:
    """拥有一个 MCP 子进程会话。调用须串行；用 with 或 close() 释放资源。

    支持 2025-11-25、2025-06-18、2024-11-05 协商、分页工具发现和调用。
    超时/协议错误关闭本会话，不重发；超时后的宿主操作结果可能未知。
    """

    VERSIONS = ("2025-11-25", "2025-06-18", "2024-11-05")

    def __init__(self, argv, timeout_ms=15000):
        """以独立 argv 启动进程并完成握手；失败清理进程，超时范围 1–60000 ms。"""
        require(isinstance(argv, (list, tuple)) and argv and all(isinstance(a, str) and a and "\0" not in a for a in argv), "Invalid MCP command")
        require(type(timeout_ms) is int and 1 <= timeout_ms <= 60000, "Invalid MCP timeout")
        self.timeout = timeout_ms / 1000
        self._sequence = 0
        self._closed = False
        self._lock = threading.Lock()
        options = {"creationflags": subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        self._process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, bufsize=0, **options)
        try:
            result = self._rpc("initialize", {"protocolVersion": self.VERSIONS[0], "capabilities": {},
                                              "clientInfo": {"name": "EmbedAiChatPython", "version": "0.1.0"}})
            require(result.get("protocolVersion") in self.VERSIONS, "Unsupported negotiated MCP version")
            self._write({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        """返回当前已握手客户端，供 with 管理。"""
        return self

    def __exit__(self, *_):
        """离开 with 时幂等关闭本客户端启动的进程树。"""
        self.close()

    def close(self):
        """关闭本客户端的进程树、管道并回收进程；不关闭其他宿主进程。"""
        if self._closed:
            return
        self._closed = True
        process = self._process
        # 超时读取/写入任务在进程树终止后解除阻塞，避免遗留活动会话。
        try:
            if os.name == "nt":
                if process.poll() is None:
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, timeout=3, creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                os.killpg(process.pid, signal.SIGKILL)
        except (OSError, subprocess.TimeoutExpired):
            pass
        if process.poll() is None:
            process.kill()
        process.wait(timeout=3)
        process.stdin.close()
        process.stdout.close()

    def _write(self, message):
        """写一条 UTF-8 消息，总长度（含换行）不得超过 32 KiB。"""
        data = (dumps(message) + "\n").encode("utf-8")
        require(len(data) <= 32768, "MCP request exceeds 32 KiB")
        view = memoryview(data)
        while view:
            n = self._process.stdin.write(view)
            require(n is not None and n > 0, "MCP write failed")
            view = view[n:]

    def _read(self):
        """按块累积单行，未收到换行也检查 4 MiB 上限，EOF 视为协议错误。"""
        # FileIO.readline 无缓冲且逐字节读取；单独使用 BufferedReader 保持有界高效。
        if not hasattr(self, "_reader"):
            import io
            self._reader = io.BufferedReader(self._process.stdout)
        data = self._reader.readline(MAX_BYTES + 1)
        require(data.endswith(b"\n") and len(data) <= MAX_BYTES, "MCP EOF or line exceeds 4 MiB")
        reply = loads(data)
        require(isinstance(reply, dict) and reply.get("jsonrpc") == "2.0", "Invalid MCP JSON-RPC")
        return reply

    def _rpc(self, method, params):
        """同一期限内完成写入/读取，处理通知及不支持的服务端请求。"""
        with self._lock:
            require(not self._closed, "MCP connection closed")
            self._sequence += 1
            request_id = self._sequence
            completed = queue.Queue(maxsize=1)

            def exchange():
                try:
                    self._write({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
                    while True:
                        reply = self._read()
                        if "method" in reply:
                            if "id" in reply:
                                self._write({"jsonrpc": "2.0", "id": reply["id"], "error": {
                                    "code": -32601, "message": "Client feature unsupported"}})
                            continue
                        require(type(reply.get("id")) is int and reply["id"] == request_id, "Mismatched MCP response ID")
                        require("error" not in reply, "MCP RPC error: " + dumps(reply.get("error")))
                        require(isinstance(reply.get("result"), dict), "Invalid MCP result")
                        completed.put((reply["result"], None))
                        return
                except Exception as exc:
                    completed.put((None, exc))

            # worker 包含阻塞写入，因此服务端不读取 stdin 时同样受总超时限制。
            worker = threading.Thread(target=exchange, daemon=True, name="embedai-mcp-rpc")
            worker.start()
            try:
                result, error = completed.get(timeout=self.timeout)
                if error:
                    raise error
                return result
            except queue.Empty:
                self.close()
                raise TimeoutError("MCP timed out; outcome unknown; inspect host before another edit") from None
            except BaseException:
                self.close()
                raise
            finally:
                if self._closed:
                    worker.join(timeout=0.5)

    def tools(self):
        """分页发现全部工具；拒绝重复名称、循环游标和超过 100 页的响应。"""
        result, names, cursors, params = [], set(), set(), {}
        for _ in range(100):
            reply = self._rpc("tools/list", params)
            require(isinstance(reply.get("tools"), list), "Invalid MCP tools")
            for tool in reply["tools"]:
                require(isinstance(tool, dict) and isinstance(tool.get("name"), str) and tool["name"] and tool["name"] not in names, "Duplicate/invalid MCP tool")
                names.add(tool["name"])
                result.append(clone(tool))
            if "nextCursor" not in reply:
                return result
            cursor = reply["nextCursor"]
            require(isinstance(cursor, str) and cursor not in cursors, "Repeated/invalid MCP cursor")
            cursors.add(cursor)
            params = {"cursor": cursor}
        raise ValueError("Too many MCP pages")

    def call(self, name, arguments):
        """调用原始工具名，保留 content/structuredContent/isError；失败不重试。"""
        require(isinstance(name, str) and name and isinstance(arguments, dict), "Invalid MCP call")
        return self._rpc("tools/call", {"name": name, "arguments": clone(arguments)})
