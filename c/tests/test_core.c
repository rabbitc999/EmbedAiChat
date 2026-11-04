#include "embed_ai_chat/ai_chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
typedef struct { int requests, edits, mode, anthropic; HAL_NET_HandleTypeDef *net; } Fixture;
/* 模拟 HAL 检查真实请求内容和第二轮工具结果，不访问公网或收费模型。 */
static HAL_NET_StatusTypeDef mock_post(HAL_NET_HandleTypeDef *net, const HAL_NET_RequestTypeDef *q, HAL_NET_ResponseTypeDef *r) {
    Fixture *f=(Fixture *)net->UserData;
    cJSON *body=cJSON_Parse(q->Body);
    const char *response;
    CHECK(net->State==HAL_NET_STATE_BUSY);
    CHECK(HAL_NET_Post(net,q,r)==HAL_NET_BUSY);
    CHECK(cJSON_IsObject(body));
    CHECK(strstr(q->URL,f->anthropic?"/messages":"/chat/completions"));
    CHECK(q->Timeout==1000);
    if (f->mode==1 || (f->mode==2 && f->requests)) { cJSON_Delete(body); return HAL_NET_TIMEOUT; }
    if (!f->requests) {
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(body,"tools"))==5);
        if (f->anthropic) {
            CHECK(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(body,"system")));
            response="{\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"host_set_value\",\"input\":{\"value\":42}}]}";
        } else {
            response="{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"t1\",\"type\":\"function\",\"function\":{\"name\":\"host_set_value\",\"arguments\":\"{\\\"value\\\":42}\"}}]}}]}";
        }
    } else {
        CHECK(f->edits==1);
        CHECK(strstr(q->Body,"tool_result") || strstr(q->Body,"tool_call_id"));
        CHECK(strstr(q->Body,"42"));
        response=f->anthropic?"{\"content\":[{\"type\":\"text\",\"text\":\"**ok**\"}]}":"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"**ok**\"}}]}";
    }
    ++f->requests; r->StatusCode=200; r->Size=strlen(response);
    CHECK(r->Size<r->Capacity); memcpy(r->Data,response,r->Size);
    cJSON_Delete(body); return HAL_NET_OK;
}
static const HAL_NET_DriverTypeDef driver={NULL,NULL,mock_post};
static AI_CHAT_StatusTypeDef edit(const cJSON *args, cJSON **result, void *user) {
    Fixture *f=(Fixture *)user;
    CHECK(cJSON_GetObjectItemCaseSensitive(args,"value")->valuedouble==42);
    ++f->edits; *result=cJSON_CreateObject(); cJSON_AddNumberToObject(*result,"value",42); return AI_CHAT_OK;
}
/* 事件回调中断不得重发或丢失已执行审计；done 只在正常结束时发送。 */
static AI_CHAT_StatusTypeDef observe_event(const cJSON *event, void *user) {
    int *counts=(int *)user;
    const char *type=cJSON_GetObjectItemCaseSensitive(event,"type")->valuestring;
    if (!strcmp(type,"assistant")) { ++counts[0]; if (counts[3]) { return AI_CHAT_ERROR; } }
    else if (!strcmp(type,"tool")) { ++counts[1]; }
    else if (!strcmp(type,"done")) { ++counts[2]; }
    else { CHECK(0); }
    return AI_CHAT_OK;
}
static void test_engine(int anthropic, int mode) {
    Fixture f={0,0,mode,anthropic,NULL};
    HAL_NET_HandleTypeDef net={&driver,&f,HAL_NET_STATE_RESET,HAL_NET_OK};
    AI_CHAT_InitTypeDef config=AI_CHAT_DefaultConfig();
    AI_CHAT_HandleTypeDef *h=NULL;
    cJSON *schema, *args, *result=NULL, *archive, *before, *after;
    char *answer=NULL, *a, *b;
    int events[4]={0,0,0,mode==3};
    config.Provider=anthropic?"anthropic":"openai"; config.Model="fixture";
    config.APIKey="test-secret"; config.Timeout=1000;
    CHECK(HAL_NET_Init(&net)==HAL_NET_OK);
    CHECK(AI_CHAT_Init(&h,&config,&net)==AI_CHAT_OK);
    CHECK(AI_CHAT_SetAPIKey(h,"bad\r\ninjected")==AI_CHAT_ERROR);
    schema=cJSON_Parse("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\",\"minimum\":0,\"maximum\":100}},\"required\":[\"value\"],\"additionalProperties\":false}");
    CHECK(AI_CHAT_RegisterTool(h,"host_set_value","Edit value",schema,edit,&f)==AI_CHAT_OK);
    CHECK(AI_CHAT_RegisterTool(h,"host_set_value","Duplicate",schema,edit,&f)==AI_CHAT_ERROR);
    cJSON_Delete(schema);
    schema=cJSON_Parse("{\"type\":\"object\",\"pattern\":\"ignored?\"}");
    CHECK(AI_CHAT_RegisterTool(h,"bad","Unsupported constraint",schema,edit,&f)==AI_CHAT_ERROR); cJSON_Delete(schema);
    args=cJSON_Parse("{\"value\":42}");
    CHECK(AI_CHAT_Invoke(h,"host_set_value",args,&result)==AI_CHAT_ERROR); CHECK(!result && !f.edits); cJSON_Delete(args);
    AI_CHAT_SetAllowTools(h,1);
    args=cJSON_Parse("{\"value\":101}"); CHECK(AI_CHAT_Invoke(h,"host_set_value",args,&result)==AI_CHAT_ERROR); cJSON_Delete(args);
    args=cJSON_Parse("{\"value\":42,\"extra\":true}"); CHECK(AI_CHAT_Invoke(h,"host_set_value",args,&result)==AI_CHAT_ERROR); cJSON_Delete(args);
    CHECK(f.edits==0);
    CHECK(AI_CHAT_LoadSkill(h,"demo","---\nname: demo\n---\nHost instructions")==AI_CHAT_OK);
    CHECK(AI_CHAT_ActivateSkill(h,"demo")==AI_CHAT_OK);
    CHECK(AI_CHAT_ActivateSkill(h,"unknown")==AI_CHAT_ERROR);
    if (mode>=3) {
        CHECK(AI_CHAT_SendWithEvents(h,"hello",&answer,observe_event,events)==(mode==3?AI_CHAT_ERROR:AI_CHAT_OK));
        CHECK(events[0]==1 && events[1]==1 && events[2]==(mode==4));
        if (mode==3) { CHECK(!answer); } else { CHECK(!strcmp(answer,"**ok**")); AI_CHAT_Free(answer); }
    }
    else if (!mode) { CHECK(AI_CHAT_Send(h,"hello",&answer)==AI_CHAT_OK); CHECK(!strcmp(answer,"**ok**")); AI_CHAT_Free(answer); }
    else { CHECK(AI_CHAT_Send(h,"hello",&answer)==AI_CHAT_ERROR); CHECK(!answer); }
    CHECK(HAL_NET_GetState(&net)==HAL_NET_STATE_READY);
    archive=AI_CHAT_ExportHistory(h);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(archive,"audit"))==(mode==1?0:1));
    CHECK(f.edits==(mode==1?0:1));
    a=cJSON_PrintUnformatted(archive); CHECK(!strstr(a,"test-secret")); free(a);
    CHECK(AI_CHAT_ImportHistory(h,archive)==AI_CHAT_OK);
    before=AI_CHAT_ExportHistory(h);
    cJSON_ReplaceItemInObjectCaseSensitive(archive,"covered",cJSON_CreateNumber(9999));
    CHECK(AI_CHAT_ImportHistory(h,archive)==AI_CHAT_ERROR);
    after=AI_CHAT_ExportHistory(h); a=cJSON_PrintUnformatted(before); b=cJSON_PrintUnformatted(after);
    CHECK(!strcmp(a,b)); free(a); free(b); cJSON_Delete(before); cJSON_Delete(after); cJSON_Delete(archive);
    CHECK(AI_CHAT_Clear(h)==AI_CHAT_OK); AI_CHAT_DeInit(h); CHECK(HAL_NET_DeInit(&net)==HAL_NET_OK);
    CHECK(HAL_NET_GetState(&net)==HAL_NET_STATE_RESET);
}
int main(void) {
    test_engine(0,0); test_engine(1,0); test_engine(0,1); test_engine(1,1); test_engine(0,2); test_engine(1,2);
    test_engine(0,3); test_engine(1,3); test_engine(0,4); test_engine(1,4);
    puts("C core/HAL tests passed (OpenAI, Anthropic, timeout, audit, schema, history)"); return 0;
}
