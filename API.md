# 接口与宿主接入

公开接口源码均包含中文 Doxygen/Javadoc 注释；实现按职责写段落注释。下面的最小示例说明如何脱离控制台接入自己的宿主。

## Token 配置与自动压缩

七种语言共用 JSON 字段：`context_budget`（默认 16000，输入估算预算）、`max_tokens`（默认 2048，每次响应输出上限）、`max_summary_tokens`（默认 1024，摘要输出上限）、`auto_compact`（默认 true）、`compression_threshold`（默认 90，预算使用百分比，范围 1–100）。`max_rounds` 默认 `0`，不限制工具轮数和每轮调用数量；正整数为宿主显式上限。输入与输出上限独立；供应商模型的实际限制仍生效。

| 语言 | 输出上限 | 压缩开关 | 压缩触发百分比 |
| --- | --- | --- | --- |
| C | `config.MaxTokens` | `config.AutoCompact` | `config.CompressionThreshold` |
| C++ / Python / Rust | `config.max_tokens` | `config.auto_compact` | `config.compression_threshold` |
| Go / C# | `Config.MaxTokens` | `Config.AutoCompact` | `Config.CompressionThreshold` |
| Java | `config.maxTokens` | `config.autoCompact` | `config.compressionThreshold` |

C 通过 `AI_CHAT_DefaultConfig()` 取得默认值，在 `AI_CHAT_Init` 前设置；其他版本在发送前串行修改配置。使用量以 UTF-8 字节/3 估算。每次发送先检查历史视图，达到阈值且有超过四条未摘要消息时，请求模型摘要较早部分；保留最近四条消息和完整原始归档。摘要请求禁用工具，输出 token 上限为 `min(max_tokens, max_summary_tokens)`；将旧摘要与新增历史滚动合并，优先保留目标、决定、待办及实际工具结果。摘要失败不更新覆盖边界；实际请求超过输入预算仍明确报错。摘要预算字段分别为 C 的 `MaxSummaryTokens`、C++/Python/Rust 的 `max_summary_tokens`、Go/C# 的 `MaxSummaryTokens`、Java 的 `maxSummaryTokens`。详细统计范围见 [历史和上下文](README.md#历史和上下文)。

## Python：MCP 与 Skill

入口为 `python/embedai`，Python 3.10+，仅使用标准库。可将 `python` 目录加入包搜索路径，或用 `python -m pip install -e ./python` 安装。

```python
import sys
from embedai import Engine, McpClient

with Engine() as chat:
    chat.config.provider = "openai"  # 或 anthropic。
    chat.config.base_url = "https://my-provider.example/v1"
    chat.config.model = "my-model"
    chat.config.api_key = obtain_key_from_host()
    with McpClient([sys.executable, "绝对路径/server.py"], timeout_ms=15000) as mcp:
        chat.attach_mcp("service", mcp)
        chat.activate_skill(chat.load_skill("绝对路径/my-skill"))
        chat.allow_tools = True
        markdown = chat.send("使用已注册工具检查宿主状态")
        archive = chat.export_history()
```

`Engine(http=transport)` 允许注入 `(url, headers, body) -> dict` 网络函数；认证头不得记录。`register_tool(Tool(name, description, schema, handler))` 注册同步宿主回调。所有方法及配置修改串行执行，UI 调度由宿主完成。`attach_mcp` 全部发现和预检成功后才注册；成功后引擎持有并负责关闭客户端，失败由调用者关闭，with 可确保异常清理。

Skill 只加载 UTF-8 SKILL.md 指令，不执行脚本，也不改变工具权限；只有预加载的名称可以被模型激活。完整协议范围、上下文摘要、归档格式、异常行为和中文 docstring 说明见 [Python 接入文档](python/README.md)。

## C99 与网络 HAL

