#pragma once
#include "json_util.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace embed_ai_chat {

/**
 * 宿主可注入的 HTTP JSON 请求回调。
 *
 * @param url 完整请求 URL。
 * @param headers 请求头列表，包含敏感凭据，不得记录到日志。
 * @param body 请求 JSON 正文。
 * @return 解析后的模型响应 JSON 对象。
 * @throws std::exception 网络、HTTP 或响应解析失败时，由回调实现抛出。
 * @note 在 Engine::send 的调用线程中串行执行；框架不会自动重试。
 */
using Http = std::function<Json(const std::string &, const std::vector<std::string> &, const Json &)>;

/**
 * 执行宿主白名单操作的工具回调。
 *
 * @param arguments 已通过工具 JSON Schema 校验的参数。
 * @return 可序列化的实际执行结果，最大 1 MiB。
 * @throws std::exception 操作失败时由宿主抛出，模型调用流程会保存失败审计。
 * @note 在调用线程执行；界面宿主应自行切换到 UI 线程并回读实际结果。
 */
using Handler = std::function<Json(const Json &)>;

/**
 * 模型连接配置，由宿主在发送请求前填写。
 *
 * @note 配置与 API Key 不包含在历史归档中；访问时应与 send 串行执行。
 */
struct ModelConfig {
    /** 协议类型：openai（包括兼容接口、Kimi、DeepSeek）或 anthropic。 */
    std::string provider = "openai";
    /** API 基础地址，不含 /chat/completions 或 /messages 后缀。 */
    std::string base_url = "https://api.openai.com/v1";
    /** 当前账号可用的模型 ID。 */
    std::string model;
    /** 仅内存持有的凭据，不得记录到日志。 */
    std::string api_key;
    /** 输入上下文估算预算，范围 2000–1000000；压缩后仍超限则拒绝请求。 */
    size_t context_budget = 16000;
    /** 每次模型响应的输出 token 上限，1–1000000。 */
    size_t max_tokens = 2048;
    /** 滚动摘要的独立输出 token 上限，1–1000000。 */
    size_t max_summary_tokens = 1024;
    /** 自动摘要较早历史，保留原始归档。 */
    bool auto_compact = true;
    /** 自动压缩的预算使用百分比，1–100。 */
    unsigned compression_threshold = 90;
    /** 单次聊天的工具循环轮次上限，0 不限制；正整数为显式上限。 */
    unsigned max_rounds = 0;
};

/**
 * 一个宿主白名单工具的定义，供模型发现及执行。
 *
 * @note schema 采用 validate 文档所述子集；只有注册的回调可以被调用。
 */
struct Tool {
    /** 唯一名称，包含 1–64 位字母、数字或下划线。 */
    std::string name;
    /** 提供给模型阅读的功能与适用场景说明。 */
    std::string description;
    /** 工具参数的 JSON Schema。 */
    Json schema;
    /** 执行宿主操作并返回实际结果的回调。 */
    Handler handler;
};

/**
 * 持有一个使用 stdio JSON-RPC 的 MCP 子进程及其会话。
 *
 * @note 命令采用 argv，不经过 shell；超时或协议损坏后关闭连接，不重发操作。
 * @note 所有接口必须串行调用；支持 2025-11-25、2025-06-18、2024-11-05 会话协议。
 */
class McpClient {
  public:
    /**
     * 启动 MCP 子进程并完成 initialize/initialized 握手。
     *
     * @param argv 可执行文件及独立参数，数组不得为空。
     * @param timeout_ms 每次 RPC 的超时，范围 1–60000 毫秒。
     * @throws std::runtime_error 参数无效、进程启动失败、超时或协议版本不受支持。
     * @throws std::exception 握手响应无法解析或结构不符合要求。
     * @note 构造失败时释放已创建的进程与管道。
     */
    explicit McpClient(const std::vector<std::string> &argv, int timeout_ms = 15000);

    /**
     * 关闭本客户端启动的 MCP 子进程并释放管道。
     *
     * @note 不关闭宿主或其他客户端创建的程序。
     */
    ~McpClient();

    /** 禁止复制，确保 MCP 进程及管道只有一个所有者。 */
    McpClient(const McpClient &) = delete;
    /** 禁止复制赋值，避免共享可变协议会话。 */
    McpClient &operator=(const McpClient &) = delete;

