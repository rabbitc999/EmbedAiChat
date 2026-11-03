#ifndef EMBED_AI_MCU_EXAMPLE_H
#define EMBED_AI_MCU_EXAMPLE_H
#include "embed_ai_chat/ai_chat.h"
/* 单实例示例。net 必须先初始化且在整个会话保持存活；key 由应用安全提供。 */
AI_CHAT_StatusTypeDef MCU_Chat_Init(HAL_NET_HandleTypeDef *net, const char *key);
AI_CHAT_StatusTypeDef MCU_Chat_Send(const char *prompt, char **answer);
AI_CHAT_HandleTypeDef *MCU_Chat_Handle(void);
void MCU_Chat_DeInit(void);
#endif