C 接口位于 `c/include/embed_ai_chat/ai_chat.h`，网络 HAL 位于 `c/include/embed_ai_chat/hal_net.h`。两者均含中文 Doxygen 文档注释，说明参数、返回值、所有权和串行调用要求。使用 `AI_CHAT_Init`、`AI_CHAT_RegisterTool`、`AI_CHAT_Send`、`AI_CHAT_ExportHistory` 嵌入宿主；网络经 `HAL_NET_Init` / `HAL_NET_Post` / `HAL_NET_DeInit` 适配，命名采用 STM32 HAL 风格，但不依赖 STM32 头文件。

完整示例、驱动约定和 C 版本的功能范围见 [C 接入文档](c/README.md)。Go 使用 GoDoc，Rust 使用 Rustdoc，C# 使用 XML 文档注释；各语言实现也包含职责和关键步骤注释。

## C++

```cpp
#include <embed_ai_chat/framework.hpp>
using namespace embed_ai_chat;

Engine chat;
chat.config.provider = "openai";
chat.config.base_url = "https://api.deepseek.com/v1";
chat.config.model = "deepseek-chat";
chat.config.api_key = obtain_key_from_host(); // 宿主自己的会话凭据入口
chat.allow_tools = true;

chat.register_tool({
    "host_set_value", "Set host value from 0 to 100",
    Json::object({
        {"type", "object"},
        {"properties", Json::object({
            {"value", Json::object({{"type", "number"}, {"minimum", 0}, {"maximum", 100}})}
        })},
        {"required", Json::array({"value"})},
        {"additionalProperties", false}
    }),
    [&](const Json& args) -> Json {
        host.set_value(json_get<double>(json_at(args, "value")));
        return Json::object({{"value", host.value()}}); // 真实回读
    }
});

auto answer = chat.send("把数值设置为 42");
auto archive = chat.export_history(); // 写入宿主项目容器
```

`Engine` 不可复制，所有操作串行执行。回调在 `send` 调用线程运行，界面宿主自行调度到 UI 线程。`Http` 是可选构造参数，签名为 `(url, headers, body) -> Json`；HTTP 错误应抛异常且不打印机密头。

```cpp
auto mcp = std::make_shared<McpClient>(
    std::vector<std::string>{"python", "绝对路径/server.py"}, 15000);
chat.attach_mcp("service", mcp);
chat.activate_skill(chat.load_skill("绝对路径/my-skill"));
```

CMake 宿主可使用 `add_subdirectory(<框架>/cpp)` 后 `target_link_libraries(MyHost PRIVATE embed_ai_chat)`。链接依赖通过目标传递；需要可找到 libcurl。示例控制台位于 `cpp/src/main.cpp`，无需加入真实宿主。

## C++ JSON 类型与取值

`using Json = json::Value;` 定义在 `cpp/include/embed_ai_chat/json_util.hpp`，是内部 JSON 库的原生值类型，无须额外数据模型转换。JSON 实现位于 `cpp/vendor/chali_json/json.hpp` 和 `json.cpp`，直接编入 `embed_ai_chat`，不依赖其他项目的路径。

```cpp
Json value = Json::object({
    {"title", "我的应用"},
    {"items", Json::array({1, 2, 3})}
});
Json parsed = json_parse(R"({"value":42})");
double number = json_get<double>(json_at(parsed, "value"));
std::string title = json_value(value, "title", "默认标题");
for (const Json& item : json_at(value, "items").requireArray()) {
    use_value(json_get<int>(item));
}
for (const auto& member : value.requireObject()) {
    use_member(member.key, member.value);
}
std::string text = value.dump(2);
```

底层 `json::parse` 返回含诊断的 `json::ParseResult`；框架入口 `json_parse` 将解析失败统一转为异常，保持严格 JSON 语法、4 MiB 输入限制、有效 Unicode 校验和非有限数值拒绝。底层解析器容器嵌套深度上限为 256。

`json_get<T>` 支持 JSON 值、string、bool、整数、浮点及 vector&lt;string&gt;。类型不符或整数越界会抛异常；不会把数字当作布尔值或把负数转换为 size_t。`json_value` 只有字段缺失时才使用默认值，已有字段类型错误仍会失败。

