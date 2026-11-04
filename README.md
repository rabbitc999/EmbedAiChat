# EmbedAiChat

可独立运行、可嵌入宿主的 AI 控制台框架。C++17、Java 17+、Go、Rust、C# 和 Python 六个版本实现同样的核心接口行为，提供四项业务能力：AI 聊天、MCP 工具调用、Skill 加载/激活、宿主白名单控制。通过标准接口集成到任意宿主程序，提供独立控制台入口。

新增独立 C99 实现，使用 STM32 HAL 风格的网络抽象，支持 OpenAI/Anthropic 聊天、宿主白名单工具循环、Skill 文本加载和版本 1 历史归档。C 核心不依赖 C++ 或 libcurl，桌面 HTTP 驱动可选。C 桌面端通过独立 C99 stdio MCP 适配库支持工具发现与调用，可移植核心仍不依赖进程 API；压缩后预算仍超限会明确拒绝。接入、内存所有权及移植说明见 [C 版本文档](c/README.md)。

C 提供 STM32F4 / ESP32-C3 的 MCU 裁剪预设、静态收发缓冲区、共用分配器钩子，以及独立开关的工具、Skill、归档、历史、滚动摘要、审计和 Anthropic。最小聊天、基础 MCU、可选滚动摘要三种主机测试配置已通过；目标板仍需网络 BSP/SDK 接入和实测。详见 [MCU 指南](c/MCU.md)。

Python 3.10+ 版本仅使用标准库，包含完整 stdio MCP 会话、Skill 加载/激活、工具循环、自动摘要与历史归档。中文接口文档注释及实现段落注释随源码提供，接入示例见 [Python 文档](python/README.md)。

## 直接运行

项目提供七种语言的控制台入口。C 版本的命令与功能范围见其专用文档，各版本共用模型配置格式：

| 版本 | 启动脚本 | 运行条件 |
| --- | --- | --- |
| C | run-c.cmd | Windows x64；`./build.ps1 -Languages c` 构建 |
| C++ | run-cpp.cmd | Windows x64 |
| Java | run-java.cmd | Java 17+ |
| Go | run-go.cmd | Windows x64，无需安装 Go |
| Rust | run-rust.cmd | Windows x64，无需安装 Rust |
| C# | run-csharp.cmd | .NET 9 Runtime |
| Python | run-python.cmd | Python 3.10+，无第三方运行时依赖 |

控制台保留 Markdown 原文，不制作图形界面。

1. 编辑 `examples/config.deepseek.json`，填写供应商、基础地址和模型 ID；配置中不要保存密钥。
2. 启动控制台，输入 `/key`，在下一行输入 API Key。真实终端隐藏输入；重定向 stdin 用于自动测试，不回显密钥。
3. 输入普通文字聊天。要允许 AI 操作宿主或 MCP，输入 `/allow on`。
4. 输入 `/exit` 退出。密钥只在内存中，退出后重新输入；凭据由用户或宿主显式提供。

| 服务 | provider | base_url 示例 | model 示例 |
| --- | --- | --- | --- |
| OpenAI 兼容 | openai | 供应商提供的兼容基础地址 | 账号可用的模型 ID |
| Anthropic | anthropic | https://api.anthropic.com/v1 | 账号可用的模型 ID |
| Kimi | openai | https://api.moonshot.cn/v1 | 账号可用的模型 ID |
| DeepSeek | openai | https://api.deepseek.com/v1 | deepseek-chat |

这些地址只是可编辑示例，不固定账号模型列表。需要支持工具调用的模型才能控制宿主。

## 控制台命令

| 命令 | 行为 |
| --- | --- |
| `/help` | 显示命令 |
| `/key` | 下一行读取本次会话密钥 |
| `/tools` | 列出可调用工具及参数 schema |
| `/allow on`、`/allow off` | 开关全部工具执行；默认关闭 |
| `/load-skill <目录>` | 读取目录下 SKILL.md，名称取目录名 |
| `/skill <名称>` | 激活已加载 Skill；`/skill ` 空名称解除激活 |
| `/call <工具名> <JSON对象>` | 手动调用同一套白名单工具 |
| `/mcp <前缀> <JSON argv数组>` | 启动 MCP 服务并注册其工具 |
| `/host` | 查看演示宿主的实际状态 |
| `/audit` | 查看模型执行工具的结果，包括失败记录 |
| `/save <文件>`、`/load <文件>` | 保存/载入聊天、摘要及工具审计 |
| `/clear` | 清空历史、摘要及审计 |
| `/exit` | 退出并释放 MCP 子进程 |

