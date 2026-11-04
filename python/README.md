# Python：聊天、MCP 与 Skill

Python 3.10+，运行时仅使用标准库，无需安装第三方 MCP SDK 或模型 SDK。提供可独立嵌入的 `embedai` 包和演示控制台，包含中文 docstring 文档注释及实现段落注释。

## 运行

在项目根目录执行：

```powershell
python -X utf8 python/console.py --config examples/config.deepseek.json
```

Windows 可直接使用 `run-python.cmd`。控制台 `/key` 从下一行读取当前会话凭据，真实终端隐藏输入；不支持把凭据写入配置。默认禁止工具执行，使用 `/allow on` 启用。

```text
/mcp test ["python","tests/mock_mcp.py"]
/allow on
/call test_echo {"text":"你好 MCP"}
/load-skill examples/skills/demo-host
/skill demo-host
/call skill_list {}
/tools
/audit
/exit
```

支持原项目的 `/help`、`/key`、`/tools`、`/allow on|off`、`/load-skill`、`/skill`、`/mcp`、`/call`、`/host`、`/audit`、`/save`、`/load`、`/clear`、`/exit`。`/skill ` 后空名称解除激活。目录/文件参数采用命令后的整段路径；MCP command 采用 JSON argv 数组，不通过 shell。

模型配置与其他版本共用。Skill 配置中的相对目录以配置文件目录为基准；MCP 命令继承当前工作目录，可使用绝对路径避免歧义。配置示例：

```json
{
  "provider": "openai",
  "base_url": "http://127.0.0.1:8000/v1",
  "model": "my-local-model",
  "allow_tools": false,
  "context_budget": 16000,
  "max_tokens": 2048,
  "max_summary_tokens": 1024,
  "auto_compact": true,
  "compression_threshold": 90,
  "max_rounds": 0,
  "skills": ["skills/demo-host"],
  "active_skill": "demo-host",
  "mcp": [
    {
      "prefix": "service",
      "command": ["python", "C:/my-service/server.py"],
      "timeout_ms": 15000
    }
  ]
}
```

单次模式：`python python/console.py --config config.json --ask "问题"`，不读取密钥，适合无鉴权本地服务。云服务使用交互 `/key`，或宿主 API 设置 `engine.config.api_key`。

## 嵌入 Python 宿主

可将 `python` 目录加入宿主的包搜索路径，或执行 `python -m pip install -e ./python`。安装使用 setuptools，运行时没有第三方依赖。安装后也可用 `embedai-chat` 或 `python -m embedai` 启动。

```python
import sys
from embedai import Engine, McpClient, Tool

host = {"value": 0}

def set_value(arguments):
    # 在真实宿主中调用自己的 API，再返回实际回读值。
    host["value"] = arguments["value"]
    return dict(host)

with Engine() as chat:
    chat.config.provider = "openai"  # 也支持 anthropic。
    chat.config.base_url = "https://my-provider.example/v1"
    chat.config.model = "my-model"
    chat.config.api_key = obtain_key_from_host()

    chat.register_tool(Tool(
        "host_set_value", "设置宿主数值并回读",
        {
            "type": "object",
            "properties": {"value": {"type": "number", "minimum": 0, "maximum": 100}},
            "required": ["value"],
            "additionalProperties": False
        },
        set_value
    ))
    # with 确保即使 attach_mcp 失败也关闭新客户端；close 是幂等操作。
    with McpClient([sys.executable, "绝对路径/server.py"]) as mcp:
        chat.attach_mcp("service", mcp)
        chat.activate_skill(chat.load_skill("绝对路径/my-skill"))
        chat.allow_tools = True
        answer = chat.send("将数值设置为 42，并报告实际结果")
        archive = chat.export_history()
```

全部方法和配置由宿主串行调用。工具 handler 在调用线程执行；UI 宿主自行调度到 UI 线程，不得在回调中重入 `send`。参数、定义及回调结果在跨边界时深复制，避免后续修改污染已保存的审计。

`Engine(http=transport)` 支持宿主网络注入，签名为 `(url, headers, body) -> dict`。默认支持 HTTPS 和回环 HTTP，校验服务器证书，不跟随重定向、不自动重试，响应最大 4 MiB；代理可通过自定义 transport 接入。认证头属于敏感数据，宿主回调不得记录或放入异常文本。