`json_at(const Json&, key)` 读取必需字段，缺失时失败；可写重载允许在已有对象中插入 null 字段。对象成员按插入顺序存储，新增成员可能使已有引用失效；不要跨修改长期保存字段引用。临时对象中的数组必须先保存到局部变量，再通过 requireArray 遍历，避免引用临时对象生命周期结束后的存储。

## Java

```java
import io.embedai.EmbedAiChat.*;
import static io.embedai.EmbedAiChat.*;

Engine chat = new Engine();
chat.config.provider = "openai";
chat.config.baseUrl = "https://api.deepseek.com/v1";
chat.config.model = "deepseek-chat";
chat.config.key = obtainKeyFromHost();
chat.allowTools = true;

chat.registerTool(new Tool(
    "host_set_value", "Set host value from 0 to 100",
    object("type", "object", "properties",
        object("value", object("type", "number", "minimum", 0, "maximum", 100)),
        "required", array("value"), "additionalProperties", false),
    args -> {
        host.setValue(args.get("value").getAsDouble());
        return object("value", host.getValue());
    }));
String answer = chat.send("把数值设置为 42");
var archive = chat.exportHistory();
```

```java
try (McpClient client = new McpClient(
        java.util.List.of("python", "绝对路径/server.py"), 15000)) {
    chat.attachMcp("service", client);
    chat.activateSkill(chat.loadSkill(java.nio.file.Path.of("绝对路径/my-skill")));
    String answer = chat.send("按 Skill 检查宿主");
}
```

把 `EmbedAiChatJava.jar` 与 Gson JAR 放进宿主 classpath 即可，不需要启动 `Console`。Java 接口同样要求串行访问；`Handler` 与 `Http` 均可用 lambda 注入。

## Go

核心包：`go/embedai`，控制台：`go/cmd/embedai`。使用 GoDoc 注释，所有配置修改和 Send 串行调用，回调在调用线程执行。

宿主模块可以用本地替换引用：

```go
require embedai.chat/framework v0.0.0
replace embedai.chat/framework => ../EmbedAiChat/go
```

```go
import (
    "context"
    "embedai.chat/framework/embedai"
)

chat := embedai.New(nil) // 可传入自己的 HTTP 回调
chat.Config.Model = "deepseek-chat"
chat.Config.BaseURL = "https://api.deepseek.com/v1"
chat.Config.Key = obtainKeyFromHost()
chat.AllowTools = true

state := embedai.Object{"value": 0}
err := chat.RegisterTool(embedai.Tool{
    Name: "host_set_value",
    Description: "Set value from 0 to 100",
    Schema: embedai.Object{
        "type": "object",
        "properties": embedai.Object{"value": embedai.Object{"type": "number", "minimum": 0, "maximum": 100}},
        "required": []any{"value"}, "additionalProperties": false,
    },
    Handler: func(args embedai.Object) (any, error) {
        state["value"] = args["value"]
        return embedai.Object{"value": state["value"]}, nil
    },
})
// 宿主处理 err；Send 返回回答与错误，不自动重试。
answer, err := chat.Send(context.Background(), "把数值改为 42")
archive := chat.ExportHistory()
```

`NewMcpClient(argv, timeoutMs)` 返回客户端及错误，使用 `defer client.Close()` 清理，再通过 `AttachMCP` 注册。Skill 使用 `LoadSkill`、`ActivateSkill`。标准 JSON 解码使用 json.Number，以便保留归档中的整数；宿主结果必须可 JSON 序列化。

## Rust

核心：`rust/src/lib.rs`，MCP：`rust/src/mcp.rs`，控制台：`rust/src/console.rs`。以 Cargo path dependency 嵌入：

```toml
[dependencies]
embed-ai-chat = { path = "../EmbedAiChat/rust" }
```