路径命令把命令后整段作为路径，不需要额外双引号；MCP 命令采用严格 JSON 数组，Windows 路径建议使用 `/`。单次模式：`EmbedAiChatCpp --config config.json --ask "问题"`，Java 主类使用相同参数。单次模式不读取密钥，适用于无鉴权本地测试或宿主通过 API 自行设置 key；真实 API 使用交互 `/key` 或核心接口。

```text
/allow on
/call host_set_title {"title":"我的应用"}
/call host_set_value {"value":42}
/host
/load-skill examples/skills/demo-host
/skill demo-host
请把宿主的数值改为 60，并告诉我实际回读结果。
```

演示宿主只有 title 和 0–100 的 value，状态保存在当前进程内。将回调替换为自己的程序 API 后才会操作真实宿主。

## MCP

使用 stdio JSON-RPC，支持 initialize、notifications/initialized、分页 tools/list、tools/call，明确协商 2025-11-25、2025-06-18、2024-11-05 会话协议。暂不实现 Streamable HTTP/SSE、2026 新版无状态握手、MCP server、sampling、elicitation 或资源订阅。服务端主动请求不支持的客户端能力会收到 -32601。

依据：[MCP 会话生命周期](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2025-11-25/basic/lifecycle.mdx)、[工具协议](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2025-11-25/server/tools.mdx)。

```text
/mcp test ["python","tests/mock_mcp.py"]
/allow on
/call test_echo {"text":"你好 MCP"}
```

也可在配置的 `mcp` 数组中加入 `{"prefix":"test","command":["python","绝对路径/mock_mcp.py"],"timeout_ms":15000}`。工具名变为 `prefix_originalName`，只接受 1–64 位字母、数字、下划线，发现冲突会失败。命令是 argv，不通过 shell；stderr 留作服务日志，stdout 必须只有协议消息。请求上限 32 KiB，响应行上限 4 MiB，超时 1–60000 ms；超时关闭自身启动的连接，不自动重发操作。再次修改前应先检查宿主状态。

## Skill

Skill 是 UTF-8 `SKILL.md` 指令包，必须含 YAML frontmatter，最大 64 KiB，目录名允许字母、数字、下划线和连字符。`load_skill/loadSkill` 只加载文本；`activate_skill/activateSkill` 把它加入后续模型请求的系统指引。模型也可调用 `skill_list` 和 `skill_activate`。模型只能激活宿主预加载的 Skill，不能任意访问文件。

不自动执行 Skill 中的脚本，不因 Skill 文案新增宿主权限。需要执行某种动作时，由宿主另外注册带 schema 的工具回调。示例在 `examples/skills/demo-host/SKILL.md`。

## 核心接口与嵌入

C++ 公共入口：`cpp/include/embed_ai_chat/framework.hpp`；Java：`java/src/main/java/io/embedai/EmbedAiChat.java`；Go：`go/embedai`；Rust：`rust/src/lib.rs`；C#：`csharp/EmbedAiChat`；Python：`python/embedai`。接口和内部方法有 Doxygen、Javadoc、GoDoc、Rustdoc、C# XML 或 Python docstring 文档注释，实现代码按校验、网络适配、工具循环、压缩、归档、进程生命周期分段注释。参数、失败行为、回调执行线程与密钥边界可在源码和 [接口文档](API.md) 中查看。

除独立说明的 C 版本外，六个版本均提供可单独嵌入的核心接口：

- `register_tool/registerTool`：注册宿主回调及参数约束。
- `attach_mcp/attachMcp`：注册外部 MCP 工具。
- `load_skill/loadSkill`、`activate_skill/activateSkill`：加载与激活 Skill。
- `send`：聊天与有界工具循环，返回 Markdown 原文。
- `export_history/exportHistory`、`import_history/importHistory`：项目内存储适配点。
- `Http`：替换网络传输，用于宿主自己的认证、代理或测试。

全部核心方法及配置修改由宿主串行调用。同步版本回调在调用线程执行；C# 为异步接口，不保证捕获 UI 同步上下文。UI 宿主负责把操作调度到自己的 UI 线程。C++ 引擎禁止复制，MCP 使用 shared_ptr 保持存活；Java MCP 使用 try-with-resources 或 finally 关闭；Go 使用 Close，Rust 使用 Drop/close，C# 使用 IDisposable。

