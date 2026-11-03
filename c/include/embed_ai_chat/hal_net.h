#ifndef EMBED_AI_HAL_NET_H
#define EMBED_AI_HAL_NET_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief 网络状态码；增加 NET 前缀，避免与 STM32 HAL_StatusTypeDef 冲突。 */
typedef enum { HAL_NET_OK = 0, HAL_NET_ERROR, HAL_NET_BUSY, HAL_NET_TIMEOUT } HAL_NET_StatusTypeDef;
/** @brief 句柄状态。BUSY 仅防止同步重入，不提供跨线程锁。 */
typedef enum { HAL_NET_STATE_RESET = 0, HAL_NET_STATE_READY, HAL_NET_STATE_BUSY } HAL_NET_StateTypeDef;
/** @brief 同步 HTTP POST 参数；全部指针借用至 Post 返回，驱动不得保留。 */
typedef struct {
    const char *URL; /**< 完整 URL；驱动负责 DNS/TCP/TLS/HTTP，不得静默降级 TLS。 */
    const char *const *Headers; /**< 完整 HTTP 头字符串，含敏感认证信息，不得记录。 */
    size_t HeaderCount; /**< 请求头数量。 */
    const char *Body; /**< 请求正文；发送 BodySize 字节。 */
    size_t BodySize; /**< 正文长度，不含结尾 NUL。 */
    uint32_t Timeout; /**< 整个请求的超时，毫秒，非零。 */
} HAL_NET_RequestTypeDef;
/** @brief 调用者提供响应缓冲区；驱动写入 Size/StatusCode，须保留一个 NUL 位置。 */
typedef struct {
    char *Data; /**< 借用的可写缓冲区；返回后仍归调用者所有。 */
    size_t Capacity; /**< 总容量，包含结尾 NUL，至少 2 字节。 */
    size_t Size; /**< 实际正文长度，必须小于 Capacity。 */
    long StatusCode; /**< HTTP 状态码，非 2xx 由公共入口转为 ERROR。 */
} HAL_NET_ResponseTypeDef;
struct HAL_NET_HandleTypeDef;
/** @brief 网络驱动表，须覆盖句柄生命周期；Init/DeInit 可为 NULL，Post 必填。 */
typedef struct {
    /** @brief 初始化驱动私有资源；失败须自行清理部分资源。 */
    HAL_NET_StatusTypeDef (*Init)(struct HAL_NET_HandleTypeDef *);
    /** @brief 释放此句柄的驱动资源；不得关闭宿主其他连接。 */
    HAL_NET_StatusTypeDef (*DeInit)(struct HAL_NET_HandleTypeDef *);
    /** @brief 同步传输；溢出/超时立即失败，不自动重发、不打印认证头。 */
    HAL_NET_StatusTypeDef (*Post)(struct HAL_NET_HandleTypeDef *, const HAL_NET_RequestTypeDef *, HAL_NET_ResponseTypeDef *);
} HAL_NET_DriverTypeDef;
/** @brief STM32 HAL 风格网络句柄；首次使用零初始化并设置 Driver/UserData。 */
typedef struct HAL_NET_HandleTypeDef {
    const HAL_NET_DriverTypeDef *Driver; /**< 驱动函数表，借用。 */
    void *UserData; /**< 驱动/宿主上下文指针，借用。 */
    HAL_NET_StateTypeDef State; /**< 由公共 HAL 入口维护，不由驱动修改。 */
    HAL_NET_StatusTypeDef ErrorCode; /**< 最近一次传输或初始化状态。 */
} HAL_NET_HandleTypeDef;
/** @brief RESET -> READY；已 READY 时幂等成功，BUSY 时拒绝。句柄须先零初始化。 */
HAL_NET_StatusTypeDef HAL_NET_Init(HAL_NET_HandleTypeDef *hnet);
/** @brief 释放驱动资源并回到 RESET；BUSY 时不调用驱动。 */
HAL_NET_StatusTypeDef HAL_NET_DeInit(HAL_NET_HandleTypeDef *hnet);
/**
 * @brief 同步 POST；READY -> BUSY -> READY，任何驱动结果都恢复 READY。
 * @return HAL_NET_OK 仅表示传输成功且 HTTP 为 2xx，超时返回 HAL_NET_TIMEOUT。
 * @note 串行使用，不重试。驱动须严格遵守缓冲区容量，公共入口无法修复越界写。
 */
HAL_NET_StatusTypeDef HAL_NET_Post(HAL_NET_HandleTypeDef *hnet, const HAL_NET_RequestTypeDef *request, HAL_NET_ResponseTypeDef *response);
/** @brief 读取当前句柄状态；NULL 返回 RESET。 */
HAL_NET_StateTypeDef HAL_NET_GetState(const HAL_NET_HandleTypeDef *hnet);
/** @brief 可选桌面 libcurl 驱动；宿主在整个应用生命周期调用 curl_global_init/cleanup。 */
extern const HAL_NET_DriverTypeDef HAL_NET_CURL_Driver;
#ifdef __cplusplus
}
#endif
#endif
