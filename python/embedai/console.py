"""独立控制台宿主演示；真实宿主只需导入 Engine/McpClient/Tool。"""
import argparse
import getpass
import os
from pathlib import Path
import sys
import tempfile

from .engine import Engine, Tool
from .json_util import MAX_BYTES, clone, dumps, loads, require
from .mcp import McpClient


def read_json(path):
    """读取有界 UTF-8 JSON 文件，根节点必须是对象。"""
    with Path(path).open("rb") as source:
        data = source.read(MAX_BYTES + 1)
    value = loads(data)
    require(isinstance(value, dict), "Expected JSON object")
    return value


def save_json(path, value):
    """先写同目录独立临时文件，再原子替换目标；失败清理临时文件。"""
    path = Path(path).resolve()
    data = dumps(value)
    require(len(data.encode("utf-8")) <= MAX_BYTES, "Archive exceeds 4 MiB")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, prefix=".embedai-", delete=False) as output:
            temporary = output.name
            output.write(data)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary:
            Path(temporary).unlink(missing_ok=True)


def attach(engine, prefix, argv, timeout_ms=15000):
    """创建 MCP 会话并交给引擎；发现/注册失败时立即释放新进程。"""
    client = McpClient(argv, timeout_ms)
    try:
        engine.attach_mcp(prefix, client)
    except BaseException:
        client.close()
        raise


def configure(engine, path):
    """读取不含密钥的配置；Skill 相对路径以配置文件目录为基准。"""
    path = Path(path).resolve()
    config = read_json(path)
    require(not ({"key", "api_key", "APIKey"} & config.keys()), "Use /key, not config credentials")
    for name in ("provider", "base_url", "model"):
        if name in config:
            require(isinstance(config[name], str), f"Invalid config {name}")
            setattr(engine.config, name, config[name])
    for name in ("context_budget", "max_rounds", "max_tokens", "max_summary_tokens", "compression_threshold"):
        if name in config:
            require(type(config[name]) is int, f"Invalid config {name}")
            setattr(engine.config, name, config[name])
    require(type(config.get("auto_compact", True)) is bool, "Invalid auto_compact")
    engine.config.auto_compact = config.get("auto_compact", True)
    require(type(config.get("allow_tools", False)) is bool, "Invalid allow_tools")
    engine.allow_tools = config.get("allow_tools", False)
    skills = config.get("skills", [])
    require(isinstance(skills, list), "Invalid skills configuration")
    for directory in skills:
        require(isinstance(directory, str), "Invalid Skill path")
        directory = Path(directory)
        engine.load_skill(directory if directory.is_absolute() else path.parent / directory)
    engine.activate_skill(config.get("active_skill", ""))
    servers = config.get("mcp", [])
    require(isinstance(servers, list), "Invalid MCP configuration")
    for server in servers:
        require(isinstance(server, dict) and "prefix" in server and "command" in server, "Invalid MCP configuration")
        attach(engine, server["prefix"], server["command"], server.get("timeout_ms", 15000))


def demo_host(engine):
    """注册演示宿主的真实进程内状态回调；返回值是状态副本，不伪造操作成功。"""
    state = {"title": "Demo host", "value": 0}

    def update(arguments):
        state.update(arguments)
        return clone(state)

    engine.register_tool(Tool("host_get_state", "Read demo host state", {"type": "object", "additionalProperties": False}, lambda _: clone(state)))
    for name, schema in (("title", {"type": "string", "minLength": 1, "maxLength": 120}),
                         ("value", {"type": "number", "minimum": 0, "maximum": 100})):
        engine.register_tool(Tool("host_set_" + name, "Set demo host " + name, {
            "type": "object", "properties": {name: schema}, "required": [name], "additionalProperties": False}, update))
    return state


def run_console(argv=None):
    """执行 --config/--ask 或交互模式；交互错误打印后继续，退出总会释放 MCP。"""
    parser = argparse.ArgumentParser(description="EmbedAiChat Python")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--ask")
    args = parser.parse_args(argv)
    with Engine() as engine:
        state = demo_host(engine)
        if args.config:
            configure(engine, args.config)
        if args.ask is not None:
            print(engine.send(args.ask))
            print(dumps({"host": state, "history": engine.export_history()}))
            return
        print("EmbedAiChat Python 控制台；/help 查看命令，/key 输入会话密钥。")
        while True:
            line = sys.stdin.readline(65538)
            if not line:
                return
            line = line.rstrip("\r\n")
            if line == "/exit":
                return
            try:
                require(len(line.encode("utf-8")) <= 65536, "Command exceeds 64 KiB")
                if line == "/help":
                    print("/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n"
                          "/mcp <prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n"
                          "/save <文件> | /load <文件> | /clear | /audit | /exit")
                elif line == "/key":
                    # 真实终端使用 getpass；重定向输入只读取下一行，不回显凭据。
                    engine.config.api_key = getpass.getpass("API Key (hidden): ") if sys.stdin.isatty() else sys.stdin.readline().rstrip("\r\n")
                elif line == "/tools":
                    print(dumps(engine.tools()))
                elif line == "/host":
                    print(dumps(state))
                elif line == "/audit":
                    print(dumps(engine.export_history()["audit"]))
                elif line == "/clear":
                    engine.clear()
                elif line.startswith("/allow "):
                    require(line[7:] in ("on", "off"), "Use /allow on|off")
                    engine.allow_tools = line[7:] == "on"
                elif line.startswith("/load-skill "):
                    print(engine.load_skill(line[12:]))
                elif line.startswith("/skill "):
                    engine.activate_skill(line[7:])
                elif line.startswith("/save "):
                    save_json(line[6:], engine.export_history())
                elif line.startswith("/load "):
                    engine.import_history(read_json(line[6:]))
                elif line.startswith("/mcp ") or line.startswith("/call "):
                    is_mcp = line.startswith("/mcp ")
                    rest = line[5:] if is_mcp else line[6:]
                    name, separator, data = rest.partition(" ")
                    require(separator and name and data, "Expected name and JSON")
                    value = loads(data)
                    if is_mcp:
                        attach(engine, name, value)
                    else:
                        print(dumps(engine.invoke(name, value)))
                elif line.startswith("/"):
                    raise ValueError("Unknown command")
                elif line:
                    print(engine.send(line))
            except Exception as exc:
                print(f"Error: {exc}", file=sys.stderr)


def main():
    """CLI 错误统一返回非零退出码；不输出带配置/凭据的异常对象 repr。"""
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    try:
        run_console()
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
