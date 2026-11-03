#ifndef EMBED_AI_HAL_NET_STM32_H
#define EMBED_AI_HAL_NET_STM32_H
#include "embed_ai_chat/hal_net.h"
/* 应用实现此 BSP 函数：可接模组 HTTP(S)，或 LwIP + TLS。
 * 必须校验证书/主机名，处理整个请求的超时，限制写入 Capacity-1 字节，设置 Size/StatusCode。
 * 请求和响应指针仅在同步调用内借用；不得在返回后继续 DMA/异步访问。
 */
HAL_NET_StatusTypeDef BSP_AI_HTTP_Post(void *board_context, const HAL_NET_RequestTypeDef *request, HAL_NET_ResponseTypeDef *response);
extern const HAL_NET_DriverTypeDef HAL_NET_STM32_Driver;
#endif