```rust
use embed_ai_chat::{json, Engine, Tool};
use std::cell::RefCell;
use std::rc::Rc;

let mut chat = Engine::default();
chat.config.model = "deepseek-chat".into();
chat.config.base_url = "https://api.deepseek.com/v1".into();
chat.config.key = obtain_key_from_host();
chat.allow_tools = true;

let state = Rc::new(RefCell::new(json!({"value": 0})));
let host = state.clone();
chat.register_tool(Tool {
    name: "host_set_value".into(),
    description: "Set value from 0 to 100".into(),
    schema: json!({"type":"object", "properties":{"value":{"type":"number","minimum":0,"maximum":100}}, "required":["value"], "additionalProperties":false}),
    handler: Box::new(move |args| {
        host.borrow_mut()["value"] = args["value"].clone();
        Ok(host.borrow().clone())
    }),
})?;
let answer = chat.send("把数值改为 42")?;
let archive = chat.export_history();
```

可通过 `Engine::new(Some(http_callback))` 替换网络实现。`McpClient::connect(&argv, timeout_ms)` 完成握手后，用 `Arc<Mutex<McpClient>>` 传给 `attach_mcp`；连接通过 Drop 释放，也可主动 close。所有修改接口使用 `&mut self`，保持顺序执行。API 使用 Rustdoc，源码与生成文档都包含内部方法说明。

## C#

核心工程：`csharp/EmbedAiChat/EmbedAiChat.csproj`；控制台工程：`csharp/Console/Console.csproj`。宿主通过 ProjectReference 引用，或引用 `bin/csharp/EmbedAiChat.dll`。发布目录附带 XML 接口文档，供 IDE 显示说明。

```csharp
using EmbedAi;
using System.Text.Json.Nodes;

var chat = new Engine(); // 可传入 HttpTransport
chat.Config.Model = "deepseek-chat";
chat.Config.BaseUrl = "https://api.deepseek.com/v1";
chat.Config.Key = ObtainKeyFromHost();
chat.AllowTools = true;

var state = new JsonObject { ["value"] = 0 };
var schema = JsonNode.Parse("""{"type":"object","properties":{"value":{"type":"number","minimum":0,"maximum":100}},"required":["value"],"additionalProperties":false}""")!.AsObject();
chat.RegisterTool(new Tool("host_set_value", "Set value from 0 to 100", schema, args =>
{
    state["value"] = args["value"]?.DeepClone();
    return Task.FromResult<JsonNode?>(state.DeepClone());
}));
string answer = await chat.SendAsync("把数值改为 42");
JsonObject archive = chat.ExportHistory();
```

C# 网络及 MCP 是异步接口。`SendAsync` 支持 CancellationToken；用户取消不会撤销已执行的宿主操作。使用 `McpClient.ConnectAsync` 连接、`AttachMcpAsync` 注册，并通过 using/Dispose 关闭。框架不捕获 UI 同步上下文，宿主回调必须自行调度 UI 操作。JsonNode 通过深拷贝保证工具结果与归档不被后续修改影响。

## 三种新增语言的接口对照

| 能力 | Go | Rust | C# |
| --- | --- | --- | --- |
| 宿主注册 | RegisterTool | register_tool | RegisterTool |
| 手动调用 | Invoke | invoke | InvokeAsync |
| MCP 注册 | AttachMCP | attach_mcp | AttachMcpAsync |
| 加载 / 激活 Skill | LoadSkill / ActivateSkill | load_skill / activate_skill | LoadSkill / ActivateSkill |
| 聊天 | Send(ctx, text) | send(text) | SendAsync(text, token) |
| 历史 | ExportHistory / ImportHistory | export_history / import_history | ExportHistory / ImportHistory |
| 清空 | Clear | clear | Clear |
| 资源释放 | Close | Drop / close | Dispose |

新版本与 C++/Java 使用相同的版本 1 历史格式，默认预算、工具轮次、消息大小和 stdio MCP 协议范围保持一致。所有宿主修改都通过注册回调，不提供任意脚本执行工具。