## MCP 生命周期与边界

`McpClient(argv, timeout_ms=15000)` 完成 initialize 与 notifications/initialized，支持协商 `2025-11-25`、`2025-06-18`、`2024-11-05`。支持分页 tools/list、tools/call，保留 content、structuredContent、isError。未实现的服务端主动客户端请求回复 -32601；通知忽略。

每次 RPC 的超时包含阻塞写入和读取，范围 1–60000 ms。请求含换行最多 32 KiB，单条响应最多 4 MiB。分页最多 100 页，拒绝重复工具和循环游标。超时、JSON-RPC 错误、ID 不匹配或损坏响应关闭会话，不重发操作；此时先检查宿主状态，再决定是否再次编辑。

`attach_mcp` 完整发现、检查冲突后一次注册 `prefix_originalName`，名称限制 1–64 个字母、数字、下划线。成功注册后 `Engine.close()` 关闭该客户端；失败仍由调用者关闭。`McpClient` 自身也支持 with，关闭幂等。控制台退出、配置失败和异常均释放已启动会话。

当前仅实现 MCP stdio 会话客户端，不实现 Streamable HTTP/SSE、MCP server、sampling、elicitation 或资源订阅。

## Skill、工具校验与历史

`load_skill(directory)` 读取最多 64 KiB 的 UTF-8 `SKILL.md`，目录名为 Skill 名称，要求包含 YAML frontmatter 分隔段。入口只检查 frontmatter 结构，不解析完整 YAML。加载不自动激活，`activate_skill(name)` 将指令加入后续模型请求，空名称解除激活。

模型通过 `skill_list`、`skill_activate` 只能访问宿主预加载的名称；Skill 不执行脚本，也不扩大工具权限。工具总开关默认关闭，手动与模型调用共用 Schema 校验入口。

支持的 Schema 子集为 type、anyOf、enum、properties、required、布尔 additionalProperties、items、minimum/maximum、min/maxLength、min/maxItems、min/maxProperties；title/description/default 作为注释接受。未知约束（包括隐藏在未匹配分支或缺省属性中的约束）明确拒绝。布尔值不当作数字，字符串长度使用 Unicode 码点。

默认不限制工具轮数和每轮调用数量，`max_rounds: 0` 表示不限制；正整数为宿主显式上限。结果最大 1 MiB。模型执行后立即保存成功或失败审计；后续 HTTP 失败也不会抹去已发生的操作。最近四次审计作为数据补入后续请求，手动调用不写模型审计。

版本 1 归档与原五种语言共用 messages、summary、covered、audit 格式。导出不含配置、凭据、MCP 命令或回调；导入全部检查后原子替换，不重建会话或重放操作。默认上下文预算 16000，按 UTF-8 字节/3 估算，默认达到预算的 90% 时自动摘要（`compression_threshold` 可配置为 1–100，`auto_compact` 可关闭），摘要保留最近四条原文；摘要只更新模型视图，完整原文仍保留。超限无法压缩会明确报错。

## 验证与文档注释

```powershell
./build.ps1 -Languages python
python -X utf8 -m unittest discover -s python/tests -v
python -X utf8 tests/verify.py --python-only
```

`./verify.ps1` 将 Python 加入原五语言的统一验收，另执行 Python 边界测试和独立 C 验收。Python 单独验收使用本地模型和 MCP fixture，不需要密钥，不调用收费模型。

公开类、方法、模块具有中文 docstring，配置字段含说明；实现注释说明 MCP 超时/进程清理、注册原子性、Skill 权限、协议转换、工具审计、历史压缩/导入与文件原子保存。可用 `python -m pydoc embedai` 查看安装后的接口说明。


## 一次输入发送多条消息

```python
def on_event(event):
    if event["type"] == "assistant":
        print(event["text"], flush=True)

last_reply = chat.send("查询并处理", on_event=on_event)
```

模型通过 `assistant_reply` 发送消息，之后可以继续调用多个 MCP 服务；
通过 `assistant_end` 结束当前处理并等待下一条用户输入。
`last_reply` 是最后一条消息，不应在事件回调已经显示后再重复显示。
回调收到独立的 `assistant` / `tool` / `done` 事件；失败抛异常并保留已发送消息及审计。