## 历史和上下文

JSON 归档版本为 1，六种完整实现语言可以互读，C 也使用相同归档字段。原始聊天完整保留，自动摘要只替换发送给模型的历史视图，保留最近四条消息；存储不包含 API Key、MCP 命令或可执行回调。载入历史不会恢复工具连接或自动重放操作。失败后可查看 audit；最近四次实际工具结果也会作为数据补入后续请求，避免网络失败后遗忘已发生的操作。

上下文按 UTF-8 字节/3 估算，并计入系统指引、工具定义和返回值，不是供应商精确 tokenizer。七个版本均支持以下配置（编辑 `examples/config.deepseek.json`，重新启动生效；嵌入宿主通过配置接口设置）：

| 字段 | 默认值 | 范围与作用 |
| --- | --- | --- |
| `context_budget` | `16000` | 输入上下文估算预算，2000–1000000 |
| `max_tokens` | `2048` | 每次模型响应的最大输出 token，1–1000000；聊天使用该上限，摘要另受 `max_summary_tokens` 限制；供应商模型仍可能有更低限制 |
| `max_summary_tokens` | `1024` | 滚动摘要独立输出上限，1–1000000；实际使用 `min(max_tokens, max_summary_tokens)` |
| `max_rounds` | `0` | 单次对话工具轮数上限，`0` 不限制；正整数为显式上限 |
| `auto_compact` | `true` | 是否自动摘要较早历史 |
| `compression_threshold` | `90` | 触发摘要的上下文预算使用百分比，1–100；例如 `80` 表示 80% |

发送新消息前，历史发送视图的估算用量达到 `context_budget × compression_threshold / 100`，且有超过四条未摘要消息时，自动摘要较早历史并保留最近四条消息。摘要需要一次模型请求，会消耗 token；原始聊天完整保留在归档中。触发判断使用历史视图（包含摘要和近期工具审计），实际 HTTP 请求另计系统指引、工具定义与返回值并强制检查输入预算。单条长消息、系统指引或工具结果可能直接撑满预算；无法压缩或压缩后仍超限时明确拒绝，不截断原文。摘要失败不会更新摘要和覆盖边界。

压缩采用滚动摘要：将上一版摘要与尚未覆盖的较早历史合并，更新当前目标、约束、决定、待办、关键标识和工具实际结果，去除重复和过时信息。按层分配预算：系统指引和最近四条消息保留原文；旧历史摘要的输出上限为 `min(max_tokens, max_summary_tokens)`，用于控制摘要长度。

