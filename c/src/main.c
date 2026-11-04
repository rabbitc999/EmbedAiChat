#include "embed_ai_chat/ai_chat.h"
#include "embed_ai_chat/mcp_stdio.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
typedef struct { char title[1024]; double value; } DemoHost;
typedef struct McpSession { AI_CHAT_MCP *client; struct McpSession *next; } McpSession;
/* 会话属于桌面宿主，活到引擎释放之后；核心仅借用工具回调。 */
static int attach_mcp(AI_CHAT_HandleTypeDef *h, const char *prefix, const cJSON *argv, unsigned timeout, McpSession **sessions) {
    McpSession *s=(McpSession *)calloc(1,sizeof(*s));
    if (!s) { return 0; }
#if AI_CHAT_DESKTOP_MCP
    if (AI_CHAT_MCP_Attach(h,prefix,argv,timeout,&s->client)!=AI_CHAT_OK) { free(s); return 0; }
#else
    (void)h; (void)prefix; (void)argv; (void)timeout; (void)sessions; free(s); return 0;
#endif
    s->next=*sessions; *sessions=s; return 1;
}
/* 演示宿主仅修改当前进程的真实状态，返回修改后的回读值。 */
static AI_CHAT_StatusTypeDef set_value(const cJSON *args, cJSON **result, void *user) {
    DemoHost *host=(DemoHost *)user;
    host->value=cJSON_GetObjectItemCaseSensitive(args,"value")->valuedouble;
    *result=cJSON_CreateObject(); cJSON_AddNumberToObject(*result,"value",host->value); return AI_CHAT_OK;
}
static AI_CHAT_StatusTypeDef set_title(const cJSON *args, cJSON **result, void *user) {
    DemoHost *host=(DemoHost *)user;
    snprintf(host->title,sizeof(host->title),"%s",cJSON_GetObjectItemCaseSensitive(args,"title")->valuestring);
    *result=cJSON_CreateObject(); cJSON_AddStringToObject(*result,"title",host->title); return AI_CHAT_OK;
}
/* 文件访问属于桌面入口，不进入可移植核心。读取总量有界。 */
static char *read_file(const char *path) {
    FILE *f=fopen(path,"rb"); long n; char *s;
    if (!f) { return NULL; }
    if (fseek(f,0,SEEK_END) || (n=ftell(f))<0 || n>4*1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return NULL; }
    s=(char *)malloc((size_t)n+1);
    if (!s) { fclose(f); return NULL; }
    if (fread(s,1,(size_t)n,f)!=(size_t)n) { free(s); fclose(f); return NULL; }
    s[n]=0; fclose(f); return s;
}
static const char *field(const cJSON *o, const char *key, const char *fallback) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v)?v->valuestring:fallback;
}
static void print_json(cJSON *j) {
    char *s=cJSON_PrintUnformatted(j);
    if (s) { puts(s); free(s); }
    cJSON_Delete(j);
}
/* 消息事件立即显示，最终返回字符串只负责兼容原有宿主接口。 */
static AI_CHAT_StatusTypeDef display_event(const cJSON *event, void *user) {
    (void)user;
    if (!strcmp(field(event,"type",""),"assistant")) { puts(field(event,"text","")); fflush(stdout); }
    return AI_CHAT_OK;
}
static void print_host(const DemoHost *host) {
    cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"title",host->title); cJSON_AddNumberToObject(o,"value",host->value); print_json(o);
}
static int load_skill(AI_CHAT_HandleTypeDef *h, const char *directory) {
    char path[4096], name[65]; size_t n=strlen(directory), begin; char *text;
    while (n && (directory[n-1]=='/' || directory[n-1]=='\\')) { --n; }
    begin=n;
    while (begin && directory[begin-1]!='/' && directory[begin-1]!='\\') { --begin; }
    if (!n || n-begin>64 || n+10>=sizeof(path)) { return 0; }
    memcpy(name,directory+begin,n-begin); name[n-begin]=0;
    memcpy(path,directory,n); strcpy(path+n,"/SKILL.md"); text=read_file(path);
    if (!text) { return 0; }
    if (AI_CHAT_LoadSkill(h,name,text)!=AI_CHAT_OK) { free(text); return 0; }
    free(text); return 1;
}
/* 交互密钥从终端隐藏读取；重定向 stdin 用于本地自动化，不回显。 */
static int read_key(char *key, size_t capacity) {
#ifdef _WIN32
    if (_isatty(_fileno(stdin))) {
        size_t n=0; int ch;
        while ((ch=_getch())!='\r' && ch!='\n') {
            if (ch==3) { return 0; }
            if (ch==8) { if (n) { --n; } }
            else if (ch==0 || ch==224) { (void)_getch(); }
            else if (n+1<capacity) { key[n++]=(char)ch; }
        }
        key[n]=0; puts(""); return 1;
    }
#else
    struct termios old, hidden;
    int changed=0;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&old)==0) {
        hidden=old; hidden.c_lflag&=(tcflag_t)~ECHO; changed=tcsetattr(STDIN_FILENO,TCSANOW,&hidden)==0;
    }
    if (!fgets(key,(int)capacity,stdin)) {
        if (changed) { tcsetattr(STDIN_FILENO,TCSANOW,&old); }
        return 0;
    }
    if (changed) { tcsetattr(STDIN_FILENO,TCSANOW,&old); puts(""); }
    key[strcspn(key,"\r\n")]=0; return 1;
