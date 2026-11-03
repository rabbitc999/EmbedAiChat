# 验证记录

## STM32F4 / ESP32-C3 MCU 裁剪（2026-11-03）

- 新增 desktop/mcu CMake profile、独立功能开关和编译预算；MCU 默认不构建或查找 curl、stdio MCP、线程库、文件系统或 C++。新增静态收发缓冲区与核心/cJSON 共用分配器钩子。
- `python tests/verify_c_mcu.py` 三种 MSVC 主机配置通过：基础 MCU、最小无历史聊天、可选滚动摘要。测试初始化各分配点失败清理、分配/释放配对、静态缓冲不足时禁止发送、编译裁剪 API 明确失败、宿主工具真实结果反馈、可选摘要连续 16 轮。
- 指定 fixture 的核心+JSON 分配峰值：最小聊天 2243 字节、基础工具/历史 6148 字节、800 字节消息连续 16 轮滚动摘要 24034 字节；另有测试静态 I/O 6144/6144/10240 字节。未包含分配器开销、栈、SDK、Wi-Fi 或 HTTP/TLS，不代表整机最坏 RAM。
- GCC C99 主机编译和基础 MCU 可执行测试通过；STM32 BSP 薄适配器及通用生命周期示例的 GCC 编译检查通过。独立无 curl/无 MCP 的可移植核心构建通过。
- 桌面完整回归通过；最后一次 C 调整后，C 核心/HAL、三组 C HTTP 验收，以及仅 C 的五组 token/工具和三组 MCP 反馈验收再次通过。
- ESP-IDF HTTPS 组件、menuconfig 裁剪入口与 STM32 BSP 接口已提供。当前环境未安装 ARM/ESP-IDF 交叉工具链，ESP-IDF 组件尚未通过 SDK 编译，未进行目标板烧录或联网实测。接入及裁剪差异见 `c/MCU.md`。

## 七语言 MCP 结果反馈与真实 DeepSeek 验收（2026-11-03）

- C 桌面端新增独立 C99 `embed_ai_chat_c_mcp` 适配库，支持 stdio 握手、分页发现、工具调用、结果回传、失败反馈与超时关闭；可移植 C 核心仍不依赖操作系统进程或 C++。MSVC Release 构建与 MinGW GCC C99 编译检查通过。POSIX 路径未在 Linux/macOS 实测。
- C、C++、Java、Go、Rust、C#、Python 各通过真实 `deepseek-chat` API 的两个场景，共 14 个场景：工具生成不可预知 ticket 后模型直接回答；模型读取 ticket 后继续调用 confirm_ticket，再读取实际 receipt 作答。校验审计、调用顺序、后续调用参数与最终答案都来自真实 MCP 返回值。
- 密钥仅通过进程 stdin/内存使用，不写配置、源码、历史或验证记录；测试检查控制台输出和归档不含密钥。真实测试为显式可选，默认回归使用本地模型。
- 真实接入发现并修复 C/C++ Windows libcurl 未使用系统信任库的问题，以及 Rust JSON 请求重复 Content-Type 导致的 HTTP 415。保留 TLS 证书与主机名验证。
- `./verify.ps1` 全部通过。新增 MCP 反馈三组验收：七语言 × 双协议 × 三种决策（结束回答、继续调用、处理失败）共 42 个场景；C 分页/手动调用/重复连接保留已有工具；C 无效 schema 注册回滚、循环游标与 100 ms 握手超时。
- 七语言 token/滚动摘要五组验收继续通过，包括最大输出 token、独立摘要预算、默认 90% 可配置阈值、连续滚动摘要、失败保留原文、默认不限轮数以及单轮超过 16 次工具调用。

真实 API 验收入口：`python tests/verify_mcp_feedback.py --live-languages`；可通过 `--languages rust csharp` 选择指定版本。密钥从 `DEEPSEEK_API_KEY` 或 stdin 读取。

## 工具调用默认不设次数上限（2026-11-03）

- 七种语言的 `max_rounds` 默认改为 `0`（不限制），移除每轮最多 16 次工具调用的固定限制；正整数仍支持宿主显式设置轮次上限。
- 全七语言重新构建通过；`./verify.ps1` 全部通过。
- 新增跨七语言、OpenAI/Anthropic 双协议验收：单次对话连续执行 12 轮工具调用，以及单轮执行 17 次调用，均取得最终回答并保存全部成功审计。原显式 8 轮上限验收继续通过。
- `tests/verify_tokens.py` 现含五组验收，压缩、摘要和 token 配置测试继续通过。

## Token 上限与滚动摘要（2026-11-03）

