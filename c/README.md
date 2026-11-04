# C99 宿主接入与 HAL 移植

核心只使用 C99 标准库与随源码提供的 cJSON 1.7.19，不依赖 C++、操作系统进程或 libcurl。`hal/hal_net_curl.c` 是可选的桌面网络适配器。

面向 STM32F4 / ESP32-C3 的编译裁剪、静态缓冲区、自定义分配器、内存测试及板级接入见 [MCU 裁剪指南](MCU.md)。使用 `-DEMBED_AI_C_PROFILE=mcu` 可直接关闭桌面依赖，并按需关闭工具、Skill、历史、归档、摘要、审计和 Anthropic。

## 构建与运行

在项目根目录执行：

```powershell
./build.ps1 -Languages c
./run-c.cmd
ctest --test-dir build/c -C Release --output-on-failure
python tests/verify_c.py
```

单独构建核心（用于 MCU 或宿主自带网络）：

```powershell
cmake -S c -B build/c-core -DEMBED_AI_C_CURL=OFF
cmake --build build/c-core --config Release
```

交叉编译时传入自己的 `CMAKE_TOOLCHAIN_FILE` 并设置 `BUILD_TESTING=OFF`。也可直接将 `src/ai_chat.c`、`src/hal_net.c`、`vendor/cjson/cJSON.c` 加入工程，头文件搜索路径为 `c/include` 和 `c/vendor/cjson`。宿主 CMake 可 `add_subdirectory(<项目>/c)` 并链接 `embed_ai_chat_c`，桌面适配器另链接 `embed_ai_chat_c_curl`。

核心使用动态内存；默认接收缓冲区约 4 MiB，可通过 `ResponseCapacity` 调小。它不是零堆或恒定内存实现，MCU 应根据 RAM 和响应大小配置缓冲区、上下文预算与工具轮数。

## 最小嵌入示例

```c
#include <embed_ai_chat/ai_chat.h>

/* MyNetDriver 是宿主实现的静态 HAL_NET_DriverTypeDef。 */
extern const HAL_NET_DriverTypeDef MyNetDriver;

int chat_once(void) {
    HAL_NET_HandleTypeDef hnet = {0};
    AI_CHAT_HandleTypeDef *hchat = NULL;
    AI_CHAT_InitTypeDef init = AI_CHAT_DefaultConfig();
    char *answer = NULL;
    int ok = 0;

    hnet.Driver = &MyNetDriver;
    init.Model = "my-model";
    init.BaseURL = "https://my-provider.example/v1";
    init.APIKey = ""; /* 从宿主会话凭据入口取得，不写入配置文件。 */
    init.ResponseCapacity = 65536;

    if (HAL_NET_Init(&hnet) != HAL_NET_OK) { return 0; }
    if (AI_CHAT_Init(&hchat, &init, &hnet) == AI_CHAT_OK) {
        if (AI_CHAT_Send(hchat, "你好", &answer) == AI_CHAT_OK) {
            /* 交给宿主显示 Markdown 原文。 */
            AI_CHAT_Free(answer);
            ok = 1;
        }
    }
    AI_CHAT_DeInit(hchat);
    HAL_NET_DeInit(&hnet);
    return ok;
}
```

## 网络 HAL

命名采用 STM32 HAL 习惯：`HAL_NET_HandleTypeDef`、`HAL_NET_Init`、`HAL_NET_Post`、`HAL_NET_DeInit`、`HAL_NET_GetState`。状态码使用 `HAL_NET_OK/ERROR/BUSY/TIMEOUT`，避免和 STM32 已有 `HAL_StatusTypeDef` 冲突。

`HAL_NET_DriverTypeDef` 提供 Init、DeInit、Post 三个函数指针。Init/DeInit 可空；Post 必填。驱动表和 `UserData` 由宿主持有，生命周期覆盖句柄。驱动可封装 lwIP + TLS/HTTP 客户端、AT 模组或宿主网络接口。

Post 必须遵守以下契约：

1. 同步完成一次 HTTP POST，并让整个操作受 `Timeout` 毫秒限制。
2. 读取请求 URL、Headers、Body/BodySize；这些指针仅在调用期间有效，认证头不得记录。
3. 向调用者提供的 Data 写响应正文，Size 必须小于 Capacity，并设置真实 HTTP StatusCode。缓冲区不足立即失败，不能返回截断成功。
4. 不自动重试，不修改公共 State。公共入口维护 RESET → READY → BUSY → READY，超时也回到 READY；非 2xx 转为 ERROR。
5. 实现证书校验及 HTTPS 策略，不将 TLS 错误静默降级。桌面 curl 驱动只接受 HTTPS 和带端口的本机回环 HTTP，关闭重定向。

BUSY 用于拒绝同步重入，不是线程锁。引擎、网络句柄和回调须串行使用，回调内不得重入同一引擎。桌面宿主使用 `HAL_NET_CURL_Driver` 时，在应用生命周期内自行调用 `curl_global_init/cleanup`；控制台已处理。

## 工具与 Skill

`AI_CHAT_RegisterTool` 复制名称、说明、schema，借用回调 UserData。工具总开关默认关闭，`AI_CHAT_SetAllowTools(hchat, 1)` 后才允许执行。

回调参数是借用的只读 JSON 对象，回调必须用 `cJSON_Create*` 创建独立结果，并通过 `*result` 转交框架；不能返回参数对象或其子节点。宿主必须执行真实操作并回读状态。框架在模型工具执行后立即记录审计，即使下一轮 HTTP 失败也保留记录。手动 Invoke 不写模型审计。