#endif
    if (!fgets(key,(int)capacity,stdin)) { return 0; }
    key[strcspn(key,"\r\n")]=0; return 1;
}
int main(int argc, char **argv) {
    AI_CHAT_InitTypeDef config=AI_CHAT_DefaultConfig();
    HAL_NET_HandleTypeDef net={&HAL_NET_CURL_Driver,NULL,HAL_NET_STATE_RESET,HAL_NET_OK};
    AI_CHAT_HandleTypeDef *h=NULL;
    McpSession *sessions=NULL;
    cJSON *settings=NULL, *schema, *item;
    char *settings_text=NULL, *ask=NULL, line[65538], key[4096];
    DemoHost host={"EmbedAiChat",0}; int i, exit_code=1;
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--config") && i+1<argc) {
            settings_text=read_file(argv[++i]);
            if (!settings_text || !(settings=cJSON_ParseWithOpts(settings_text,NULL,1)) || !cJSON_IsObject(settings)) { fprintf(stderr,"Invalid configuration\n"); goto done; }
        } else if (!strcmp(argv[i],"--ask") && i+1<argc) { ask=argv[++i]; }
        else { fprintf(stderr,"Usage: EmbedAiChatC [--config file] [--ask text]\n"); goto done; }
    }
    config.Provider=field(settings,"provider",config.Provider); config.BaseURL=field(settings,"base_url",config.BaseURL); config.Model=field(settings,"model",config.Model);
    item=cJSON_GetObjectItemCaseSensitive(settings,"compression_threshold");
    if (item) {
        if (!cJSON_IsNumber(item) || item->valuedouble<1 || item->valuedouble>100 || item->valuedouble!=(unsigned)item->valuedouble) { fprintf(stderr,"Invalid compression_threshold\n"); goto done; }
        config.CompressionThreshold=(unsigned)item->valuedouble;
    }
    item=cJSON_GetObjectItemCaseSensitive(settings,"max_summary_tokens");
    if (item) {
        if (!cJSON_IsNumber(item) || item->valuedouble<1 || item->valuedouble>1000000 || item->valuedouble!=(unsigned)item->valuedouble) { fprintf(stderr,"Invalid max_summary_tokens\n"); goto done; }
        config.MaxSummaryTokens=(unsigned)item->valuedouble;
    }
    item=cJSON_GetObjectItemCaseSensitive(settings,"max_tokens");
    if (item) {
        if (!cJSON_IsNumber(item) || item->valuedouble<1 || item->valuedouble>1000000 || item->valuedouble!=(unsigned)item->valuedouble) { fprintf(stderr,"Invalid max_tokens\n"); goto done; }
        config.MaxTokens=(unsigned)item->valuedouble;
    }
    item=cJSON_GetObjectItemCaseSensitive(settings,"auto_compact");
    if (item) {
        if (!cJSON_IsBool(item)) { fprintf(stderr,"Invalid auto_compact\n"); goto done; }
        config.AutoCompact=cJSON_IsTrue(item);
    }
    item=cJSON_GetObjectItemCaseSensitive(settings,"context_budget"); if (cJSON_IsNumber(item)) { config.ContextBudget=(size_t)item->valuedouble; }
    item=cJSON_GetObjectItemCaseSensitive(settings,"max_rounds");
    if (item) {
        if (!cJSON_IsNumber(item) || item->valuedouble<0 || item->valuedouble>4294967295.0 || item->valuedouble!=(unsigned)item->valuedouble) { fprintf(stderr,"Invalid max_rounds\n"); goto done; }
        config.MaxRounds=(unsigned)item->valuedouble;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK || HAL_NET_Init(&net)!=HAL_NET_OK || AI_CHAT_Init(&h,&config,&net)!=AI_CHAT_OK) { fprintf(stderr,"Initialization failed\n"); goto done; }
    AI_CHAT_SetAllowTools(h,cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings,"allow_tools")));
