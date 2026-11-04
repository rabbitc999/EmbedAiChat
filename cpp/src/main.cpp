#include "embed_ai_chat/framework.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
using namespace embed_ai_chat;
namespace {
// 控制台隐藏密钥输入；重定向 stdin 时允许测试注入，但不会打印该值。
/**
 * 读取当前会话密钥，并在真实终端临时关闭输入回显。
 *
 * @return 用户输入的密钥；不将其写入配置或日志。
 */
std::string read_key() {
#ifdef _WIN32
    auto console = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool terminal = GetConsoleMode(console, &mode) != 0;
    if (terminal) {
        SetConsoleMode(console, mode & ~ENABLE_ECHO_INPUT);
    }
#else
    const bool terminal = isatty(STDIN_FILENO);
    termios mode{};
    if (terminal) {
        tcgetattr(STDIN_FILENO, &mode);
        auto hidden = mode;
        hidden.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    }
#endif
    std::string key;
    // 输入结束后恢复原有终端模式；重定向 stdin 时不修改控制台状态。
    std::getline(std::cin, key);
#ifdef _WIN32
    if (terminal) {
        SetConsoleMode(console, mode);
    }
#else
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &mode);
    }
#endif
    std::cout << "\n";
    return key;
}
/**
 * 从 UTF-8 路径指定的文件载入 JSON。
 *
 * @param path 配置文件或归档文件路径。
 * @return 解析后的 JSON 值。
 * @throws std::exception 文件无法打开或 JSON 格式无效。
 */
Json read_json(const std::string &path) {
    std::ifstream in(std::filesystem::u8path(path));
    if (!in) {
        throw std::runtime_error("Cannot open JSON file");
    }
    return json_parse(in);
}
/**
 * 先写同目录临时文件，再替换完整 JSON 归档。
 *
 * @param path 目标归档路径。
 * @param value 由引擎导出的历史对象。
 * @throws std::exception 写入或替换目标文件失败。
 */
void save(const std::string &path, const Json &value) {
    auto target = std::filesystem::u8path(path), temporary = target;
    // 临时文件与目标处于同一目录，写入成功后再替换，避免常规写入失败破坏旧归档。
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << value.dump(2);
        if (!out) {
            throw std::runtime_error("Cannot write archive");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace archive");
    }
#else
    std::filesystem::rename(temporary, target);
#endif
}
} // namespace
/** 控制台示例入口。宿主接入只需要 framework.hpp，不需要复制本文件。
 * --config 读取无密钥配置；--ask 用于一次性请求；交互模式 /help 列出命令。 */
