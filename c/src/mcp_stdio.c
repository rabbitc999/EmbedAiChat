/* 可选桌面 MCP：原生 argv 进程、带期限的管道 JSON-RPC、分页发现与宿主回调。 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "embed_ai_chat/mcp_stdio.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* JSON 输出与会话对象共用宿主安装的 cJSON 分配器。 */
#define malloc cJSON_malloc
#define free cJSON_free
static void *mcp_calloc(size_t count, size_t size) {
    void *p;
    if (size && count>((size_t)-1)/size) { return NULL; }
    p=cJSON_malloc(count*size); if (p) { memset(p,0,count*size); } return p;
}
#define calloc mcp_calloc
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#define LIMIT (4u * 1024u * 1024u)
#define GET(o, k) cJSON_GetObjectItemCaseSensitive((o), (k))
typedef struct Binding {
    struct AI_CHAT_MCP *client;
    char *name, *original;
    struct Binding *next;
} Binding;
struct AI_CHAT_MCP {
    unsigned timeout, sequence;
    int alive;
    char *buffer;
    size_t used;
    Binding *bindings;
#ifdef _WIN32
    HANDLE input, output, process, job;
#else
    int input, output;
    pid_t pid;
#endif
};
static const char *text(const cJSON *v) {
    return cJSON_IsString(v) ? v->valuestring : "";
}
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}
static int valid_name(const char *s) {
    size_t n = 0;
    if (!s || !*s) {
        return 0;
    }
    for (; *s; ++s, ++n) {
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '_')) {
            return 0;
        }
    }
    return n <= 64;
}
static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}
static void stop(AI_CHAT_MCP *c) {
    c->alive = 0;
#ifdef _WIN32
    if (c->input) {
        CloseHandle(c->input);
        c->input = NULL;
    }
    if (c->output) {
        CloseHandle(c->output);
        c->output = NULL;
    }
    if (c->job) {
        CloseHandle(c->job);
        c->job = NULL;
    }
    if (c->process) {
        TerminateProcess(c->process, 1);
        WaitForSingleObject(c->process, 2000);
        CloseHandle(c->process);
        c->process = NULL;
    }
#else
    if (c->input >= 0) {
        close(c->input);
        c->input = -1;
    }
    if (c->output >= 0) {
        close(c->output);
        c->output = -1;
    }
    if (c->pid > 0) {
        kill(-c->pid, SIGKILL);
        kill(c->pid, SIGKILL);
        while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {
        }
        c->pid = 0;
    }
#endif
}
void AI_CHAT_MCP_Close(AI_CHAT_MCP *c) {
    Binding *b, *next;
    if (!c) {
        return;
    }
    stop(c);
    for (b = c->bindings; b; b = next) {
        next = b->next;
        free(b->name);
        free(b->original);
        free(b);
    }
    free(c->buffer);
    free(c);
}
#ifdef _WIN32
/* CreateProcessW 参数遵循 Windows argv 的引号/反斜杠规则，不交给 cmd.exe。 */
static int spawn(AI_CHAT_MCP *c, const cJSON *argv) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE child_in = NULL, child_out = NULL;
    wchar_t *cmd = (wchar_t *)calloc(32768, sizeof(wchar_t));
    const cJSON *v;
    size_t at = 0;
    int ok = 0;
    if (!cmd) {
        return 0;
    }
    cJSON_ArrayForEach(v, argv) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text(v), -1, NULL, 0), i;
        wchar_t *arg;
        size_t slashes = 0;
        if (count <= 0 || count > 16000) {
            goto done;
        }
        arg = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
        if (!arg) {
            goto done;
        }
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text(v), -1, arg, count);
        if (at + 2 * (size_t)count + 4 >= 32768) {
            free(arg);
            goto done;
        }
        if (at) {
            cmd[at++] = L' ';
        }
        cmd[at++] = L'"';
        for (i = 0; i < count - 1; ++i) {
            size_t n;
            if (arg[i] == L'\\') {
                ++slashes;
                continue;
            }
            n = slashes * (arg[i] == L'"' ? 2 : 1);
            while (n--) {
                cmd[at++] = L'\\';
            }
            slashes = 0;
            if (arg[i] == L'"') {
                cmd[at++] = L'\\';
            }
            cmd[at++] = arg[i];
        }
        slashes *= 2;
        while (slashes--) {
            cmd[at++] = L'\\';
        }
        cmd[at++] = L'"';
        free(arg);
    }
    if (!CreatePipe(&child_in, &c->input, &sa, 65536) ||
        !SetHandleInformation(c->input, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&c->output, &child_out, &sa, 65536) ||
        !SetHandleInformation(c->output, HANDLE_FLAG_INHERIT, 0)) {
        goto done;
    }
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    memset(&limits, 0, sizeof(limits));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in;
    si.hStdOutput = child_out;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    c->job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!c->job ||
        !SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        goto done;
    }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si,
                        &pi)) {
        goto done;
    }
    c->process = pi.hProcess;
    if (AssignProcessToJobObject(c->job, c->process) && ResumeThread(pi.hThread) != (DWORD)-1) {
        ok = 1;
    }
    CloseHandle(pi.hThread);