## 公共方法对照

| C++ | Java | 输入 / 输出与失败行为 |
| --- | --- | --- |
| register_tool(Tool) | registerTool(Tool) | 注册唯一工具名、说明、schema、回调；非法定义抛异常 |
| tools() | tools() | 返回工具定义数组，不包含处理器或密钥 |
| invoke(name, args) | invoke(name, args) | 权限及 schema 校验后执行；手动调用错误直接抛异常 |
| attach_mcp(prefix, client) | attachMcp(prefix, client) | 发现全部工具页后注册，失败不部分注册 |
| load_skill(directory) | loadSkill(Path) | 返回 Skill 目录名称，仅加载文本 |
| activate_skill(name) | activateSkill(name) | 激活已加载 Skill；空名称解除 |
| send(text) | send(text) | 返回 Markdown，工具错误记录后回传模型；网络错误直接抛异常 |
| export_history() | exportHistory() | 返回版本 1 JSON 归档 |
| import_history(archive) | importHistory(archive) | 先完整校验，再替换历史，不重放工具 |
| clear() | clear() | 清空消息、摘要和审计，不清空工具配置 |
| McpClient::tools() | McpClient.tools() | 分页读取工具，最多 100 页 |
| McpClient::call(name,args) | McpClient.call(name,args) | 返回原始 content / structuredContent / isError |

默认工具包含 `assistant_reply`、`assistant_end`、`skill_list` 和 `skill_activate`，也受 `allow_tools/allowTools` 限制。宿主控制范围完全由宿主注册的回调决定，框架不提供任意命令执行工具。

## 历史格式

```json
{
  "version": 1,
  "messages": [{"role":"user","content":"原始 Markdown"}],
  "summary": "模型生成的较早对话摘要",
  "covered": 0,
  "audit": [{"name":"host_set_value","arguments":"{\"value\":42}",
             "result":{"ok":true,"result":{"value":42}}}]
}
```

`covered` 是原始 messages 数组中已被摘要覆盖的前缀长度。`audit` 包含模型调用的参数和实际结果，手动 `invoke` 由宿主自行记录。MCP 的 `isError:true` 会在模型工具结果外层体现为 `ok:false`，原始 MCP 返回仍保留。调用已经执行、但最终模型网络请求失败时，审计不会回滚；不能据聊天错误盲目重发动作。

归档只负责数据，不做注册表加密或项目格式绑定。需要 3MF 等存储格式时，由宿主把 export 的 JSON 放入自己的项目容器。

## C 桌面 MCP 适配

链接可选 `embed_ai_chat_c_mcp`，包含 `embed_ai_chat/mcp_stdio.h`。`AI_CHAT_MCP_Attach(engine, prefix, argv_json, timeout_ms, &client)` 完成握手、分页工具发现和注册，失败撤回本次新增工具；调用者须先释放引擎，再 `AI_CHAT_MCP_Close(client)`。核心仍为独立 C99，不依赖 C++、stdio 子进程或 libcurl。

七种语言都将 MCP 工具实际返回值与调用 ID 对应回传模型：模型可以继续调用 MCP，或输出最终回答结束本轮。完整回归见 `tests/verify_mcp_feedback.py`。

## C MCU 裁剪与分配器

`-DEMBED_AI_C_PROFILE=mcu` 面向 STM32F4/ESP32-C3，默认不构建 libcurl/桌面 MCP，裁掉 Skill、归档、摘要和 Anthropic，保留宿主工具、有界原文历史和近期审计。最小聊天可再关闭 TOOLS/HISTORY/AUDIT；需要滚动摘要时显式开启 COMPACTION。

`AI_CHAT_InitTypeDef` 新增 `RequestBuffer/RequestCapacity`、`ResponseBuffer`，复用既有 `ResponseCapacity` 指定宿主缓冲区容量；NULL 时使用分配器。缓冲区必须互不重叠并覆盖引擎生命周期。`AI_CHAT_SetAllocator(allocate, release)` 安装核心和 cJSON 共用分配器；全局设置，只能在无存活引擎/JSON 对象时更换。Send 返回值仍通过 `AI_CHAT_Free` 释放。

