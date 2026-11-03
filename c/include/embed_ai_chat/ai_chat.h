#ifndef EMBED_AI_CHAT_H
#define EMBED_AI_CHAT_H
#include "hal_net.h"
#include "cJSON.h"
#include "config.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief C99 聊天引擎。不透明句柄；所有 API 与回调须在同一线程串行调用。 */
typedef struct AI_CHAT_HandleTypeDef AI_CHAT_HandleTypeDef;
/** @brief 操作结果。失败说明通过 AI_CHAT_GetError 读取，不包含认证头。 */
typedef enum { AI_CHAT_OK = 0, AI_CHAT_ERROR, AI_CHAT_NO_MEMORY, AI_CHAT_NETWORK_ERROR } AI_CHAT_StatusTypeDef;
/** @brief 模型配置；Init 深复制字符串，APIKey 不导出到历史。 */
typedef struct {
    const char *Provider; /**< openai 或 anthropic。 */
    const char *BaseURL; /**< 不含具体资源后缀。生产环境使用 HTTPS。 */
    const char *Model; /**< 宿主指定的模型 ID。 */
    const char *APIKey; /**< 当前会话凭据，允许空字符串。 */
    size_t ContextBudget; /**< UTF-8 字节/3 的估算预算，AI_CHAT_MIN_CONTEXT_BUDGET–1000000；超限拒绝。 */
    unsigned MaxRounds; /**< 工具循环上限，0 不限制，正整数为显式上限。 */
    uint32_t Timeout; /**< 每个 HTTP 请求的超时，毫秒，非零。 */
    unsigned CompressionThreshold; /**< 自动压缩预算使用百分比，1–100，默认 90。 */
    unsigned MaxSummaryTokens; /**< 滚动摘要的独立输出 token 上限，1–1000000。 */
    unsigned MaxTokens; /**< 每次响应输出 token 上限，1–1000000。 */
    int AutoCompact; /**< 1 自动摘要较早历史，0 禁用；原始归档保留。 */
    size_t ResponseCapacity; /**< HTTP 接收容量，2–AI_CHAT_MAX_JSON_BYTES+1 字节。 */
    char *ResponseBuffer; /**< 可选宿主静态接收缓冲区，至少 ResponseCapacity 字节，借用。 */
    char *RequestBuffer; /**< 可选宿主静态请求序列化缓冲区，借用；NULL 时分配。 */
    size_t RequestCapacity; /**< RequestBuffer 的容量，包含 NUL；不足时拒绝发送。 */
} AI_CHAT_InitTypeDef;
/**
 * @brief 设置核心与 cJSON 共用的分配器；NULL/NULL 恢复 libc 默认。
 * @note 必须在所有引擎及 cJSON 对象创建前设置，全部释放后才允许更换。
 * 分配函数须返回适当对齐的内存，free(NULL) 必须安全；单线程初始化，不提供并发保护。
 */
AI_CHAT_StatusTypeDef AI_CHAT_SetAllocator(void *(*allocate)(size_t), void (*release)(void *));
/**
 * @brief 宿主白名单回调，在 Send/Invoke 调用线程执行。
 * @param arguments 借用、只读、已校验的 JSON 对象，不得释放或保留。
 * @param result 成功时写入新建的 cJSON 值；其所有权转交框架。
 * @param user_data 注册时传入的宿主指针，生命周期须覆盖引擎。
 * @return AI_CHAT_OK 表示成功；失败结果不自动重试。返回数据上限 AI_CHAT_MAX_TOOL_RESULT_BYTES。
 */
typedef AI_CHAT_StatusTypeDef (*AI_CHAT_ToolCallback)(const cJSON *arguments, cJSON **result, void *user_data);
/** @brief 返回可修改的默认配置；Init 前填写 Model 和凭据。 */
AI_CHAT_InitTypeDef AI_CHAT_DefaultConfig(void);
/**
 * @brief 创建引擎，深复制配置。失败时 *handle 为 NULL。
 * @param net 已初始化的 HAL 网络句柄；借用，不由引擎关闭，须覆盖引擎生命周期。
 * @return 创建成功为 AI_CHAT_OK，参数无效或分配失败返回错误码。
 */