    /**
     * 分页发现服务端全部工具，最多读取 100 页。
     *
     * @return 工具定义数组，每项保留 name、description 和 inputSchema。
     * @throws std::runtime_error RPC 失败、超时、分页循环、重复名称或页数超限。
     * @throws std::exception 服务端响应无效。
     */
    Json tools();

    /**
     * 调用一个 MCP 工具，保留原始工具级执行结果。
     *
     * @param name 服务端工具名称，不包含 Engine 注册时添加的前缀。
     * @param arguments 工具参数 JSON 对象。
     * @return 原始 MCP 结果，包括 content、structuredContent 及可选 isError。
     * @throws std::runtime_error 连接已关闭、请求过大、RPC 失败或超时。
     * @throws std::exception 响应无法解析或结构无效。
     * @note 超时后执行结果可能未知；检查宿主状态后再决定下一步操作。
     */
    Json call(const std::string &name, const Json &arguments);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    /**
     * 串行发送一个 RPC 请求，处理通知并等待对应 ID 的结果。
     *
     * @param method MCP 方法名称。
     * @param params 请求参数对象。
     * @return 响应中的 result JSON。
     * @throws std::exception 传输或协议失败；失败后关闭当前连接。
     */
    Json rpc(const std::string &method, const Json &params);
};

/**
 * 提供 AI 聊天、MCP、Skill 与宿主工具循环的核心引擎。
 *
 * @note 所有接口与配置修改必须串行执行；不支持并发 send。
 * @note 历史包含原文、摘要与模型工具审计，存储位置由宿主决定。
 */
class Engine {
  public:
    /** 模型连接及上下文配置，调用 send 前由宿主设置。 */
    ModelConfig config;
    /** 默认关闭；开启后才允许模型和手动入口执行已注册工具。 */
    bool allow_tools = false;

    /**
     * 创建聊天引擎并注册 skill_list、skill_activate 两个内置工具。
     *
     * @param http 可选宿主网络回调；为空时使用 post_json 的 libcurl 实现。
     * @note 回调捕获当前对象，因此引擎禁止复制。
     */
    explicit Engine(Http http = {});

    /** 禁止复制，避免工具回调引用另一个引擎的状态。 */
    Engine(const Engine &) = delete;
    /** 禁止复制赋值，保证回调与所属引擎一致。 */
    Engine &operator=(const Engine &) = delete;

    /**
     * 注册一个宿主白名单工具。
     *
     * @param tool 工具唯一名称、用途说明、参数 schema 及实际执行回调。
     * @throws std::runtime_error 名称无效、重复、回调缺失或 schema 不是对象。
     * @note 回调在调用线程执行，宿主应在回调内实现所需的线程调度。
     */
    void register_tool(Tool tool);

    /**
     * 发现 MCP 工具并以 prefix_ 前缀注册到引擎。
     *
     * @param prefix 工具名称前缀，采用与工具名相同的字符规则。
     * @param client 已完成握手的客户端；引擎通过 shared_ptr 保持其存活。
     * @throws std::runtime_error 客户端无效、发现失败、工具名称冲突或过长。
     * @throws std::exception 工具定义结构无效。
     * @note 工具发现及名称检查失败时不会部分注册工具。
     */
    void attach_mcp(const std::string &prefix, std::shared_ptr<McpClient> client);

    /**
     * 加载目录中的 UTF-8 SKILL.md 指令包。
     *
     * @param directory Skill 目录路径，名称允许字母、数字、下划线及连字符。
     * @return 用于 activate_skill 的目录名称。
     * @throws std::runtime_error 目录名、文件大小或 YAML frontmatter 无效，或读取失败。
     * @throws std::filesystem::filesystem_error 文件状态查询失败。
     * @note 文本上限 64 KiB；此接口只读取指引，不执行附带脚本。
     */
    std::string load_skill(const std::string &directory);

    /**
     * 激活已加载的 Skill，作为后续模型请求的系统指引。
     *
     * @param name 已加载的 Skill 名称；空字符串解除激活。
     * @throws std::runtime_error 名称尚未加载。
     */
    void activate_skill(const std::string &name);

    /**
     * 发送用户原文并执行有界的模型工具循环。
     *
     * @param text 用户消息，长度为 1–65536 个 UTF-8 字节。
     * @return 助手最终回复的 Markdown 原文。
     * @throws std::runtime_error 输入、预算或轮次配置无效，或请求超限。
     * @throws std::exception 网络、模型协议、摘要或宿主网络回调失败。
     * @note 不自动重试；已经执行的工具审计在后续请求失败时仍会保留。
     */
    std::string send(const std::string &text);