int run(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        Engine engine;
        Json state = Json::object({{"title", "Demo host"}, {"value", 0}});
        // 下面三个回调模拟宿主程序；替换这些回调即可控制真实应用的白名单能力。
        engine.register_tool({"host_get_state", "Read demo host state",
                              Json::object({{"type", "object"}, {"additionalProperties", false}}),
                              [&](const Json &) { return state; }});
        engine.register_tool(
            {"host_set_title", "Set demo host title",
             Json::object({{"type", "object"},
                           {"properties", Json::object({{"title", Json::object({{"type", "string"},
                                                                                {"minLength", 1},
                                                                                {"maxLength", 120}})}})},
                           {"required", Json::array({"title"})},
                           {"additionalProperties", false}}),
             [&](const Json &a) {
                 json_at(state, "title") = json_at(a, "title");
                 return state;
             }});
        engine.register_tool(
            {"host_set_value", "Set demo host numeric value",
             Json::object(
                 {{"type", "object"},
                  {"properties",
                   Json::object(
                       {{"value", Json::object({{"type", "number"}, {"minimum", 0}, {"maximum", 100}})}})},
                  {"required", Json::array({"value"})},
                  {"additionalProperties", false}}),
             [&](const Json &a) {
                 json_at(state, "value") = json_at(a, "value");
                 return state;
             }});
        // 命令行只承载配置路径与问题，不接受密钥参数，避免凭据进入进程命令行记录。
        std::string configuration, ask;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) {
                configuration = argv[++i];
            } else if (arg == "--ask" && i + 1 < argc) {
                ask = argv[++i];
            } else {
                throw std::runtime_error("Usage: EmbedAiChatCpp [--config file] [--ask text]");
            }
        }
        if (!configuration.empty()) {
            auto cfg = read_json(configuration);
            if (json_contains(cfg, "key") || json_contains(cfg, "api_key")) {
                throw std::runtime_error("Do not put API keys in config; use /key");
            }
            engine.config.provider = json_value(cfg, "provider", "openai");
            engine.config.base_url = json_value(cfg, "base_url", "https://api.openai.com/v1");
            engine.config.model = json_value(cfg, "model", "");
            engine.config.compression_threshold = json_value(cfg, "compression_threshold", unsigned(90));
            engine.config.max_summary_tokens = json_value(cfg, "max_summary_tokens", size_t(1024));
            engine.config.max_rounds = json_value(cfg, "max_rounds", unsigned(0));
            engine.config.max_tokens = json_value(cfg, "max_tokens", size_t(2048));
            engine.config.auto_compact = json_value(cfg, "auto_compact", true);
            engine.config.context_budget = json_value(cfg, "context_budget", size_t(16000));
            engine.allow_tools = json_value(cfg, "allow_tools", false);
            // Skill 相对路径以配置文件目录为基准，启动时的工作目录变化不会改变其指向。
            const auto base = std::filesystem::absolute(std::filesystem::u8path(configuration)).parent_path();
            const auto configured_skills = json_value(cfg, "skills", Json::array({}));
            for (const auto &skill : configured_skills.requireArray()) {
                engine.load_skill((base / std::filesystem::u8path(json_get<std::string>(skill))).u8string());
            }
            engine.activate_skill(json_value(cfg, "active_skill", std::string()));
            const auto configured_servers = json_value(cfg, "mcp", Json::array({}));
            for (const auto &server : configured_servers.requireArray()) {
                engine.attach_mcp(json_get<std::string>(json_at(server, "prefix")),
                                  std::make_shared<McpClient>(
                                      json_get<std::vector<std::string>>(json_at(server, "command")),
                                      json_value(server, "timeout_ms", 15000)));
            }
        }
        if (!ask.empty()) {
            engine.send(ask, [](const Json &event) {
                if (json_at(event, "type") == "assistant") {
                    std::cout << json_get<std::string>(json_at(event, "text")) << std::endl;
                }
            });
            std::cout << Json::object({{"host", state}, {"history", engine.export_history()}}).dump() << "\n";
            return 0;
        }
        std::cout << "EmbedAiChat C++ 控制台；/help 查看命令，/key 输入会话密钥。\n";
        std::string line;
        // 单条命令失败只结束本次处理；循环保留引擎状态，允许查看审计或保存历史。
        while (std::cout << "> " && std::getline(std::cin, line)) {
            try {
                if (line == "/exit") {
                    break;
                }
                if (line == "/help") {
                    std::cout << "/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n/mcp "
                                 "<prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n/save <文件> | "
                                 "/load <文件> | /clear | /audit | /exit\n模型连接通过 --config "
                                 "配置；普通文字直接发送。\n";
                    continue;
                }
                if (line == "/key") {
                    std::cout << "API Key (hidden): ";
                    engine.config.api_key = read_key();
                    continue;
                }
                if (line == "/tools") {
                    std::cout << engine.tools().dump(2) << "\n";
                    continue;
                }
                if (line == "/host") {
                    std::cout << state.dump() << "\n";
                    continue;
                }
                if (line == "/audit") {
                    std::cout << json_at(engine.export_history(), "audit").dump(2) << "\n";
                    continue;
                }
                if (line == "/clear") {
                    engine.clear();
                    continue;
                }
                if (line.rfind("/allow ", 0) == 0) {
                    auto v = line.substr(7);
                    if (v != "on" && v != "off") {
                        throw std::runtime_error("Use /allow on|off");
                    }
                    engine.allow_tools = v == "on";
                    continue;
                }
                if (line.rfind("/load-skill ", 0) == 0) {
                    std::cout << engine.load_skill(line.substr(12)) << "\n";
                    continue;
                }
                if (line.rfind("/skill ", 0) == 0) {
                    engine.activate_skill(line.substr(7));
                    continue;
                }
                if (line.rfind("/save ", 0) == 0) {
                    save(line.substr(6), engine.export_history());
                    continue;
                }
                if (line.rfind("/load ", 0) == 0) {
                    engine.import_history(read_json(line.substr(6)));
                    continue;
                }
                if (line.rfind("/mcp ", 0) == 0 || line.rfind("/call ", 0) == 0) {
                    const bool mcp = line.rfind("/mcp ", 0) == 0;
                    auto rest = line.substr(mcp ? 5 : 6);
                    auto split = rest.find(' ');
                    if (split == std::string::npos) {
                        throw std::runtime_error("Supply name and JSON arguments");
                    }
                    auto name = rest.substr(0, split);
                    auto args = json_parse(rest.substr(split + 1));
                    if (mcp) {
                        engine.attach_mcp(
                            name, std::make_shared<McpClient>(json_get<std::vector<std::string>>(args)));
                    } else {
                        std::cout << engine.invoke(name, args).dump(2) << "\n";
                    }
                    continue;
                }
                if (!line.empty() && line[0] == '/') {
                    throw std::runtime_error("Unknown command; /help");
                }
                if (!line.empty()) {
                    engine.send(line, [](const Json &event) {
                        if (json_at(event, "type") == "assistant") {
                            std::cout << json_get<std::string>(json_at(event, "text")) << std::endl;
                        }
                    });
                }
            } catch (const std::exception &e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

// Windows CRT 的窄 argv 使用本机代码页；显式从宽字符入口转为 UTF-8。
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> arguments;
    std::vector<char *> pointers;
    for (int i = 0; i < argc; ++i) {
        int count = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), count, nullptr, nullptr);
        arg.pop_back();
        arguments.push_back(std::move(arg));
    }
    for (auto &arg : arguments) {
        pointers.push_back(arg.data());
    }
    return run(argc, pointers.data());
}
#else
int main(int argc, char **argv) {
    return run(argc, argv);
}
#endif