AI_CHAT_StatusTypeDef AI_CHAT_Init(AI_CHAT_HandleTypeDef **handle, const AI_CHAT_InitTypeDef *config, HAL_NET_HandleTypeDef *net);
/** @brief 释放引擎及其持有的 JSON/字符串；允许 NULL。不会关闭宿主网络句柄。 */
void AI_CHAT_DeInit(AI_CHAT_HandleTypeDef *handle);
/** @brief 返回借用的错误说明，有效期至下一次操作或 DeInit；不得释放。 */
const char *AI_CHAT_GetError(const AI_CHAT_HandleTypeDef *handle);
/** @brief 设置工具总开关，默认关闭；同时限制模型及手动入口。 */
void AI_CHAT_SetAllowTools(AI_CHAT_HandleTypeDef *handle, int allow);
/** @brief 深复制新凭据并清除旧密钥内存，不改变历史；拒绝 CR/LF。 */
AI_CHAT_StatusTypeDef AI_CHAT_SetAPIKey(AI_CHAT_HandleTypeDef *handle, const char *key);
/** @brief 返回工具定义数组副本，无回调/凭据；调用者 cJSON_Delete。 */
cJSON *AI_CHAT_ToolDefinitions(AI_CHAT_HandleTypeDef *handle);
/**
 * @brief 注册唯一工具，复制名称、说明及 schema，借用回调 UserData。
 * @note 支持 schema 的 type/properties/required/additionalProperties/items/enum/
 * minimum/maximum/minItems/maxItems/minLength/maxLength；未知校验关键字拒绝。
 */
AI_CHAT_StatusTypeDef AI_CHAT_RegisterTool(AI_CHAT_HandleTypeDef *handle, const char *name, const char *description, const cJSON *schema, AI_CHAT_ToolCallback callback, void *user_data);
/** @brief 注销宿主注册的工具；释放定义但不释放借用的回调 UserData。 */
AI_CHAT_StatusTypeDef AI_CHAT_UnregisterTool(AI_CHAT_HandleTypeDef *handle, const char *name);
/** @brief 调用已注册工具；成功 *result 由调用者 cJSON_Delete，失败置 NULL。 */
AI_CHAT_StatusTypeDef AI_CHAT_Invoke(AI_CHAT_HandleTypeDef *handle, const char *name, const cJSON *arguments, cJSON **result);
/**
 * @brief 加载宿主读取的 SKILL.md 文本（最多 64 KiB，须含 frontmatter），不执行脚本。
 * @note 文件读取由宿主负责，因此核心可在无文件系统 MCU 上使用。
 */
AI_CHAT_StatusTypeDef AI_CHAT_LoadSkill(AI_CHAT_HandleTypeDef *handle, const char *name, const char *text);
/** @brief 激活预加载 Skill；空名称解除激活，下轮请求生效。 */
AI_CHAT_StatusTypeDef AI_CHAT_ActivateSkill(AI_CHAT_HandleTypeDef *handle, const char *name);
/**
 * @brief 同步聊天与有界工具循环，失败不重发请求，保留已执行工具的审计。
 * @param answer 成功时返回 Markdown 原文；调用者使用 AI_CHAT_Free 释放。
 * @note 自动摘要较早历史并保留最近四条消息；压缩失败保留原文。回调中不得重入引擎。
 */
AI_CHAT_StatusTypeDef AI_CHAT_Send(AI_CHAT_HandleTypeDef *handle, const char *text, char **answer);
/** @brief 导出版本 1 归档副本；调用者 cJSON_Delete，不包含密钥及可执行回调。 */
cJSON *AI_CHAT_ExportHistory(const AI_CHAT_HandleTypeDef *handle);
/** @brief 完整校验后原子替换历史；不恢复连接或重放工具调用。 */
AI_CHAT_StatusTypeDef AI_CHAT_ImportHistory(AI_CHAT_HandleTypeDef *handle, const cJSON *archive);
/** @brief 清空消息、摘要、审计，保留模型配置与已注册工具。 */
AI_CHAT_StatusTypeDef AI_CHAT_Clear(AI_CHAT_HandleTypeDef *handle);
/** @brief 释放 Send 返回的字符串；允许 NULL。 */
void AI_CHAT_Free(void *memory);
#ifdef __cplusplus
}
#endif
#endif