done:
    if (child_in) {
        CloseHandle(child_in);
    }
    if (child_out) {
        CloseHandle(child_out);
    }
    free(cmd);
    if (!ok) {
        stop(c);
    }
    return ok;
}
typedef struct {
    HANDLE pipe;
    const char *data;
    DWORD size;
    int ok;
} WriteTask;
static DWORD WINAPI writer(void *arg) {
    WriteTask *w = (WriteTask *)arg;
    DWORD offset = 0, n;
    while (offset < w->size) {
        if (!WriteFile(w->pipe, w->data + offset, w->size - offset, &n, NULL) || !n) {
            return 0;
        }
        offset += n;
    }
    w->ok = 1;
    return 0;
}
#else
static int spawn(AI_CHAT_MCP *c, const cJSON *argv) {
    int in[2] = {-1, -1}, out[2] = {-1, -1}, i, count = cJSON_GetArraySize(argv);
    char **args = (char **)calloc((size_t)count + 1, sizeof(char *));
    if (!args) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        args[i] = (char *)text(cJSON_GetArrayItem(argv, i));
    }
    if (pipe(in) || pipe(out)) {
        goto bad;
    }
    for (i = 0; i < 2; ++i) {
        fcntl(in[i], F_SETFD, FD_CLOEXEC);
        fcntl(out[i], F_SETFD, FD_CLOEXEC);
    }
    c->pid = fork();
    if (c->pid == 0) {
        setpgid(0, 0);
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execvp(args[0], args);
        _exit(127);
    }
    if (c->pid < 0) {
        c->pid = 0;
        goto bad;
    }
    setpgid(c->pid, c->pid);
    close(in[0]);
    close(out[1]);
    c->input = in[1];
    c->output = out[0];
    fcntl(c->input, F_SETFL, O_NONBLOCK);
    fcntl(c->output, F_SETFL, O_NONBLOCK);
    free(args);
    return 1;
