#include "embed_ai_chat/framework.hpp"
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace embed_ai_chat {
namespace {
/**
 * 检查 MCP 传输层的内部不变量。
 *
 * @param ok 当前条件是否成立。
 * @param message 条件不成立时的错误说明。
 * @throws std::runtime_error 条件为 false。
 */
void require(bool ok, const char *message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
#ifdef _WIN32
// 按 CreateProcess 的 argv 规则转义，参数从不交给 cmd.exe 解释。
/**
 * 把严格 UTF-8 参数转换成 Windows 宽字符字符串。
 *
 * @param s 未经 shell 解释的 UTF-8 参数。
 * @return 可供 Windows 进程 API 使用的宽字符串。
 * @throws std::runtime_error 输入不是有效 UTF-8 或转换失败。
 */
std::wstring wide(const std::string &s) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    require(count > 0, "Invalid UTF-8 process argument");
    std::wstring out(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), count);
    return out;
}
// Windows 在引号前和参数末尾对反斜杠有不同规则，必须逐段转义以保留原始 argv。
/**
 * 按 Windows 命令行参数规则转义一个独立参数。
 *
 * @param s 原始宽字符参数，允许包含空格、引号和反斜杠。
 * @return 带外层引号、可还原为原始 argv 项的文本。
 */
std::wstring quote(const std::wstring &s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') {
            out += L'\\';
        }
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
#endif
} // namespace
struct McpClient::Impl {
    int timeout;
    std::int64_t id = 0;
    bool alive = true;
    std::string buffered;
#ifdef _WIN32
    HANDLE input = nullptr, output = nullptr, process = nullptr, job = nullptr;
#else
    int input = -1, output = -1;
    pid_t pid = -1;
#endif
    /**
     * 析构时释放本实例拥有的子进程和管道。
     */
    ~Impl() {
        close();
    }
    // 进程对象拥有管道及子进程；任何超时都会使连接失效，防止重试产生重复修改。
    /**
     * 使会话失效并清理自身启动的进程树和管道。
     */
    void close() {
        alive = false;
#ifdef _WIN32
        if (input) {
            CloseHandle(input);
            input = nullptr;
        }
        if (output) {
            CloseHandle(output);
            output = nullptr;
        }
        // 关闭带 KILL_ON_JOB_CLOSE 的 Job 会清理其进程树，避免服务派生的子进程残留。
        if (job) {
            CloseHandle(job);
            job = nullptr;
        }
        if (process) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
            process = nullptr;
        }
#else
        if (input >= 0) {
            ::close(input);
            input = -1;
        }
        if (output >= 0) {
            ::close(output);
            output = -1;
        }
        if (pid > 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
#endif
    }
    /**
     * 建立标准输入输出管道并启动受本实例管理的进程。
     *
     * @param argv 可执行文件及参数，不经过 shell。
     * @throws std::runtime_error 参数无效、管道创建失败或进程启动失败。
     */
    void start(const std::vector<std::string> &argv) {
        require(!argv.empty(), "Empty MCP command");
        for (const auto &arg : argv) {
            require(arg.find('\0') == std::string::npos, "NUL in MCP argument");
        }
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE child_in = nullptr, child_out = nullptr;
        require(CreatePipe(&child_in, &input, &sa, 65536) != 0, "Cannot create MCP stdin");
        if (!CreatePipe(&output, &child_out, &sa, 65536)) {
            CloseHandle(child_in);
            throw std::runtime_error("Cannot create MCP stdout");
        }
        // 父端管道不可被子进程继承，否则多出的句柄可能让读端永远收不到关闭信号。
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        std::wstring command;
        for (const auto &arg : argv) {
            if (!command.empty()) {
                command += L' ';
            }
            command += quote(wide(arg));
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_in;
        si.hStdOutput = child_out;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const bool job_ok =
            job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        // 先挂起创建，再加入 Job 后恢复，确保服务没有机会在受管理前派生进程。
        const bool ok =
            job_ok && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(child_in);
        CloseHandle(child_out);
        require(ok, "Cannot start MCP executable");
        process = pi.hProcess;
        if (!AssignProcessToJobObject(job, process)) {
            TerminateProcess(process, 1);
            CloseHandle(pi.hThread);
            throw std::runtime_error("Cannot isolate MCP child process");
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
#else
        int in[2], out[2];
        require(pipe(in) == 0, "Cannot create MCP stdin");
        if (pipe(out) != 0) {
            ::close(in[0]);
            ::close(in[1]);
            throw std::runtime_error("Cannot create MCP stdout");
        }
        pid = fork();
        if (pid < 0) {
            for (auto fd : in) {
                ::close(fd);
            }
            for (auto fd : out) {
                ::close(fd);
            }
            throw std::runtime_error("Cannot fork MCP");
        }
        // 子进程只保留标准输入输出所需的管道端；独立进程组用于后续统一清理。
        if (pid == 0) {
            setpgid(0, 0);
            dup2(in[0], STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            for (auto fd : in) {
                ::close(fd);
            }
            for (auto fd : out) {
                ::close(fd);
            }
            std::vector<char *> args;
            for (const auto &arg : argv) {
                args.push_back(const_cast<char *>(arg.c_str()));
            }
            args.push_back(nullptr);
            execvp(args[0], args.data());
            // exec 失败必须直接退出子进程，避免执行父进程的析构或刷新其继承的输出缓冲。
            _exit(127);
        }
        setpgid(pid, pid);
        ::close(in[0]);
        ::close(out[1]);
        input = in[1];
        output = out[0];
        fcntl(input, F_SETFL, O_NONBLOCK);
        fcntl(output, F_SETFL, O_NONBLOCK);

#endif
    }
    /**
     * 向 MCP 标准输入写入一条换行分隔的 JSON 消息。
     *
     * @param message 完整 JSON-RPC 消息，序列化后含换行不超过 32 KiB。
     * @throws std::runtime_error 会话关闭、消息过大、写入失败或 POSIX 写入超时。
     */
    void write(const Json &message) {
        require(alive, "MCP connection is closed");
        // stdio MCP 一行对应一条 JSON-RPC 消息；序列化器会转义正文内的换行。
        const auto text = message.dump() + "\n";
        require(text.size() <= 32768, "MCP request exceeds 32 KiB");
#ifdef _WIN32
        DWORD written = 0;
        require(WriteFile(input, text.data(), DWORD(text.size()), &written, nullptr) &&
                    written == text.size(),
                "MCP write failed");
#else
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        size_t offset = 0;
        while (offset < text.size()) {
            require(std::chrono::steady_clock::now() < deadline, "MCP write timed out");
            pollfd p{input, POLLOUT, 0};
            poll(&p, 1, 20);
            // 仅在当前线程的本次写入中屏蔽 SIGPIPE，不更改宿主的全局信号处理器。
            sigset_t blocked, previous, pending;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            sigpending(&pending);
            bool was_pending = sigismember(&pending, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, &previous);
            auto n = ::write(input, text.data() + offset, text.size() - offset);
            int saved_errno = errno;
            if (n < 0 && saved_errno == EPIPE && !was_pending) {
                sigset_t after;
                sigpending(&after);
                if (sigismember(&after, SIGPIPE)) {
                    int received = 0;
                    sigwait(&blocked, &received);
                }
            }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            errno = saved_errno;
            if (n > 0) {
                offset += size_t(n);
            } else {
                require(errno == EAGAIN || errno == EINTR, "MCP write failed");
            }
        }
#endif
    }
    /**
     * 在绝对截止时间前读取一条完整 MCP 消息。
     *
     * @param deadline 整个 RPC 共用的单调时钟截止时间。
     * @return 解析后的单条 JSON 消息；多余字节留在缓冲中。
     * @throws std::exception 超时、管道关闭、行过大或 JSON 无法解析。
     */
    Json read(std::chrono::steady_clock::time_point deadline) {
        // 读取使用整个 RPC 共享的绝对期限；零碎数据不会不断延长等待时间。
        while (std::chrono::steady_clock::now() < deadline) {
            auto newline = buffered.find('\n');
            if (newline != std::string::npos) {
                auto line = buffered.substr(0, newline);
                // 只消费当前完整行，保留同一批读取中下一条消息的字节。
                buffered.erase(0, newline + 1);
                return json_parse(line);
            }
            char bytes[4096];
            size_t count = 0;
#ifdef _WIN32
            DWORD ready = 0;
            require(PeekNamedPipe(output, nullptr, 0, nullptr, &ready, nullptr) != 0,
                    "MCP exited or pipe closed");
            if (ready) {
                DWORD got = 0;
                require(ReadFile(output, bytes, std::min<DWORD>(ready, sizeof(bytes)), &got, nullptr) != 0,
                        "MCP read failed");
                count = got;
            }
#else
            pollfd p{output, POLLIN, 0};
            poll(&p, 1, 20);
            auto got = ::read(output, bytes, sizeof(bytes));
            if (got > 0) {
                count = size_t(got);
            } else if (got == 0) {
                throw std::runtime_error("MCP exited");
            } else {
                require(errno == EAGAIN || errno == EINTR, "MCP read failed");
            }
#endif
            buffered.append(bytes, count);
            // 对尚未收到换行的缓冲也设限，避免对端用无限长单行占满宿主内存。
            require(buffered.size() <= 4 * 1024 * 1024, "MCP line exceeds 4 MiB");
            if (!count) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        throw std::runtime_error("MCP timed out; outcome unknown; inspect host before another edit");
    }
};
McpClient::McpClient(const std::vector<std::string> &argv, int timeout_ms)
    : m_impl(std::make_unique<Impl>()) {
    require(timeout_ms > 0 && timeout_ms <= 60000, "Invalid MCP timeout");
    m_impl->timeout = timeout_ms;
    m_impl->start(argv);
    const auto result =
        rpc("initialize",
            Json::object({{"protocolVersion", "2025-11-25"},
                          {"capabilities", Json::object({})},
                          {"clientInfo", Json::object({{"name", "EmbedAiChat"}, {"version", "0.1.0"}})}}));
    const std::set<std::string> versions{"2025-11-25", "2025-06-18", "2024-11-05"};
    require(versions.count(json_value(result, "protocolVersion", std::string())) != 0,
            "Unsupported negotiated MCP version");
    // 收到兼容版本的 initialize 结果后再发 initialized，服务端随后才能接受工具请求。
    m_impl->write(Json::object(
        {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", Json::object({})}}));
}
McpClient::~McpClient() = default;
/**
 * 串行发送 RPC，并在处理通知后等待匹配 ID 的响应。
 *
 * @param method MCP 方法名称。
 * @param params 该方法的请求参数。
 * @return 响应中的 result 对象。
 * @throws std::exception 传输或协议失败；失败后关闭会话且不重试。
 */
Json McpClient::rpc(const std::string &method, const Json &params) {
    try {
        const auto id = ++m_impl->id;
        m_impl->write(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_impl->timeout);
        while (true) {
            const auto reply = m_impl->read(deadline);
            require(reply.isObject() && json_value(reply, "jsonrpc", std::string()) == "2.0",
                    "Invalid MCP JSON-RPC message");
            // 等待响应期间可能插入通知或服务端请求；通知跳过，未实现的请求明确返回错误。
            if (json_contains(reply, "method")) {
                if (json_contains(reply, "id")) {
                    m_impl->write(Json::object(
                        {{"jsonrpc", "2.0"},
                         {"id", json_at(reply, "id")},
                         {"error",
                          Json::object({{"code", -32601}, {"message", "Client feature not supported"}})}}));
                }
                continue;
            }
            // 只接受当前请求的 ID，防止把其他响应误认为本次宿主操作已经成功。
            require(json_contains(reply, "id") && json_at(reply, "id") == id, "Mismatched MCP response ID");
            if (json_contains(reply, "error")) {
                throw std::runtime_error("MCP RPC error: " + json_at(reply, "error").dump());
            }
            return json_at(reply, "result");
        }
    } catch (...) {
        // 协议状态一旦未知就关闭会话；不在这里重试，避免已经执行的修改重复发生。
        m_impl->close();
        throw;
    }
}
Json McpClient::tools() {
    Json result = Json::array({}), params = Json::object({});
    // 同时记录分页游标和工具名，分别识别分页死循环与重复工具定义。
    std::set<std::string> cursors, names;
    for (int page = 0; page < 100; ++page) {
        auto data = rpc("tools/list", params);
        for (const auto &tool : json_at(data, "tools").requireArray()) {
            require(names.insert(json_get<std::string>(json_at(tool, "name"))).second, "Duplicate MCP tool");
            result.push_back(tool);
        }
        if (!json_contains(data, "nextCursor")) {
            return result;
        }
        auto cursor = json_get<std::string>(json_at(data, "nextCursor"));
        require(cursors.insert(cursor).second, "Repeated MCP cursor");
        params = Json::object({{"cursor", cursor}});
    }
    throw std::runtime_error("Too many MCP tool pages");
}
Json McpClient::call(const std::string &name, const Json &args) {
    return rpc("tools/call", Json::object({{"name", name}, {"arguments", args}}));
}
} // namespace embed_ai_chat
