#include "hal_net_esp_idf.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <limits.h>
#include <string.h>
typedef struct { HAL_NET_ResponseTypeDef *response; int overflow; } ReceiveContext;
static esp_err_t on_event(esp_http_client_event_t *event) {
    ReceiveContext *context=(ReceiveContext *)event->user_data;
    if (event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        HAL_NET_ResponseTypeDef *r=context->response;
        size_t count=(size_t)event->data_len;
        if (context->overflow || count>=r->Capacity-r->Size) { context->overflow=1; return ESP_FAIL; }
        memcpy(r->Data+r->Size,event->data,count); r->Size+=count;
    }
    return ESP_OK;
}
static HAL_NET_StatusTypeDef post(HAL_NET_HandleTypeDef *h, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    esp_http_client_config_t config={0}; esp_http_client_handle_t client;
    ReceiveContext context={r,0}; esp_err_t error=ESP_FAIL; size_t i;
    (void)h;
    if (strncmp(q->URL,"https://",8) || q->BodySize>INT_MAX || q->Timeout>INT_MAX) { return HAL_NET_ERROR; }
    config.url=q->URL; config.method=HTTP_METHOD_POST; config.timeout_ms=(int)q->Timeout;
    config.event_handler=on_event; config.user_data=&context;
    config.crt_bundle_attach=esp_crt_bundle_attach; config.disable_auto_redirect=true;
    config.buffer_size=512; config.buffer_size_tx=512;
    client=esp_http_client_init(&config); if (!client) { return HAL_NET_ERROR; }
    for (i=0;i<q->HeaderCount;++i) {
        const char *colon=strchr(q->Headers[i],':'), *value; char name[64]; size_t length;
        if (!colon || strpbrk(q->Headers[i],"\r\n")) { goto done; }
        length=(size_t)(colon-q->Headers[i]); if (!length || length>=sizeof(name)) { goto done; }
        memcpy(name,q->Headers[i],length); name[length]=0;
        value=colon+1; while (*value==' ' || *value=='\t') { ++value; }
        if (esp_http_client_set_header(client,name,value)!=ESP_OK) { goto done; }
    }
    if (esp_http_client_set_post_field(client,q->Body,(int)q->BodySize)!=ESP_OK) { goto done; }
    error=esp_http_client_perform(client); r->StatusCode=esp_http_client_get_status_code(client);
done:
    esp_http_client_cleanup(client);
    if (context.overflow) { return HAL_NET_ERROR; }
    return error==ESP_OK?HAL_NET_OK:error==ESP_ERR_TIMEOUT?HAL_NET_TIMEOUT:HAL_NET_ERROR;
}
const HAL_NET_DriverTypeDef HAL_NET_ESP_IDF_Driver={NULL,NULL,post};
