# STM32F4 / ESP32-C3 裁剪接入

核心为 C99，仅编译 `src/ai_chat.c`、`src/hal_net.c` 和 cJSON 即可。MCU profile 不查找或链接 libcurl、桌面 stdio MCP、线程库、文件系统或 C++。模型在远端服务运行，MCU 负责请求、结果解析和宿主工具回调。

## 三种配置

使用新的构建目录，避免复用桌面缓存。下面的命令在主机编译/测试裁剪配置；生成目标 MCU 固件时增加厂商工具链，并设置 `-DBUILD_TESTING=OFF`。

```powershell
# 基础 MCU：聊天 + 宿主工具 + 有界历史和审计。
cmake -S c -B build/c-mcu -DEMBED_AI_C_PROFILE=mcu
cmake --build build/c-mcu --config MinSizeRel
ctest --test-dir build/c-mcu -C MinSizeRel --output-on-failure

# 最小聊天：进一步关闭工具、跨轮历史和审计。
cmake -S c -B build/c-mcu-min -DEMBED_AI_C_PROFILE=mcu -DEMBED_AI_C_TOOLS=OFF -DEMBED_AI_C_HISTORY=OFF -DEMBED_AI_C_AUDIT=OFF

# 保留滚动摘要：适合可用 RAM 较多的板卡。
cmake -S c -B build/c-mcu-summary -DEMBED_AI_C_PROFILE=mcu -DEMBED_AI_C_COMPACTION=ON
```

| 选项 | MCU 默认 | 作用 |
| --- | --- | --- |
| `EMBED_AI_C_TOOLS` | ON | 宿主工具注册、schema 校验及执行结果反馈 |
| `EMBED_AI_C_HISTORY` | ON | 保留跨轮上下文；关闭后每次 send 只发送本轮消息和可选近期审计 |
| `EMBED_AI_C_AUDIT` | ON | 保留最近实际执行结果 |
| `EMBED_AI_C_SKILLS` | OFF | Skill 文本和模型发现/激活工具 |
| `EMBED_AI_C_ARCHIVE` | OFF | JSON 历史导入/导出 |
| `EMBED_AI_C_COMPACTION` | OFF | 滚动摘要，需要保留历史 |
| `EMBED_AI_C_ANTHROPIC` | OFF | Anthropic 消息转换；OpenAI 兼容协议始终保留 |
| `EMBED_AI_C_CURL` / `EMBED_AI_C_MCP` | OFF | 桌面网络与子进程，不用于 MCU |

裁掉的接口保留声明并返回明确失败；被裁掉的功能不能只通过运行时配置重新打开。`max_rounds=0` 仍表示工具轮数不设次数上限，内存和协议预算另外检查。MCP stdio 依赖进程，MCU 不运行该适配器；需要 MCP 时通过宿主/网关桥接，或把设备能力注册为 C 工具回调。

启用摘要时采用旧摘要与新增历史滚动合并的方式，默认 90% 触发，`CompressionThreshold` 可调整。MCU 默认关闭归档，因此摘要成功后会释放已覆盖的原文，只保留摘要和最近消息；若要求完整原文存储，应由宿主另行持久化，或开启归档并提高内存预算。桌面完整配置继续保留原文。

## 内存与默认预算

| 参数 | MCU 默认 |
| --- | --- |
| 最大 JSON/历史序列化大小 | 8192 字节 |
| 默认 HTTP 接收容量 | 4096 字节 |
| 输入上下文估算预算 | 2000 token，MCU 最小允许 128 |
| 最大输出 token | 256 |
| 摘要输出 token | 128 |
| 单个工具结果 | 1024 字节 |
| 审计记录 | 最近 4 条 |
| 原文历史 | 最多 16 条消息 |
| 随附 cJSON 最大嵌套深度 | 32 |

达到历史数量或字节上限时明确拒绝，不悄悄截断原文。可调用 `AI_CHAT_Clear`，或启用摘要；裁掉归档且开启摘要时，已覆盖历史会被释放。无历史配置不保留跨轮聊天。工具轮数上限与这些存储上限独立。

可使用 `-DEMBED_AI_C_MAX_JSON_BYTES=4096`、`-DEMBED_AI_C_MAX_TOOL_RESULT_BYTES=512`、`-DEMBED_AI_C_MAX_AUDIT_ENTRIES=2` 等覆盖限额，全部列表见 `CMakeLists.txt` 的 `limit` 循环。JSON 容量、静态缓冲区与上下文预算应一起调整，不能只增大 token 配置。请求中 UTF-8 字节/3 是估算，不是供应商 tokenizer。

`AI_CHAT_InitTypeDef.RequestBuffer/RequestCapacity` 与 `ResponseBuffer/ResponseCapacity` 可借用静态 RAM，避免把缓冲区放在任务栈上；两者必须不重叠，生命周期覆盖引擎，容量不足会失败。`examples/mcu_chat.c` 给出 8 KiB 请求、4 KiB 响应的完整生命周期示例。摘要需要足够容纳触发前上下文的请求区；例如 2000 token 的预算应为协议开销留出空间，不能配一个仅 1 KiB 的请求缓冲。