支持的 JSON Schema 关键字为 `type`、`properties`、`required`、布尔 `additionalProperties`、`items`、`enum`、`minimum`、`maximum`、`minLength/maxLength`、`minItems/maxItems`；支持 `title/description` 注释。类型包括 object、array、string、number、integer、boolean、null。未知关键字在注册时拒绝；不提供完整 JSON Schema 实现。字符串长度按 UTF-8 码点起始字节估算，输入须由宿主保证为有效 UTF-8。默认不限制工具轮数与每轮调用数量，工具结果最大 1 MiB。

无文件系统场景使用 `AI_CHAT_LoadSkill(handle, name, text)` 加载宿主提供的文本，最多 64 KiB，要求 frontmatter 分隔标记。只检查入口格式，不解析完整 YAML。`AI_CHAT_ActivateSkill` 激活预加载名称，空名称清除。控制台 `/load-skill` 负责读取目录下 SKILL.md。内置 `skill_list` 和 `skill_activate` 工具和宿主工具共用权限入口。

## 历史、错误和内存

`AI_CHAT_Send` 成功返回由调用者 `AI_CHAT_Free` 释放的字符串；失败将输出指针置 NULL。`AI_CHAT_GetError` 返回引擎内部借用字符串，不得释放。Init 深复制配置，可通过 `AI_CHAT_SetAPIKey` 替换会话凭据；配置和密钥不进入归档。

`AI_CHAT_ExportHistory` 返回独立 JSON 对象，由调用者 `cJSON_Delete`。`AI_CHAT_ImportHistory` 校验后复制，失败不覆盖当前状态，不恢复连接或重放工具。归档使用原项目版本 1 的 messages、summary、covered、audit 字段，可导入其他版本生成的摘要视图。

C 版本支持按可配置阈值自动摘要（默认上下文预算的 90%），包含工具定义和协议结构的请求超过 ContextBudget 时直接拒绝。原始用户提问及已发生的审计仍保留。桌面端现提供独立 C99 stdio MCP 适配库 `embed_ai_chat_c_mcp`，支持配置中的 `mcp` 数组以及 `/mcp <前缀> <JSON argv数组>`；MCU 核心不依赖此适配库。

## 注释规范

公开头文件使用中文 Doxygen 文档注释，说明职责、参数、返回、指针所有权和调用限制。实现中的段落注释覆盖基础辅助、Schema 校验、Skill 加载、归档原子替换、请求上下文、供应商协议转换、HAL 传输、工具循环和桌面文件/密钥处理。第三方 cJSON 源码保持上游原样。

其他语言已有对应文档注释：Go 为 GoDoc，Rust 为 Rustdoc，C# 为 XML 文档；关键实现还有段落或步骤说明。原项目的生成文档位于 `docs`，本次 C 文档直接从头文件和本说明读取，未生成 HTML 文档。

## 桌面 MCP 与模型反馈

桌面构建包含 `embed_ai_chat_c_mcp`，源码为 `src/mcp_stdio.c`，公共头为 `embed_ai_chat/mcp_stdio.h`。Windows 使用原生管道与进程 Job，POSIX 使用 fork/exec 和 poll；严格 argv 不经过 shell。支持 initialize/initialized、分页 tools/list、tools/call，协议版本为 2025-11-25、2025-06-18、2024-11-05。

```c
#include <embed_ai_chat/mcp_stdio.h>
AI_CHAT_MCP *mcp = NULL;
cJSON *argv = cJSON_Parse("[\"python\",\"tests/mock_mcp_workflow.py\"]");
AI_CHAT_StatusTypeDef status = AI_CHAT_MCP_Attach(hchat, "flow", argv, 15000, &mcp);
cJSON_Delete(argv);
/* 成功后由模型调用 flow_issue_ticket / flow_confirm_ticket。 */
/* 使用结束时先释放引擎，再释放回调借用的 MCP 会话。 */
AI_CHAT_DeInit(hchat);
AI_CHAT_MCP_Close(mcp);
```

连接成功后工具加入同一白名单；默认工具开关仍由宿主配置。MCP 返回的 content、structuredContent 和 isError 保留，工具结果通过匹配的 tool_call_id 返回模型。模型可继续调用工具，也可给出最终回答结束本轮。工具失败以 ok=false 回传，不伪造执行成功。

单次 RPC 期限为 1–60000 ms，请求最多 32 KiB、响应行最多 4 MiB；超时关闭本会话创建的进程，不自动重发。重复名称、循环分页游标、非法 schema 或发现失败会撤回本次注册，保留已有工具。会话必须覆盖引擎生命周期，释放引擎之后才调用 Close；不得在引擎仍可能执行回调时提前释放会话。此模块仅本次实测 Windows；POSIX 路径尚未在 Linux/macOS 实测。


## 消息工具与实时事件

开启工具功能时（包括 MCU tools profile），内置 `assistant_reply` 和 `assistant_end`，
不依赖 Skill 或文件系统。消息工具参数为 `{"text":"内容"}`，结束工具为空对象。
`AI_CHAT_SendWithEvents(handle, text, &answer, callback, user_data)` 按序通知
`assistant`、`tool`、`done`；回调借用只读 cJSON，不得释放/保留或重入引擎，返回非 OK 状态中止处理。
最终 `answer` 仍由 `AI_CHAT_Free` 释放。原 `AI_CHAT_Send` 保持兼容并返回最后一条消息。
所有回复均计入历史容量限制，开启工具会增加两个工具定义的 RAM 占用。
工具被裁剪时仍能接收普通助手消息事件。完整协议见根目录 API.md。