- `./build.ps1`：七个语言版本全部构建成功，并更新对应控制台产物。
- `./verify.ps1`：Python 11 项边界测试、C 核心/HAL 测试、C 三组 HTTP 验收、C++ JSON 测试、六语言九组统一验收全部通过。
- 新增 `tests/verify_tokens.py` 四组验收通过，跨七种语言与 OpenAI/Anthropic 双协议验证：默认 90% 触发、阈值调整、禁用压缩、输出 token 上限、独立摘要预算、连续滚动摘要合并、摘要失败原文保留，以及非法配置在 HTTP 前拒绝。
- 滚动摘要输出上限为 `min(max_tokens, max_summary_tokens)`；完整原文保留。以 LangGraph/LangMem 的独立摘要状态与滚动摘要思路为主，未引入第三方压缩模型或实现逐 token 评分。

全部验收使用本地模型 fixture，不调用收费 API。输入 token 仍以 UTF-8 字节/3 估算，未承诺精确 tokenizer 或无损压缩。

## Python MCP / Skill 新增验证（2026-11-03）

实际测试环境为 Windows x64 / Python 3.12.3。运行时仅标准库，最低声明版本 3.10；本次未在 Python 3.10 或 Linux/macOS 实测。

- Python 边界测试 11 项通过：严格 JSON、bool/int 类型边界、Schema 未匹配分支中的未知约束拒绝、回调结果/定义深复制、send 重入拒绝、凭据隔离、Skill 格式及大小、MCP 协商版本和响应 ID 校验、超时后进程关闭与禁止复用、32 KiB 请求限额、MCP 工具注册冲突的原子性。
- `python -X utf8 tests/verify.py --python-only`：原有九组验收全部通过，覆盖 OpenAI/Anthropic MCP 工具循环、Skill 下一轮激活、手动 MCP 工具级失败、权限和参数约束、摘要保留原文、密钥不入归档、MCP 超时清理、最终 HTTP 503 保留实际审计、工具轮次上限及归档原子导入。
- 将 Python 与 C++、Java、Go、Rust、C# 一并运行统一九组验收，全部通过；六种语言完成 30 个方向的摘要归档互读。

使用本地 HTTP 模型和真实 stdio MCP 测试子进程，不调用收费模型。C 版本仍单独验收，不纳入 MCP 功能等价声明。

## C99 / HAL 新增验证（2026-11-03）

实际执行并通过：

- MSVC 19.44 x64 Release：`./build.ps1 -Languages c`，生成 C 核心静态库、可选 libcurl 适配库和 `bin/c/EmbedAiChatC.exe`。
- GCC 11.2 / MinGW C99 Debug：关闭 `EMBED_AI_C_CURL` 后独立构建核心，证明核心不依赖 C++ 和 libcurl。
- 两种编译器的 `c_core_hal` 测试：OpenAI/Anthropic 请求与工具结果转换、HAL BUSY 重入拒绝、超时恢复 READY、不重试、权限与 Schema 拒绝、Skill 加载/激活、归档不含密钥、无效归档原子拒绝、最终网络失败保留审计。
- `python -X utf8 tests/verify_c.py` 三组本地 HTTP 验收：真实 libcurl 的双协议工具循环和归档往返、修改宿主后 HTTP 503 保留审计、权限/参数校验和无效历史导入。

公开 C 接口与网络 HAL 已添加中文 Doxygen 注释，核心实现按职责分段说明；Go、Rust、C# 的现有源码分别具有 GoDoc、Rustdoc、XML 文档注释和实现步骤说明。

C 验收与原五语言统一验收分开。C 版本尚无 stdio MCP 子进程客户端和自动摘要，不纳入原有五语言功能等价声明。本次未执行真实云模型调用或 STM32 实机验证，未重新运行原五语言全部验收。

## 原五语言验证记录

平台：Windows x64，MSVC C++17、JDK 22（--release 17）、Go 1.27.1、Rust 1.98.1、.NET SDK 9.0.317。五个控制台均已构建。Go vet、Rust 格式检查、C# XML 文档编译及 Java 文档检查通过。

统一的九组验收对全部五个版本执行，使用本地 HTTP 模型及 stdio MCP fixture，不需要真实密钥，不调用收费模型：

1. OpenAI 兼容及 Anthropic 工具循环，含宿主实际回读、MCP 工具级错误和 Skill 激活后生效。
2. 工具权限、参数上下限、额外字段拒绝与归档保存。
3. MCP 分页发现、手动调用、Skill 发现/激活。
4. 自动摘要仍保留全部原文；五版本之间完成 20 个方向的归档互读。
5. MCP 超时关闭，不自动重试操作。
6. API Key 不回显、不进入归档。
7. 宿主修改后最终 HTTP 请求失败，实际审计仍然保留。
8. 工具循环在默认八轮后停止，实际操作与审计均为八次。
9. 无效归档导入失败，已有历史不被部分覆盖。

C++ 原生 JSON 测试同时验证原生值类型、Unicode/代理对、64 位整数、科学记数法、严格解析、输入大小和嵌套边界。

完整复验：`./verify.ps1`。源码测试入口为 `tests/verify.py`，可传入 --cpp、--go、--rust、--csharp。Java 默认通过打包后的 JAR 启动。

本次未执行真实云模型调用，也未实测 Linux/macOS。