配置宏与 MCU 预算见 `c/include/embed_ai_chat/config.h`，详细构建与移植见 [MCU 指南](c/MCU.md)。被裁掉的 API 明确失败；关闭归档且开启摘要时会释放已覆盖原文，以维持有限内存中的滚动会话。桌面完整配置保持原始历史归档行为。


## 单次处理中的消息工具与事件回调

七种实现均内置以下两个工具，与外部 MCP 工具使用相同的模型工具调用接口、参数校验、工具总开关和审计流程。
它们是框架内置工具，不需要另启动 MCP 服务。

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `assistant_reply` | `{"text":"消息内容"}` | 发送一条非空消息，可以继续调用其他工具 |
| `assistant_end` | `{}` | 结束当前用户输入的处理，不清空会话历史 |

一次处理允许多个 MCP 服务、多轮工具调用及多次消息发送。模型调用结束工具后，
同批中后续工具不执行，并记为失败审计；不再发起下一轮模型请求。
没有工具调用的普通助手回复也结束处理，兼容关闭工具及旧模型的行为。
原有发送方法仍返回最后一条已发送消息；历史保存全部中途及最终回复。

宿主通过以下接口实时消费完整消息事件（不是逐 token 流）：

| 语言 | 接口 |
| --- | --- |
| C | `AI_CHAT_SendWithEvents(handle, text, &answer, callback, user_data)` |
| C++ | `engine.send(text, on_event)` |
| Java | `engine.send(text, events)` |
| Go | `engine.SendWithEvents(ctx, text, onEvent)` |
| Rust | `engine.send_with_events(text, &mut on_event)` |
| C# | `engine.SendWithEventsAsync(text, onEvent, token)` |
| Python | `engine.send(text, on_event=None)` |

事件按实际执行顺序通知：

```json
{"type":"assistant","text":"开始查询"}
{"type":"tool","audit":{"name":"a_query","arguments":"{}","result":{"ok":true,"result":{}}}}
{"type":"assistant","text":"查询完成"}
{"type":"done"}
```

消息工具自身也生成工具审计及 `tool` 事件，随后生成 `assistant` 消息事件。
`done` 只在正常结束时产生。异常通过原有发送方法的异常/错误返回报告，不发送 `done`，
已完成的消息与工具审计保留，禁止据错误自动重发写操作。
控制台已使用事件回调即时显示消息，避免只显示最后一条或重复显示最终消息。

事件回调在发送流程中串行执行，不得重入引擎。C/C++/Rust 的事件是只在回调期间有效的只读借用；
需保留时自行深复制。Java/Go/C#/Python 提供独立副本。
C 回调返回 `AI_CHAT_OK`、Go 回调返回 `nil`、Rust 回调返回 `Ok(())` 表示继续；
错误状态或其他语言的回调异常中止处理，不自动重试。
C 的返回字符串仍需 `AI_CHAT_Free`；禁用工具的 MCU profile 不注册对话工具，但可接收普通回复事件。

### 数字样式字符串的类型边界

JSON `"1.0"` 是字符串，JSON `1.0` 是数字。`"001"`、`"1e3"`、`"-0.0"` 同样应保持字符串，
解析、序列化、工具参数及归档不得通过尝试数值解析猜测类型。
字符串可传给 `assistant_reply.text`，数值 `1.0` 不可以；
要求 number 的工具也必须拒绝字符串 `"1.0"`。

回归入口：`python -X utf8 tests/verify_conversation.py`，覆盖七语言、双协议、两个 MCP 会话、
多次消息、主动结束及结束后禁止操作、后续网络失败、数字样式字符串。
C++ 原生 JSON 测试另覆盖根字符串、混合对象/数组、类型读取与序列化往返。
