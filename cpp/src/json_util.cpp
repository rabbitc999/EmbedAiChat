#include "embed_ai_chat/json_util.hpp"
#include <array>
#include <cmath>

namespace embed_ai_chat {
namespace {
/** 接收网络及归档 JSON 的最大字节数。 */
constexpr size_t maximum_json_bytes = 4 * 1024 * 1024;
/**
 * 检查文本是否由有效 Unicode 标量的标准 UTF-8 编码组成。
 * @param text 待检查的原始文本或解析后的字符串。
 * @throws std::runtime_error 存在非法字节、过长编码或孤立代理码点。
 */
void require_utf8(std::string_view text) {
    for (size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80) {
            continue;
        }
        unsigned remaining = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            code = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            code = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            code = first & 0x07;
            minimum = 0x10000;
        } else {
            throw std::runtime_error("Invalid JSON UTF-8 lead byte");
        }
        while (remaining > 0) {
            if (index >= text.size()) {
                throw std::runtime_error("Truncated JSON UTF-8");
            }
            const auto next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xc0) != 0x80) {
                throw std::runtime_error("Invalid JSON UTF-8 continuation");
            }
            code = (code << 6) | (next & 0x3f);
            --remaining;
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            throw std::runtime_error("Invalid JSON Unicode scalar");
        }
    }
}
/**
 * 检查解析树中的 Unicode 标量和数值可表示性。
 * @param value 原生解析器已经成功生成的值树。
 * @throws std::runtime_error 包含非法 Unicode 标量或非有限数值。
 * @note JSON 语法仍由原生解析器处理；此处只收紧网络边界的 Unicode 校验。
 */
void require_json_values(const Json &value) {
    if (value.isNumber() && !std::isfinite(*value.asDouble())) {
        throw std::runtime_error("JSON number outside finite numeric range");
    }
    if (value.isString()) {
        require_utf8(*value.asString());
    } else if (value.isArray()) {
        for (const auto &item : value.requireArray()) {
            require_json_values(item);
        }
    } else if (value.isObject()) {
        for (const auto &member : value.requireObject()) {
            require_utf8(member.key);
            require_json_values(member.value);
        }
    }
}

} // namespace
Json json_parse(std::string_view text) {
    if (text.size() > maximum_json_bytes) {
        throw std::runtime_error("JSON input exceeds 4 MiB");
    }
    // 不启用注释或尾随逗号，网络协议和归档保持标准 JSON。
    require_utf8(text);
    auto result = json::parse(text);
    if (!result.ok()) {
        const auto detail =
            result.diagnostics.empty() ? "Unknown JSON parse error" : result.diagnostics.front().toString();
        throw std::runtime_error("Invalid JSON: " + detail);
    }
    // 转义串本身是 ASCII，但可能解码成非法代理码点，需要检查实际字符串树。
    require_json_values(*result.value);
    return std::move(*result.value);
}
Json json_parse(std::istream &input) {
    std::string text;
    std::array<char, 4096> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        if (text.size() + static_cast<size_t>(input.gcount()) > maximum_json_bytes) {
            throw std::runtime_error("JSON input exceeds 4 MiB");
        }
        text.append(buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if (input.bad()) {
        throw std::runtime_error("JSON stream read failed");
    }
    return json_parse(text);
}
const Json &json_at(const Json &value, std::string_view key) {
    (void)value.requireObject();
    auto *found = value.find(key);
    if (!found) {
        throw std::out_of_range("Missing JSON field: " + std::string(key));
    }
    return *found;
}
Json &json_at(Json &value, std::string_view key) {
    return value[key];
}
const Json &json_at(const Json &value, size_t index) {
    return value.at(index);
}
Json &json_at(Json &value, size_t index) {
    return value.at(index);
}
bool json_contains(const Json &value, std::string_view key) {
    return value.find(key) != nullptr;
}
} // namespace embed_ai_chat
