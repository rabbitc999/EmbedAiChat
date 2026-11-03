#include "embed_ai_chat/hal_net.h"
/* 初始化/释放由公共入口维护状态，底层驱动只负责自己的资源。 */
HAL_NET_StatusTypeDef HAL_NET_Init(HAL_NET_HandleTypeDef *h) {
    HAL_NET_StatusTypeDef s;
    if (!h || !h->Driver || !h->Driver->Post) { return HAL_NET_ERROR; }
    if (h->State == HAL_NET_STATE_BUSY) { return HAL_NET_BUSY; }
    if (h->State == HAL_NET_STATE_READY) { return HAL_NET_OK; }
    s = h->Driver->Init ? h->Driver->Init(h) : HAL_NET_OK;
    h->ErrorCode = s;
    h->State = s == HAL_NET_OK ? HAL_NET_STATE_READY : HAL_NET_STATE_RESET;
    return s;
}
HAL_NET_StatusTypeDef HAL_NET_DeInit(HAL_NET_HandleTypeDef *h) {
    HAL_NET_StatusTypeDef s;
    if (!h || !h->Driver) { return HAL_NET_ERROR; }
    if (h->State == HAL_NET_STATE_BUSY) { return HAL_NET_BUSY; }
    if (h->State == HAL_NET_STATE_RESET) { return HAL_NET_OK; }
    s = h->Driver->DeInit ? h->Driver->DeInit(h) : HAL_NET_OK;
    h->ErrorCode = s;
    if (s == HAL_NET_OK) { h->State = HAL_NET_STATE_RESET; }
    return s;
}
HAL_NET_StatusTypeDef HAL_NET_Post(HAL_NET_HandleTypeDef *h, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    HAL_NET_StatusTypeDef s;
    if (!h || !q || !r || !q->URL || !q->Body || !q->Timeout ||
        (q->HeaderCount && !q->Headers) || !r->Data || r->Capacity < 2) { return HAL_NET_ERROR; }
    if (h->State == HAL_NET_STATE_BUSY) { return HAL_NET_BUSY; }
    if (h->State != HAL_NET_STATE_READY || !h->Driver || !h->Driver->Post) { return HAL_NET_ERROR; }
    /* 传输前锁定同步重入；失败和超时均恢复 READY，操作绝不自动重发。 */
    h->State = HAL_NET_STATE_BUSY;
    r->Size = 0; r->StatusCode = 0; r->Data[0] = '\0';
    s = h->Driver->Post(h, q, r);
    /* 统一补终止符、检查驱动返回长度及 HTTP 状态。驱动本身必须防止越界写。 */
    if (r->Size >= r->Capacity) { r->Size = 0; s = HAL_NET_ERROR; }
    r->Data[r->Size] = '\0';
    if (s == HAL_NET_OK && (r->StatusCode < 200 || r->StatusCode >= 300)) { s = HAL_NET_ERROR; }
    h->ErrorCode = s; h->State = HAL_NET_STATE_READY;
    return s;
}
HAL_NET_StateTypeDef HAL_NET_GetState(const HAL_NET_HandleTypeDef *h) {
    return h ? h->State : HAL_NET_STATE_RESET;
}