主要参考 [LangGraph/LangMem 滚动摘要](https://langchain-ai.github.io/langmem/guides/summarization/)：独立保存完整历史、摘要和覆盖边界；达到阈值时仅对尚未覆盖的历史与旧摘要进行合并更新；通过 `max_summary_tokens` 独立限制摘要输出。预算按系统指引、近期原文与历史摘要分层，参考 [LLMLingua 的预算控制思路](https://github.com/microsoft/LLMLingua/blob/main/DOCUMENT.md)。当前沿用七语言的轻量核心，不直接依赖 LangGraph/LangMem 或 LLMLingua，也未实现 LLMLingua 的逐 token 评分，不保证摘要无损。

默认不限制单次对话的工具调用轮数和每轮调用数量，模型调用 `assistant_end` 或返回不带工具调用的最终回答时结束。`max_rounds` 默认 `0` 表示不限制，宿主可显式设置正整数上限；参数回调结果上限仍为 1 MiB。

JSON Schema 支持 object/array/string/number/integer/boolean/null、anyOf、enum、properties、required、additionalProperties、items、minimum/maximum，以及 min/maxLength、min/maxItems、min/maxProperties。`$ref`、pattern、oneOf 等不支持，出现时明确拒绝调用；不宣称执行了完整 JSON Schema 校验。

## 构建

C++：CMake 3.20+、C++17 编译器、libcurl 开发包（含 TLS 及其依赖）；内部 JSON 实现随源码提供，类型为 `json::Value`，只依赖 C++17 标准库。独立构建：

```powershell
cmake -S cpp -B build/cpp -DCMAKE_PREFIX_PATH="<libcurl安装前缀>"
cmake --build build/cpp --config Release
```

Java：JDK 17+，依赖 Gson 2.11.0。`java/build.ps1`（Windows）或 `sh java/build.sh`（Linux/macOS）下载固定版本并校验 SHA-256，然后使用 javac/jar 编译；也提供 Maven pom.xml。桌面包已经包含校验后的 Gson JAR，无需再次下载。

Windows 检查或构建全部七个版本：`./build.ps1`。可用 `./build.ps1 -Languages go,rust,csharp` 只构建指定版本。Go 需要 1.23+，Rust 依赖要求 1.88+，C# 使用 .NET 9 SDK；Go/Rust 首次构建会按锁定文件下载依赖。发行包附带 `deps/windows-x64` 开发依赖，脚本会自动使用；也可通过 `-DependencyPrefix "<libcurl安装前缀>"` 选择自己的依赖。构建完成后更新各语言的 `bin` 子目录，启动脚本可直接使用。C++ 内含 Windows/POSIX 管道实现；本次实测环境是 Windows x64 / MSVC、JDK 22 以 --release 17 编译、Go 1.27.1、Rust 1.98.1、.NET SDK 9.0.317，未实测 Linux/macOS。

`build.ps1` 默认也构建新增的 C 版本到 `bin/c`；C 的独立验收为 `ctest --test-dir build/c -C Release --output-on-failure` 和 `python tests/verify_c.py`。C 独立验收与含 Python 的六语言统一验收分开。

Python 直接运行源码，无须原生编译；`./build.ps1 -Languages python` 检查 Python 3.10+ 和源码语法。默认构建现包含七种语言。独立验收为 `python -m unittest discover -s python/tests -v` 和 `python tests/verify.py --python-only`，统一验收使用 `--python` 将 Python 纳入原五语言测试及两两归档互读。

## 验证

```powershell
./verify.ps1
```

原生 JSON 验证：`ctest --test-dir build/cpp -C Release --output-on-failure`，覆盖原生类型、Unicode、数值边界、严格解析和归档校验。

新增 `python tests/verify_mcp_feedback.py` 跨七语言验证 MCP 真实结果回传后结束回答、继续调用及失败结果回传，并验证 C MCP 发现回滚、循环游标和超时。`python tests/verify_mcp_feedback.py --live-languages` 可选进行七语言真实 DeepSeek 测试，密钥通过 stdin 或 `DEEPSEEK_API_KEY` 提供；默认验收不调用收费模型。

控制台测试使用本地 HTTP 模型与 stdio MCP fixture，不需要真实密钥，也不会调用收费模型。覆盖六种语言的 OpenAI/Anthropic 工具循环、MCP 分页和错误、Skill 对下一轮生效、真实宿主回读、参数及权限校验、摘要与原文保留、六种语言两两归档互读、密钥不入归档、MCP 超时关闭、最终 HTTP 失败保留审计、工具轮次上限及无效归档的原子校验。

## 许可

本框架采用 GNU AGPL-3.0 许可，见 LICENSE.txt。C++ JSON 组件与 Gson 使用 Apache-2.0；第三方许可及版本记录见 THIRD_PARTY.md。发行包包含框架源码、控制台程序、示例与文档。

## 项目维护

项目根目录包含独立 Git 仓库，默认分支为 `main`。源码位于 `c`、`cpp`、`java`、`go`、`rust`、`csharp`、`python`，示例和验收位于 `examples`、`tests`。构建产物与预编译依赖不纳入版本历史，Go 和 Rust 依赖锁定文件纳入版本控制，交付包通过 MANIFEST.json 记录文件校验值。

## 代码排版约定

所有版本的 `if`、`else`、`for`、`while`、`do` 语句体统一使用大括号，单行语句也不例外。`else if` 保持连续分支写法，每个 `if` 的语句体仍使用大括号。C++ 格式由 `.clang-format` 配置，C# 使用 `.editorconfig`，Go/Rust 使用各自标准格式工具；排版时保留接口文档注释和实现中的说明。第三方依赖源码保持各自格式。


## 多消息与主动结束

七种语言均支持一次用户输入调用多个 MCP 服务。AI 可以交替调用
`assistant_reply({"text":"进度或结果"})` 与其他工具，完成后调用 `assistant_end({})`。
消息发送本身也是工具调用，会保存审计。控制台立即展示每条消息，原发送接口仍返回最后一条消息。
嵌入式宿主可通过消息事件回调同步更新界面，接口表与事件约定见 [API.md](API.md#单次处理中的消息工具与事件回调)。

新功能回归：`python -X utf8 tests/verify_conversation.py`，可传 `--languages python java` 选择语言。
全部使用本地模型与两个 stdio MCP fixture，不调用付费 API。
