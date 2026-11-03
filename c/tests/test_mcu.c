#include "embed_ai_chat/ai_chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
/* 统计整个核心与 cJSON 的分配，验证分配/释放配对及裁剪后的真实堆峰值。 */
typedef union { struct { size_t size; } value; double align_double; void *align_pointer; } Allocation;
static size_t live_bytes, peak_bytes, allocations;
static int fail_after=-1;
static int calls, edits, tool_mode, summary_calls;
#if AI_CHAT_ENABLE_COMPACTION
static char request_buffer[8192];
#else
static char request_buffer[4096];
#endif
static char response_buffer[2048];
static void *allocate(size_t size) {
    Allocation *p;
    if (fail_after==0) { return NULL; }
    if (fail_after>0) { --fail_after; }
    if (size>((size_t)-1)-sizeof(*p) || live_bytes+size>65536) { return NULL; }
    p=(Allocation *)malloc(sizeof(*p)+size); if (!p) { return NULL; }
    p->value.size=size; live_bytes+=size; if (live_bytes>peak_bytes) { peak_bytes=live_bytes; } ++allocations; return p+1;
}
static void release(void *pointer) {
    Allocation *p;
    if (!pointer) { return; } p=(Allocation *)pointer-1; CHECK(live_bytes>=p->value.size); live_bytes-=p->value.size; free(p);
}
static AI_CHAT_StatusTypeDef edit(const cJSON *args, cJSON **result, void *user) {
    (void)args; (void)user; ++edits; *result=cJSON_CreateObject(); cJSON_AddNumberToObject(*result,"value",42); return AI_CHAT_OK;
}
static HAL_NET_StatusTypeDef post(HAL_NET_HandleTypeDef *h, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    const char *reply="{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}";
    (void)h; ++calls;
    CHECK(q->Body==request_buffer); CHECK(r->Data==response_buffer);
    CHECK(strstr(q->Body,"\"max_tokens\""));
    if (strstr(q->Body,"Summarize this historical")) { ++summary_calls; }
    if (tool_mode && edits==0) {
        reply="{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"edit\",\"type\":\"function\",\"function\":{\"name\":\"edit\",\"arguments\":\"{}\"}}]}}]}";
    } else if (tool_mode) {
        CHECK(strstr(q->Body,"\"role\":\"tool\"")); CHECK(strstr(q->Body,"\"tool_call_id\":\"edit\""));
        CHECK(strstr(q->Body,"value"));
    }
    if (strlen(reply)>=r->Capacity) { return HAL_NET_ERROR; }
    memcpy(r->Data,reply,strlen(reply)); r->Size=strlen(reply); r->StatusCode=200; return HAL_NET_OK;
}
int main(void) {
    const HAL_NET_DriverTypeDef driver={NULL,NULL,post};
    HAL_NET_HandleTypeDef net={&driver,NULL,HAL_NET_STATE_RESET,HAL_NET_OK};
    AI_CHAT_HandleTypeDef *h=NULL; AI_CHAT_InitTypeDef c=AI_CHAT_DefaultConfig();
    cJSON *schema, *history; char *answer=NULL; int old_calls;
    CHECK(AI_CHAT_SetAllocator(allocate,release)==AI_CHAT_OK);
    CHECK(AI_CHAT_SetAllocator(allocate,NULL)==AI_CHAT_ERROR);
    CHECK(HAL_NET_Init(&net)==HAL_NET_OK);
    c.Model="fixture"; c.RequestBuffer=request_buffer; c.RequestCapacity=sizeof(request_buffer);
    c.ContextBudget=2000;
    c.ResponseBuffer=response_buffer; c.ResponseCapacity=sizeof(response_buffer);
    /* 低内存初始化：每个分配点失败都须清理已分配状态。 */
    {
        int i;
        for (i=0;i<16;++i) {
            AI_CHAT_StatusTypeDef status;
            fail_after=i; status=AI_CHAT_Init(&h,&c,&net);
            if (status==AI_CHAT_OK) { AI_CHAT_DeInit(h); h=NULL; }
            else { CHECK(!h); }
            CHECK(live_bytes==0);
        }
        fail_after=-1;
    }
    CHECK(AI_CHAT_Init(&h,&c,&net)==AI_CHAT_OK);
    schema=cJSON_Parse("{\"type\":\"object\",\"additionalProperties\":false}");
#if AI_CHAT_ENABLE_TOOLS
    CHECK(AI_CHAT_RegisterTool(h,"edit","edit actual state",schema,edit,NULL)==AI_CHAT_OK);
    AI_CHAT_SetAllowTools(h,1); tool_mode=1;
#else
    CHECK(AI_CHAT_RegisterTool(h,"edit","edit",schema,edit,NULL)==AI_CHAT_ERROR);
#endif
    cJSON_Delete(schema);
    CHECK(AI_CHAT_Send(h,"first message",&answer)==AI_CHAT_OK); CHECK(!strcmp(answer,"done")); AI_CHAT_Free(answer);
#if AI_CHAT_ENABLE_TOOLS
    CHECK(edits==1 && calls==2);
#endif
    tool_mode=0;
    CHECK(AI_CHAT_Send(h,"second message",&answer)==AI_CHAT_OK); AI_CHAT_Free(answer);
#if AI_CHAT_ENABLE_HISTORY
    CHECK(strstr(request_buffer,"first message"));
#else
    CHECK(!strstr(request_buffer,"first message"));
#endif
    history=AI_CHAT_ExportHistory(h);
#if AI_CHAT_ENABLE_ARCHIVE
    CHECK(history); CHECK(AI_CHAT_ImportHistory(h,history)==AI_CHAT_OK);
#else
    CHECK(!history); CHECK(AI_CHAT_ImportHistory(h,NULL)==AI_CHAT_ERROR);
#endif
    cJSON_Delete(history);
#if AI_CHAT_ENABLE_COMPACTION
    {
        char long_message[801]; int i;
        memset(long_message,'x',sizeof(long_message)-1); long_message[sizeof(long_message)-1]=0;
        CHECK(AI_CHAT_Clear(h)==AI_CHAT_OK); AI_CHAT_SetAllowTools(h,0);
        for (i=0;i<16;++i) {
            AI_CHAT_StatusTypeDef status=AI_CHAT_Send(h,long_message,&answer);
            if (status!=AI_CHAT_OK) { fprintf(stderr,"summary turn %d: %s\n",i,AI_CHAT_GetError(h)); }
            CHECK(status==AI_CHAT_OK); AI_CHAT_Free(answer);
        }
        CHECK(summary_calls>0);
        CHECK(strstr(request_buffer,"Summary:\\ndone"));
    }
#endif
#if !AI_CHAT_ENABLE_SKILLS
    CHECK(AI_CHAT_LoadSkill(h,"demo","---\nname: demo\n---\ntext")==AI_CHAT_ERROR);
#endif
    AI_CHAT_DeInit(h); h=NULL;
    /* 静态请求缓冲不足时，不发出截断请求。 */
    c.RequestCapacity=8; CHECK(AI_CHAT_Init(&h,&c,&net)==AI_CHAT_OK); old_calls=calls;
    CHECK(AI_CHAT_Send(h,"too small",&answer)!=AI_CHAT_OK); CHECK(calls==old_calls && !answer);
    AI_CHAT_DeInit(h); h=NULL; c.RequestCapacity=sizeof(request_buffer);
#if !AI_CHAT_ENABLE_ANTHROPIC
    c.Provider="anthropic"; CHECK(AI_CHAT_Init(&h,&c,&net)==AI_CHAT_ERROR && !h); c.Provider="openai";
#endif
#if !AI_CHAT_ENABLE_COMPACTION
    c.AutoCompact=1; CHECK(AI_CHAT_Init(&h,&c,&net)==AI_CHAT_ERROR && !h); c.AutoCompact=0;
#endif
    CHECK(HAL_NET_DeInit(&net)==HAL_NET_OK);
    CHECK(live_bytes==0 && allocations>0);
    CHECK(peak_bytes<32768u);
    CHECK(AI_CHAT_SetAllocator(NULL,NULL)==AI_CHAT_OK);
    printf("MCU profile passed: core+JSON peak=%lu bytes, static IO=%lu bytes, tools=%d history=%d compact=%d archive=%d\n",(unsigned long)peak_bytes,(unsigned long)(sizeof(request_buffer)+sizeof(response_buffer)),AI_CHAT_ENABLE_TOOLS,AI_CHAT_ENABLE_HISTORY,AI_CHAT_ENABLE_COMPACTION,AI_CHAT_ENABLE_ARCHIVE);
    return 0;
}
