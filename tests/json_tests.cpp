#include "embed_ai_chat/framework.hpp"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
using namespace embed_ai_chat;
namespace {
/**
 * 检查一个具有具体行为意义的 JSON 断言。
 * @param condition 断言是否成立。
 * @param message 失败时输出的场景说明。
 * @throws std::runtime_error 场景不符合预期。
 */
void expect(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
/**
 * 确认错误输入在进入业务逻辑前失败。
 * @tparam Action 可调用测试动作类型。
 * @param action 应当抛出标准异常的动作。
 * @param message 动作未失败时的场景说明。
 * @throws std::runtime_error 错误输入未被拒绝。
 */
template <class Action> void expect_failure(Action action, const char *message) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }
    expect(failed, message);
}
} // namespace
/**
 * 验证新的 JSON 解析、值类型、边界和归档接口。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    static_assert(std::is_same_v<Json, json::Value>, "Framework must expose the native value type");
    try {
        auto value = json_parse(
            R"({"text":"\u4f60\u597d \ud83d\ude00","flag":true,"nothing":null,"items":[1,2.5],"large":9223372036854775807})");
        expect(json_get<std::string>(json_at(value, "text")) == u8"你好 😀", "Unicode decode");
        expect(json_get<bool>(json_at(value, "flag")), "Boolean decode");
        expect(json_at(value, "nothing").isNull(), "Null decode");
        expect(json_get<double>(json_at(json_at(value, "items"), size_t(1))) == 2.5, "Floating point decode");
        expect(json_get<std::int64_t>(json_at(value, "large")) == INT64_MAX, "64-bit integer precision");
        auto roundtrip = json_parse(value.dump());
        expect(json_get<std::int64_t>(json_at(roundtrip, "large")) == INT64_MAX,
               "Integer serialization precision");
        expect(roundtrip == value, "Nested roundtrip");
        expect(json_get<std::string>(json_parse(R"("a\u0000b")")).size() == 3, "Embedded zero is preserved");
        expect(json_get<double>(json_parse("1.2e-3")) == 0.0012, "Scientific notation");
        // 数字样式文本始终保持 string，不能被读取/校验为 number。
        for (const auto &text : {"1.0", "001", "1e3", "-0.0"}) {
            const Json native(text);
            const auto parsed = json_parse(native.dump());
            expect(parsed.isString() && !parsed.isNumber(), "Numeric-looking string type");
            expect(json_get<std::string>(parsed) == text, "Numeric-looking string contents");
            expect(!parsed.asDouble() && !parsed.asInt(), "No string-to-number coercion");
            expect_failure([&] { json_get<double>(parsed); }, "Number getter rejects string");
            validate(Json::object({{"type", "string"}}), parsed);
            expect_failure([&] { validate(Json::object({{"type", "number"}}), parsed); },
                           "Number schema rejects string");
            expect(parsed != json_parse("1.0"), "String differs from JSON number");
        }
        const auto typed = json_parse(R"({"version":"1.0","value":1.0,"items":["1.0",1.0]})");
        expect(json_at(typed, "version").isString(), "Version text remains string");
        expect(json_at(typed, "value").isNumber(), "Unquoted number remains numeric");
        expect(json_parse(typed.dump()) == typed, "Mixed strings/numbers survive roundtrip");

        expect_failure([] { json_parse("1e999"); }, "Overflowing exponent rejected");
        expect_failure([] { json_parse("-1e999"); }, "Negative overflowing exponent rejected");

        expect(Json::object({}).isObject() && Json::array({}).isArray(), "Empty container types");
        auto ordered = Json::object({{"second", 2}, {"first", 1}});
        expect(ordered.requireObject().begin()->key == "second", "Insertion order");
        expect(json_get<int>(json_at(json_parse(R"({"x":1,"x":2})"), "x")) == 2, "Duplicate key policy");
        expect(json_value(ordered, "missing", 12) == 12, "Missing field default");
        expect(json_get<std::vector<std::string>>(Json::array({"a", "b"})).size() == 2,
               "MCP argv conversion");
        expect_failure([&] { json_get<bool>(Json(1)); }, "Boolean coercion rejected");
        expect_failure([&] { json_get<size_t>(Json(-1)); }, "Negative size rejected");
        expect_failure([&] { json_get<int>(json_at(value, "large")); }, "Integer overflow rejected");
        expect_failure([&] { json_get<int>(Json(1.5)); }, "Fractional integer rejected");
        expect_failure([&] { json_value(ordered, "first", false); }, "Wrong type is not a fallback");
        const auto &read_only = ordered;
        expect_failure([&] { json_at(read_only, "missing"); }, "Required member rejected");
        expect(!json_contains(ordered, "missing"), "Reading missing member did not insert");
        expect_failure([] { json_parse("{\"x\":1,}"); }, "Trailing comma rejected");
        expect_failure([] { json_parse("[1] trailing"); }, "Trailing input rejected");
        expect_failure([] { json_parse("/* comment */ {}"); }, "Comments rejected");
        expect_failure([] { json_parse(R"("\ud800")"); }, "Unpaired high surrogate rejected");
        expect_failure([] { json_parse(R"("\udc00")"); }, "Unpaired low surrogate rejected");
        expect_failure([] { json_parse(std::string("\"") + char(0xff) + "\""); }, "Invalid UTF-8 rejected");
        expect_failure([] { json_parse(std::string(257, '[') + "0" + std::string(257, ']')); },
                       "Parser nesting limit");
        std::istringstream stream(value.dump());
        expect(json_parse(stream) == value, "Stream parse");
        expect_failure(
            [] {
                std::istringstream huge(std::string(4 * 1024 * 1024 + 1, ' '));
                json_parse(huge);
            },
            "Stream size limit");
        Engine engine;
        auto archive = Json::object(
            {{"version", 1},
             {"messages", Json::array({Json::object({{"role", "user"}, {"content", u8"历史"}})})},
             {"covered", 0},
             {"summary", ""},
             {"audit", Json::array({})}});
        engine.import_history(archive);
        const auto saved = engine.export_history();
        expect_failure(
            [&] {
                auto bad = archive;
                bad["covered"] = -1;
                engine.import_history(bad);
            },
            "Invalid history coverage");
        expect(engine.export_history() == saved, "Invalid history is atomic");
        std::cout << "PASS: native JSON, Unicode, numeric boundaries, strict parsing and history\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
