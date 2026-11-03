#include "hal_net_stm32.h"
/* CubeMX 的网络初始化由应用完成；适配层不绑定具体 HAL、模组或 RTOS 版本。 */
static HAL_NET_StatusTypeDef post(HAL_NET_HandleTypeDef *h, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    return BSP_AI_HTTP_Post(h->UserData,q,r);
}
const HAL_NET_DriverTypeDef HAL_NET_STM32_Driver={NULL,NULL,post};
