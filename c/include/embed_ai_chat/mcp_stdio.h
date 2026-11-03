#ifndef EMBED_AI_MCP_STDIO_H
#define EMBED_AI_MCP_STDIO_H
#include "ai_chat.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief 可选的桌面 stdio MCP 会话；C99 核心和 MCU 不依赖此模块。 */
typedef struct AI_CHAT_MCP AI_CHAT_MCP;
/**
 * @brief 以严格 JSON argv 启动进程，握手、分页发现并注册 prefix_name 工具。
 * @param client 成功返回会话；失败置 NULL，撤回本次注册，不修改已有工具。
 * @param timeout_ms 每次完整 RPC 的超时，1–60000 ms；请求 32 KiB、响应行 4 MiB。
 * @note 不通过 shell；回调执行和配置需串行。超时关闭会话，不自动重发。
 * 会话必须覆盖引擎生命周期；引擎释放后调用 Close，不恢复归档中的连接。
 */
AI_CHAT_StatusTypeDef AI_CHAT_MCP_Attach(AI_CHAT_HandleTypeDef *engine, const char *prefix, const cJSON *argv,
                                         unsigned timeout_ms, AI_CHAT_MCP **client);
/** @brief 关闭本会话创建的进程及管道；允许 NULL。不释放或访问引擎。 */
void AI_CHAT_MCP_Close(AI_CHAT_MCP *client);
#ifdef __cplusplus
}
#endif
#endif
