#include "mcu_chat.h"
/* 位于静态 RAM，避免占用任务栈；容量不足会明确报错，不截断请求。 */
static char request_buffer[8192];
static char response_buffer[4096];
static AI_CHAT_HandleTypeDef *chat;
AI_CHAT_StatusTypeDef MCU_Chat_Init(HAL_NET_HandleTypeDef *net, const char *key) {
    AI_CHAT_InitTypeDef config=AI_CHAT_DefaultConfig();
    if (chat || !net || !key) { return AI_CHAT_ERROR; }
    config.Provider="openai";
    config.BaseURL="https://api.deepseek.com/v1";
    config.Model="deepseek-chat";
    config.APIKey=key;
    config.RequestBuffer=request_buffer; config.RequestCapacity=sizeof(request_buffer);
    config.ResponseBuffer=response_buffer; config.ResponseCapacity=sizeof(response_buffer);
    config.MaxTokens=256;
    return AI_CHAT_Init(&chat,&config,net);
}
AI_CHAT_StatusTypeDef MCU_Chat_Send(const char *prompt, char **answer) {
    /* 成功后的 answer 由应用调用 AI_CHAT_Free；UI/串口显示也由应用负责。 */
    return AI_CHAT_Send(chat,prompt,answer);
}
AI_CHAT_HandleTypeDef *MCU_Chat_Handle(void) { return chat; }
void MCU_Chat_DeInit(void) { AI_CHAT_DeInit(chat); chat=NULL; }
