#include "embed_ai_chat/hal_net.h"
#include <curl/curl.h>
#include <string.h>
/* 按块写入调用者缓冲区；空间不足直接中止传输，不截断后冒充成功。 */
static size_t receive_data(char *data, size_t size, size_t count, void *user) {
    HAL_NET_ResponseTypeDef *r = (HAL_NET_ResponseTypeDef *)user;
    size_t n;
    if (size && count > (size_t)-1 / size) { return 0; }
    n = size * count;
    if (n >= r->Capacity - r->Size) { return 0; }
    memcpy(r->Data + r->Size, data, n); r->Size += n;
    return n;
}
static HAL_NET_StatusTypeDef post(HAL_NET_HandleTypeDef *h, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    CURLcode code = CURLE_FAILED_INIT;
    size_t i;
    (void)h;
    if (!curl) { return HAL_NET_ERROR; }
    /* 只接受 HTTP/HTTPS；认证头和正文均禁止输出到诊断日志。 */
    if (strncmp(q->URL,"https://",8) && strncmp(q->URL,"http://127.0.0.1:",17) &&
        strncmp(q->URL,"http://localhost:",17) && strncmp(q->URL,"http://[::1]:",13)) { goto done; }
    for (i = 0; i < q->HeaderCount; ++i) {
        struct curl_slist *next = curl_slist_append(headers, q->Headers[i]);
        if (!next) { goto done; }
        headers = next;
    }
    curl_easy_setopt(curl, CURLOPT_URL, q->URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, q->Body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)q->BodySize);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)q->Timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    /* Windows/OpenSSL 使用系统信任库，保持证书与主机名校验。 */
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    /* 保留 libcurl 的 TLS 证书校验；不跟随重定向，避免向其他地址转发凭据。 */
    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->StatusCode);
done:
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return code == CURLE_OK ? HAL_NET_OK : code == CURLE_OPERATION_TIMEDOUT ? HAL_NET_TIMEOUT : HAL_NET_ERROR;
}
const HAL_NET_DriverTypeDef HAL_NET_CURL_Driver = { NULL, NULL, post };
