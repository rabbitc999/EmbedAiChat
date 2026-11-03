#pragma once
#include <chali_json/json.hpp>
#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace embed_ai_chat {
/** 内部 JSON 值类型，与 json::Value 是同一类型。 */
using Json = json::Value;

/**
 * 严格解析 JSON 文本，保留底层解析器的行列诊断。
 * @param text UTF-8 JSON 文本，最大 4 MiB。
 * @return 原生 json::Value。
 * @throws std::runtime_error 文本过大或语法解析失败。
 */
Json json_parse(std::string_view text);
/**
 * 在大小限制内读取输入流并解析 JSON。
 * @param input 文件或其他输入流。
 * @return 原生 JSON 值。
 * @throws std::runtime_error 流读取失败、输入超过 4 MiB 或 JSON 无效。
 */
Json json_parse(std::istream &input);
/**
 * 读取必需的对象字段。
 * @param value JSON 对象。
 * @param key 字段名称。
 * @return 字段的只读引用，与所属对象共享生命周期。
 * @throws std::domain_error 当前值不是对象。
 * @throws std::out_of_range 必需字段不存在。
 */
const Json &json_at(const Json &value, std::string_view key);
/**
 * 取得可写对象字段，字段缺失时由原生对象插入 null。
 * @param value JSON 对象。
 * @param key 字段名称。
 * @return 对象内部字段引用，重新分配成员存储后可能失效。
 * @throws std::domain_error 当前值不是对象。
 */
Json &json_at(Json &value, std::string_view key);
/**
 * 读取必需的数组元素。
 * @param value JSON 数组。
 * @param index 零基下标。
 * @return 与数组共享生命周期的元素引用。
 * @throws std::exception 当前值不是数组或下标越界。
 */
const Json &json_at(const Json &value, size_t index);
/**
 * 取得可写数组元素。
 * @param value JSON 数组。
 * @param index 零基下标。
 * @return 数组内部元素的引用。
 * @throws std::exception 当前值不是数组或下标越界。
 */
Json &json_at(Json &value, size_t index);
/**
 * 检查对象是否包含指定字段，不插入任何成员。
 * @param value 待检查的 JSON 值。
 * @param key 字段名称。
 * @return 存在该对象字段时为 true；非对象返回 false。
 */
bool json_contains(const Json &value, std::string_view key);

/**
 * 从原生 JSON 中严格提取框架需要的 C++ 值。
 * @tparam T Json、string、bool、整数、浮点或 vector<string>。
 * @param value 待转换值；数值转整数要求无小数且在目标范围内。
 * @return 独立的 C++ 值，不返回临时字符串视图。
 * @throws std::domain_error JSON 类型不符合要求。
 * @throws std::out_of_range 整数无法放入目标类型。
 */
template <class T> T json_get(const Json &value) {
    if constexpr (std::is_same_v<T, Json>) {
        return value;
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto text = value.asString();
        if (!text) {
            throw std::domain_error("Expected JSON string");
        }
        return std::string(*text);
    } else if constexpr (std::is_same_v<T, bool>) {
        auto boolean = value.asBool();
        if (!boolean) {
            throw std::domain_error("Expected JSON boolean");
        }
        return *boolean;
    } else if constexpr (std::is_integral_v<T>) {
        auto integer = value.asInt();
        if (!integer) {
            throw std::domain_error("Expected JSON integer");
        }
        // 先按符号分类校验边界，再转换，避免负数被转换成很大的 size_t。
        if constexpr (std::is_unsigned_v<T>) {
            if (*integer < 0 || static_cast<std::uint64_t>(*integer) > (std::numeric_limits<T>::max)()) {
                throw std::out_of_range("JSON integer outside target range");
            }
        } else {
            if (*integer < (std::numeric_limits<T>::min)() || *integer > (std::numeric_limits<T>::max)()) {
                throw std::out_of_range("JSON integer outside target range");
            }
        }
        return static_cast<T>(*integer);
    } else if constexpr (std::is_floating_point_v<T>) {
        auto number = value.asDouble();
        if (!number) {
            throw std::domain_error("Expected JSON number");
        }
        return static_cast<T>(*number);
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        std::vector<std::string> result;
        for (const auto &element : value.requireArray()) {
            result.push_back(json_get<std::string>(element));
        }
        return result;
    } else {
        static_assert(!sizeof(T), "Unsupported JSON conversion");
    }
}
/**
 * 读取可选字段，只有字段缺失时使用默认值；类型错误仍然失败。
 * @tparam T json_get 支持的目标类型。
 * @param value 对象值。
 * @param key 可选字段名称。
 * @param fallback 缺失时返回的默认值。
 * @return 字段转换结果或默认值副本。
 * @throws std::exception 对象或已存在字段类型无效。
 */
template <class T> T json_value(const Json &value, std::string_view key, T fallback) {
    (void)value.requireObject();
    const auto *member = value.find(key);
    return member ? json_get<T>(*member) : std::move(fallback);
}
/**
 * 为字符串字面量默认值提供 string 类型的可选字段读取。
 * @param value 对象值。
 * @param key 字段名称。
 * @param fallback 字符串默认值。
 * @return 已存在的文本或默认文本副本。
 * @throws std::exception 已存在字段不是字符串或输入不是对象。
 */
inline std::string json_value(const Json &value, std::string_view key, const char *fallback) {
    return json_value(value, key, std::string(fallback));
}
} // namespace embed_ai_chat