bad:
    for (i = 0; i < 2; ++i) {
        if (in[i] >= 0) {
            close(in[i]);
        }
        if (out[i] >= 0) {
            close(out[i]);
        }
    }
    free(args);
    return 0;
}
#endif
/* 读写共享单次 RPC 的绝对截止时间；超时或协议错误立即使连接失效。 */
static int send_json(AI_CHAT_MCP *c, const cJSON *value, uint64_t deadline) {
    char *data = cJSON_PrintUnformatted(value), *line;
    size_t size;
    int ok = 0;
    if (!data) {
        return 0;
    }
    size = strlen(data);
    if (size > 32768 || now_ms() >= deadline) {
        free(data);
        return 0;
    }
    line = (char *)malloc(size + 2);
    if (!line) {
        free(data);
        return 0;
    }
    memcpy(line, data, size);
    line[size++] = '\n';
    line[size] = 0;
    free(data);
#ifdef _WIN32
    {
        WriteTask task = {c->input, line, (DWORD)size, 0};
        HANDLE thread = CreateThread(NULL, 0, writer, &task, 0, NULL);
        if (thread) {
            uint64_t now = now_ms();
            DWORD wait = WaitForSingleObject(thread, now < deadline ? (DWORD)(deadline - now) : 0);
            if (wait != WAIT_OBJECT_0) {
                CancelSynchronousIo(thread);
            }
            WaitForSingleObject(thread, INFINITE);
            ok = wait == WAIT_OBJECT_0 && task.ok;
            CloseHandle(thread);
        }
    }
#else
    {
        size_t offset = 0;
        sigset_t blocked, previous, pending;
        int was_pending;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        sigpending(&pending);
        was_pending = sigismember(&pending, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        while (offset < size && now_ms() < deadline) {
            struct pollfd fd = {c->input, POLLOUT, 0};
            ssize_t n;
            uint64_t now = now_ms();
            int ready = poll(&fd, 1, now < deadline ? (int)(deadline - now) : 0);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0 || !(fd.revents & POLLOUT)) {
                break;
            }
            n = write(c->input, line + offset, size - offset);
            if (n > 0) {
                offset += (size_t)n;
            } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
        sigpending(&pending);
        if (!was_pending && sigismember(&pending, SIGPIPE)) {
            int received;
            sigwait(&blocked, &received);
        }
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        ok = offset == size;
    }
#endif
    free(line);
    return ok;
}
static cJSON *read_json(AI_CHAT_MCP *c, uint64_t deadline) {
    while (now_ms() < deadline) {
        char *newline = (char *)memchr(c->buffer, '\n', c->used);
        size_t available = LIMIT - c->used;
        if (newline) {
            size_t size = (size_t)(newline - c->buffer);
            cJSON *value;
            *newline = 0;
            value = cJSON_ParseWithOpts(c->buffer, NULL, 1);
            memmove(c->buffer, newline + 1, c->used - size - 1);
            c->used -= size + 1;
            return value;
        }
        if (!available) {
            return NULL;
        }
#ifdef _WIN32
        {
            DWORD count = 0, n = 0;
            if (!PeekNamedPipe(c->output, NULL, 0, NULL, &count, NULL)) {
                return NULL;
            }
            if (!count) {
                Sleep(2);
                continue;
            }
            if (count > available) {
                count = (DWORD)available;
            }
            if (!ReadFile(c->output, c->buffer + c->used, count, &n, NULL) || !n) {
                return NULL;
            }
            c->used += n;
        }
#else
        {
            struct pollfd fd = {c->output, POLLIN, 0};
            ssize_t n;
            uint64_t now = now_ms();
            int ready = poll(&fd, 1, now < deadline ? (int)(deadline - now) : 0);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                return NULL;
            }
            n = read(c->output, c->buffer + c->used, available);
            if (n > 0) {
                c->used += (size_t)n;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                return NULL;
            }
        }
#endif
    }
    return NULL;
}
static cJSON *rpc(AI_CHAT_MCP *c, const char *method, cJSON *params) {
    cJSON *q = cJSON_CreateObject(), *response = NULL, *result = NULL;
    unsigned id = ++c->sequence;
    uint64_t deadline = now_ms() + c->timeout;
    cJSON_AddStringToObject(q, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(q, "id", id);
    cJSON_AddStringToObject(q, "method", method);
    cJSON_AddItemToObject(q, "params", params);
    if (!c->alive || !send_json(c, q, deadline)) {
        goto done;
    }
    while ((response = read_json(c, deadline)) != NULL) {
        if (!cJSON_IsObject(response) || strcmp(text(GET(response, "jsonrpc")), "2.0")) {
            break;
        }
        if (cJSON_IsString(GET(response, "method"))) {
            if (GET(response, "id")) {
                cJSON *reply = cJSON_CreateObject(), *error;
                cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
                cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(GET(response, "id"), 1));
                error = cJSON_AddObjectToObject(reply, "error");
                cJSON_AddNumberToObject(error, "code", -32601);
                cJSON_AddStringToObject(error, "message", "Unsupported client request");
                if (!send_json(c, reply, deadline)) {
                    cJSON_Delete(reply);
                    break;
                }
                cJSON_Delete(reply);
            }
            cJSON_Delete(response);
            response = NULL;
            continue;
        }
        if (!cJSON_IsNumber(GET(response, "id")) || GET(response, "id")->valuedouble != id ||
            GET(response, "error") || !GET(response, "result")) {
            break;
        }
        result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
        break;
    }
done:
    cJSON_Delete(q);
    cJSON_Delete(response);
    if (!result) {
        stop(c);
    }
    return result;
}
static AI_CHAT_StatusTypeDef invoke(const cJSON *args, cJSON **result, void *user) {
    Binding *b = (Binding *)user;
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", b->original);
    cJSON_AddItemToObject(params, "arguments", cJSON_Duplicate(args, 1));
    *result = rpc(b->client, "tools/call", params);
    if (!cJSON_IsObject(*result)) {
        cJSON *content, *item;
        cJSON_Delete(*result);
        *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(*result, "isError", 1);
        content = cJSON_AddArrayToObject(*result, "content");
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text",
                                "MCP transport/protocol failure; session closed; operation not retried");
        cJSON_AddItemToArray(content, item);
    }
    return *result ? AI_CHAT_OK : AI_CHAT_NO_MEMORY;
}
AI_CHAT_StatusTypeDef AI_CHAT_MCP_Attach(AI_CHAT_HandleTypeDef *engine, const char *prefix, const cJSON *argv,
                                         unsigned timeout, AI_CHAT_MCP **out) {
    AI_CHAT_MCP *c;
    cJSON *params = NULL, *result = NULL, *notify = NULL, *definitions = NULL, *seen = NULL, *existing = NULL;
    const cJSON *v;
    char *cursor = NULL;
    Binding *b;
    AI_CHAT_StatusTypeDef status = AI_CHAT_ERROR;
    if (out) {
        *out = NULL;
    }
    if (!out || !engine || !valid_name(prefix) || !cJSON_IsArray(argv) || !cJSON_GetArraySize(argv) ||
        !timeout || timeout > 60000) {
        return AI_CHAT_ERROR;
    }
    {
        size_t total = 0;
        cJSON_ArrayForEach(v, argv) {
            if (!cJSON_IsString(v) || !*text(v)) {
                return AI_CHAT_ERROR;
            }
            total += strlen(text(v)) + 1;
        }
        if (total > 32768) {
            return AI_CHAT_ERROR;
        }
    }
    c = (AI_CHAT_MCP *)calloc(1, sizeof(*c));
    if (!c) {
        return AI_CHAT_NO_MEMORY;
    }
#ifndef _WIN32
    c->input = -1;
    c->output = -1;
#endif
    c->timeout = timeout;
    c->buffer = (char *)malloc(LIMIT + 1);
    if (!c->buffer || !spawn(c, argv)) {
        goto done;
    }
    c->alive = 1;
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", "2025-11-25");
    cJSON_AddObjectToObject(params, "capabilities");
    {
        cJSON *info = cJSON_AddObjectToObject(params, "clientInfo");
        cJSON_AddStringToObject(info, "name", "EmbedAiChatC");
        cJSON_AddStringToObject(info, "version", "0.1.0");
    }
    result = rpc(c, "initialize", params);
    params = NULL;
    if (!result || (strcmp(text(GET(result, "protocolVersion")), "2025-11-25") &&
                    strcmp(text(GET(result, "protocolVersion")), "2025-06-18") &&
                    strcmp(text(GET(result, "protocolVersion")), "2024-11-05"))) {
        goto done;
    }
    cJSON_Delete(result);
    result = NULL;
    notify = cJSON_CreateObject();
    cJSON_AddStringToObject(notify, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notify, "method", "notifications/initialized");
    if (!send_json(c, notify, now_ms() + timeout)) {
        goto done;
    }
    definitions = cJSON_CreateArray();
    seen = cJSON_CreateArray();
    existing = AI_CHAT_ToolDefinitions(engine);
    if (!definitions || !seen || !existing) {
        goto done;
    }
    do {
        const cJSON *tool;
        params = cJSON_CreateObject();
        if (cursor) {
            cJSON_AddStringToObject(params, "cursor", cursor);
        }
        result = rpc(c, "tools/list", params);
        params = NULL;
        free(cursor);
        cursor = NULL;
        if (!result || !cJSON_IsArray(GET(result, "tools"))) {
            goto done;
        }
        cJSON_ArrayForEach(tool, GET(result, "tools")) {
            char name[65];
            const cJSON *known;
            size_t length = strlen(prefix) + strlen(text(GET(tool, "name"))) + 1;
            if (!valid_name(text(GET(tool, "name"))) || length > 64 ||
                !cJSON_IsObject(GET(tool, "inputSchema"))) {
                goto done;
            }
            snprintf(name, sizeof(name), "%s_%s", prefix, text(GET(tool, "name")));
            cJSON_ArrayForEach(known, existing) {
                if (!strcmp(name, text(GET(known, "name")))) {
                    goto done;
                }
            }
            cJSON_ArrayForEach(known, definitions) {
                if (!strcmp(name, text(GET(known, "registeredName")))) {
                    goto done;
                }
            }
            {
                cJSON *entry = cJSON_Duplicate(tool, 1);
                if (!entry) {
                    goto done;
                }
                cJSON_AddStringToObject(entry, "registeredName", name);
                cJSON_AddItemToArray(definitions, entry);
            }
        }
        if (GET(result, "nextCursor")) {
            const cJSON *old;
            const char *next = text(GET(result, "nextCursor"));
            if (!cJSON_IsString(GET(result, "nextCursor")) || !*next) {
                goto done;
            }
            cJSON_ArrayForEach(old, seen) {
                if (!strcmp(next, text(old))) {
                    goto done;
                }
            }
            cursor = copy(next);
            if (!cursor || !cJSON_AddItemToArray(seen, cJSON_CreateString(next))) {
                goto done;
            }
        }
        {
            char *check = cJSON_PrintUnformatted(definitions), *check_seen = cJSON_PrintUnformatted(seen);
            int large = !check || !check_seen || strlen(check) + strlen(check_seen) > LIMIT;
            free(check);
            free(check_seen);
            if (large) {
                goto done;
            }
        }
        cJSON_Delete(result);
        result = NULL;
    } while (cursor);
    cJSON_ArrayForEach(v, definitions) {
        b = (Binding *)calloc(1, sizeof(*b));
        if (!b) {
            goto done;
        }
        b->client = c;
        b->name = copy(text(GET(v, "registeredName")));
        b->original = copy(text(GET(v, "name")));
        if (!b->name || !b->original ||
            AI_CHAT_RegisterTool(engine, b->name, text(GET(v, "description")), GET(v, "inputSchema"), invoke,
                                 b) != AI_CHAT_OK) {
            free(b->name);
            free(b->original);
            free(b);
            goto done;
        }
        b->next = c->bindings;
        c->bindings = b;
    }
    *out = c;
    status = AI_CHAT_OK;
done:
    free(cursor);
    cJSON_Delete(params);
    cJSON_Delete(result);
    cJSON_Delete(notify);
    cJSON_Delete(definitions);
    cJSON_Delete(seen);
    cJSON_Delete(existing);
    if (status != AI_CHAT_OK) {
        for (b = c->bindings; b; b = b->next) {
            AI_CHAT_UnregisterTool(engine, b->name);
        }
        AI_CHAT_MCP_Close(c);
    }
    return status;
}