    /**
     * 导出可供文件或宿主项目容器保存的历史数据。
     *
     * @return 版本为 1 的 JSON 归档，包含 messages、summary、covered 和 audit。
     * @note 不包含 API Key、连接配置或可执行回调；调用者取得独立数据副本。
     */
    Json export_history() const;

    /**
     * 校验归档后替换当前聊天、摘要和工具审计。
     *
     * @param archive export_history 导出的版本 1 JSON 归档，最大 4 MiB。
     * @throws std::runtime_error 归档大小、版本、消息或摘要覆盖计数无效。
     * @throws std::exception 必需字段缺失或字段类型无效。
     * @note 校验失败保留原历史；载入归档不恢复连接，也不重放操作。
     */
    void import_history(const Json &archive);

    /**
     * 清空消息、摘要及工具审计。
     *
     * @note 保留模型配置、工具连接和 Skill；不会回滚宿主已经发生的修改。
     */
    void clear();

    /**
     * 读取当前注册工具的发现定义。
     *
     * @return 工具定义数组，不包含处理回调、API Key 或其他会话凭据。
     */
    Json tools() const;

    /**
     * 通过与模型相同的权限及参数校验入口手动调用工具。
     *
     * @param name 已注册的工具名称，MCP 工具使用添加前缀后的名称。
     * @param arguments 工具参数 JSON 对象。
     * @return 实际工具执行结果，最大 1 MiB。
     * @throws std::runtime_error 工具关闭、名称未知、参数无效或结果超限。
     * @throws std::exception 宿主回调或 MCP 调用失败。
     * @note 手动调用的审计由宿主负责；模型调用由 send 记录。
     */
    Json invoke(const std::string &name, const Json &arguments);

  private:
    Http m_http;
    std::map<std::string, Tool> m_tools;
    std::map<std::string, std::string> m_skills;
    std::string m_active, m_summary;
    size_t m_covered = 0;
    Json m_messages = Json::array({}), m_audit = Json::array({});

    /**
     * 适配供应商协议并执行一次模型请求。
     *
     * @param messages 内部统一格式的请求消息数组。
     * @param with_tools 是否允许将当前工具定义加入请求。
     * @return 统一为 assistant.content 和 tool_calls 的助手消息。
     * @throws std::exception 输入预算、网络、协议或响应解析失败。
     */
    Json request(const Json &messages, bool with_tools);

    /**
     * 生成历史发送视图，加入摘要、未覆盖原文及近期实际工具审计。
     *
     * @return 可提交给模型的消息数组，不改变原始归档。
     */
    Json context() const;

    /**
     * 在需要时请求模型摘要较早消息，保留近期四条原文。
     *
     * @throws std::exception 压缩输入超限、摘要请求失败或摘要为空。
     * @note 只更新摘要及覆盖计数，完整原文继续保存在历史中。
     */
    void compact();
};

/**
 * 使用默认 libcurl 网络层提交 HTTP JSON 请求。
 *
 * @param url 完整 HTTPS URL；HTTP 仅允许 localhost、127.0.0.1 或 [::1]。
 * @param headers 请求头列表，包含机密认证信息，不得记录。
 * @param body 请求 JSON 正文。
 * @return 已解析的 JSON 响应，原始响应最大 4 MiB。
 * @throws std::runtime_error URL 或请求头无效、初始化失败、网络错误或非 2xx 响应。
 * @throws std::exception 响应无法解析为 JSON。
 * @note TLS 验证开启；连接超时 15 秒，总超时 120 秒，不自动重试。
 */
Json post_json(const std::string &url, const std::vector<std::string> &headers, const Json &body);

/**
 * 校验工具参数与受支持的 JSON Schema 子集。
 *
 * @param schema Schema 对象或布尔值；未知约束关键字明确拒绝。
 * @param value 待校验的 JSON 值。
 * @param path 错误提示中的字段路径，默认从 $ 开始。
 * @throws std::runtime_error 参数不满足约束，或 schema 含不支持的关键字。
 * @throws std::exception Schema 字段类型无效。
 * @note 支持基本类型、anyOf、enum、对象及数组约束、数值范围和长度约束。
 */
void validate(const Json &schema, const Json &value, const std::string &path = "$");
} // namespace embed_ai_chat