`AI_CHAT_SetAllocator(allocate, release)` 将核心和 cJSON 一起接到 RTOS heap 或固定内存池。它仍需要动态对象分配，并非零堆实现；应用可以从静态内存池实现这些分配函数。分配器必须对齐、支持释放，且只能在所有核心/JSON 对象创建前安装，所有对象释放后才能更换。FreeRTOS 可使用 `pvPortMalloc` / `vPortFree`。`AI_CHAT_Free` 释放 Send 返回值，JSON 对象用 `cJSON_Delete`。

主机 MSVC x64、短文本与本地 HAL fixture 的分配计数结果：

| 配置 | 核心 + JSON 堆分配峰值 | 测试静态收发缓冲 |
| --- | ---: | ---: |
| 最小聊天 | 2243 字节 | 6144 字节 |
| 基础 MCU（工具 + 历史） | 6148 字节 | 6144 字节 |
| 滚动摘要，连续 16 轮、每条 800 字节 | 24034 字节 | 10240 字节 |

这些是指定 fixture 的已申请字节峰值，不是最大 RAM 承诺，未计分配器头部/碎片、任务栈、Wi-Fi、SDK、HTTP/TLS、证书和固件其他模块。32 位目标的对象大小也不同；必须在目标板测峰值。无需网络的可重复矩阵为 `python tests/verify_c_mcu.py`。

## STM32F4 / CubeIDE / Keil

1. 将三个核心源文件加入 C99 工程，头文件目录为 `c/include` 和 `c/vendor/cjson`，全工程定义 `AI_CHAT_MCU=1`。
2. 将 `ports/stm32/hal_net_stm32.c` 加入工程；应用实现 `BSP_AI_HTTP_Post`。可用已有的 HTTPS 模组，或板上的 LwIP + TLS。适配器不会自行假设模组、网口或 RTOS。
3. 应用初始化网络后，设置 `HAL_NET_HandleTypeDef.Driver=&HAL_NET_STM32_Driver`，通过 `UserData` 传递板级上下文，调用 `HAL_NET_Init`，再创建聊天引擎。
4. BSP 按 HTTP 请求提供的 URL、认证头、正文和超时完成传输，保留证书及主机名校验。响应最多写 `Capacity-1` 字节，填写 `Size` 和真实 `StatusCode`，错误时不自动重发操作。

裸工程不使用 CMake 时，可通过 `AI_CHAT_USER_CONFIG="board_ai_config.h"` 指定配置头，在其中定义 `AI_CHAT_ENABLE_*` 和 `AI_CHAT_MAX_*`；所有核心编译单元及应用应使用一致配置。随附 cJSON 额外定义 `CJSON_NESTING_LIMIT=32`。启用浮点 JSON 需确保标准库链接选项支持相关转换。

STM32F4 是一系列不同内存容量的器件。以 [STM32F401 官方资料](https://www.st.com/en/microcontrollers-microprocessors/stm32f401/documentation.html) 的 64 KiB RAM 型号为例，最小聊天适合作为起点；HTTPS 若在外部模组完成，MCU 侧可少承担 TLS 缓冲。具体 BSP、时钟、DMA 和链接脚本由板级工程提供。

## ESP32-C3 / ESP-IDF

在项目顶层 `project()` 之前，把本仓库的 `c/ports/esp_idf` 加入 `EXTRA_COMPONENT_DIRS`。此组件编译核心和 `HAL_NET_ESP_IDF_Driver`，使用 SDK 自带 `json` 组件，避免同时编译两份 cJSON；其 JSON 嵌套限制以 SDK 配置为准。

`idf.py menuconfig` 的 **EmbedAiChat MCU** 菜单提供相同功能开关，以及 JSON、响应、token、工具结果和历史容量设置。响应容量不得超过最大 JSON 大小加一；编译时会拒绝不一致配置。

启用 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`，先由应用完成 Wi-Fi 联网和时间同步，再初始化 HAL 网络和聊天引擎：

```c
#include "hal_net_esp_idf.h"
HAL_NET_HandleTypeDef net = { &HAL_NET_ESP_IDF_Driver, NULL, HAL_NET_STATE_RESET, HAL_NET_OK };
/* net 应保存在会话全生命周期有效的位置；在工作任务中调用同步接口。 */
HAL_NET_Init(&net);
/* MCU_Chat_Init(&net, app_provided_key); */
```

适配器使用 [ESP-IDF 官方 HTTP Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/protocols/esp_http_client.html) 的 HTTPS POST、证书 bundle 和数据事件，直接写入调用方响应区；超出容量时明确失败，关闭重定向，不输出认证头。HTTP/TLS 分配由 ESP-IDF 管理，未包含在核心内存测量中；网络 I/O 超时遵循 SDK 的阻塞请求配置。

本次实际完成主机 C99 编译与三种裁剪配置测试；环境中未安装 ARM/ESP-IDF 交叉工具链，ESP-IDF 适配器尚未通过 SDK 编译，也未进行 STM32F4 或 ESP32-C3 烧录/联网实测。