#if AI_CHAT_ENABLE_TOOLS
    schema=cJSON_Parse("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\",\"minimum\":0,\"maximum\":100}},\"required\":[\"value\"],\"additionalProperties\":false}");
    if (AI_CHAT_RegisterTool(h,"host_set_value","Set host value from 0 to 100",schema,set_value,&host)!=AI_CHAT_OK) { cJSON_Delete(schema); goto done; } cJSON_Delete(schema);
    schema=cJSON_Parse("{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"maxLength\":255}},\"required\":[\"title\"],\"additionalProperties\":false}");
    if (AI_CHAT_RegisterTool(h,"host_set_title","Set host title",schema,set_title,&host)!=AI_CHAT_OK) { cJSON_Delete(schema); goto done; } cJSON_Delete(schema);
#endif
    cJSON_ArrayForEach(item,cJSON_GetObjectItemCaseSensitive(settings,"skills")) {
        if (!cJSON_IsString(item) || !load_skill(h,item->valuestring)) { fprintf(stderr,"Skill load failed\n"); goto done; }
    }
    item=cJSON_GetObjectItemCaseSensitive(settings,"mcp");
    if (item && !cJSON_IsArray(item)) { fprintf(stderr,"Invalid MCP configuration\n"); goto done; }
    cJSON_ArrayForEach(item,cJSON_GetObjectItemCaseSensitive(settings,"mcp")) {
        const cJSON *timeout=cJSON_GetObjectItemCaseSensitive(item,"timeout_ms");
        unsigned value=15000;
        if (timeout) {
            if (!cJSON_IsNumber(timeout) || timeout->valuedouble<1 || timeout->valuedouble>60000 || timeout->valuedouble!=(unsigned)timeout->valuedouble) { fprintf(stderr,"Invalid MCP timeout\n"); goto done; }
            value=(unsigned)timeout->valuedouble;
        }
        if (!attach_mcp(h,field(item,"prefix",""),cJSON_GetObjectItemCaseSensitive(item,"command"),value,&sessions)) { fprintf(stderr,"MCP connection/discovery failed\n"); goto done; }
    }
    if (ask) {
        char *answer=NULL; cJSON *out;
        if (AI_CHAT_SendWithEvents(h,ask,&answer,display_event,NULL)!=AI_CHAT_OK) { fprintf(stderr,"%s\n",AI_CHAT_GetError(h)); goto done; }
        AI_CHAT_Free(answer);
        out=cJSON_CreateObject(); item=cJSON_AddObjectToObject(out,"host"); cJSON_AddStringToObject(item,"title",host.title); cJSON_AddNumberToObject(item,"value",host.value);
        cJSON_AddItemToObject(out,"history",AI_CHAT_ExportHistory(h)); print_json(out); exit_code=0; goto done;
    }
    puts("EmbedAiChat C (/help)");
    while (fgets(line,sizeof(line),stdin)) {
        AI_CHAT_StatusTypeDef status=AI_CHAT_OK;
        line[strcspn(line,"\r\n")]=0;
        if (!strcmp(line,"/exit")) { break; }
        else if (!strcmp(line,"/help")) { puts("/key /allow on|off /call name JSON /mcp prefix JSON-argv /tools /host /load-skill dir /skill name /audit /save file /load file /clear /exit"); }
        else if (!strcmp(line,"/key")) {
            if (!read_key(key,sizeof(key))) { break; }
            status=AI_CHAT_SetAPIKey(h,key); memset(key,0,sizeof(key));
        } else if (!strcmp(line,"/allow on")) { AI_CHAT_SetAllowTools(h,1); }
        else if (!strcmp(line,"/allow off")) { AI_CHAT_SetAllowTools(h,0); }
        else if (!strcmp(line,"/host")) { print_host(&host); }
        else if (!strcmp(line,"/clear")) { status=AI_CHAT_Clear(h); }
        else if (!strncmp(line,"/load-skill ",12)) { if (!load_skill(h,line+12)) { fprintf(stderr,"Skill load failed\n"); } }
        else if (!strncmp(line,"/skill ",7)) { status=AI_CHAT_ActivateSkill(h,line+7); }
        else if (!strncmp(line,"/call ",6)) {
            char *args=strchr(line+6,' '); cJSON *a, *r=NULL;
            if (!args) { fprintf(stderr,"Expected name and JSON object\n"); continue; }
            *args++=0; a=cJSON_ParseWithOpts(args,NULL,1); status=AI_CHAT_Invoke(h,line+6,a,&r); cJSON_Delete(a);
            if (status==AI_CHAT_OK) { print_json(r); }
        } else if (!strncmp(line,"/mcp ",5)) {
            char *args=strchr(line+5,' '); cJSON *argv;
            if (!args) { fprintf(stderr,"Expected prefix and JSON argv\n"); continue; }
            *args++=0; argv=cJSON_ParseWithOpts(args,NULL,1);
            if (!attach_mcp(h,line+5,argv,15000,&sessions)) { fprintf(stderr,"MCP connection/discovery failed\n"); }
            cJSON_Delete(argv);
        } else if (!strcmp(line,"/audit")) {
            cJSON *a=AI_CHAT_ExportHistory(h); print_json(cJSON_DetachItemFromObjectCaseSensitive(a,"audit")); cJSON_Delete(a);
        } else if (!strcmp(line,"/tools")) { print_json(AI_CHAT_ToolDefinitions(h)); }
        else if (!strncmp(line,"/save ",6)) {
            cJSON *a=AI_CHAT_ExportHistory(h); char *s=cJSON_PrintUnformatted(a); FILE *f=fopen(line+6,"wb");
            if (!f || !s) { fprintf(stderr,"Save failed\n"); }
            else if (fwrite(s,1,strlen(s),f)!=strlen(s)) { fprintf(stderr,"Save failed\n"); }
            if (f) { fclose(f); } free(s); cJSON_Delete(a);
        } else if (!strncmp(line,"/load ",6)) {
            char *s=read_file(line+6); cJSON *a=s?cJSON_ParseWithOpts(s,NULL,1):NULL;
            status=AI_CHAT_ImportHistory(h,a); cJSON_Delete(a); free(s);
        } else if (line[0]=='/') { fprintf(stderr,"Unsupported command\n"); }
        else if (*line) {
            char *answer=NULL; status=AI_CHAT_SendWithEvents(h,line,&answer,display_event,NULL);
            if (status==AI_CHAT_OK) { AI_CHAT_Free(answer); }
        }
        if (status!=AI_CHAT_OK) { fprintf(stderr,"%s\n",AI_CHAT_GetError(h)); }
    }
    exit_code=0;
done:
    AI_CHAT_DeInit(h);
    while (sessions) { McpSession *next=sessions->next;
#if AI_CHAT_DESKTOP_MCP
        AI_CHAT_MCP_Close(sessions->client);
#endif
        free(sessions); sessions=next; }
    HAL_NET_DeInit(&net); curl_global_cleanup(); cJSON_Delete(settings); free(settings_text); return exit_code;
}
