using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace EmbedAi;
/// <summary>内部 JSON 辅助方法；使用深拷贝防止 JsonNode 被多个父节点重复持有。</summary>
internal static class J
{
    internal const int MaximumBytes = 4 * 1024 * 1024;
    private static readonly JsonSerializerOptions Options = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    };
    /// <summary>构造对象并克隆已有 JsonNode。</summary>
    /// <param name = "pairs">交替的键和值。</param>
    /// <returns>独立对象。</returns>
    internal static JsonObject O(params object? [] pairs)
    {
        var result = new JsonObject();
        for (var i = 0; i < pairs.Length; i += 2)
        {
            result[(string)pairs[i]!] = Node(pairs[i + 1]);
        }

        return result;
    }

    /// <summary>构造独立 JSON 数组。</summary>
    /// <param name = "values">成员值。</param>
    /// <returns>独立数组。</returns>
    internal static JsonArray A(params object? [] values)
    {
        var result = new JsonArray();
        foreach (var value in values)
        {
            result.Add(Node(value));
        }

        return result;
    }

    /// <summary>把宿主值转换为节点；现有节点深拷贝。</summary>
    /// <param name = "value">宿主值。</param>
    /// <returns>可独立持有的节点。</returns>
    private static JsonNode? Node(object? value)
    {
        return value is JsonNode node ? node.DeepClone() : JsonSerializer.SerializeToNode(value, Options);
    }

    /// <summary>紧凑序列化，保留 UTF-8 中文便于控制台阅读。</summary>
    /// <param name = "value">JSON 值。</param>
    /// <returns>标准 JSON 文本。</returns>
    internal static string Dump(JsonNode? value)
    {
        return value?.ToJsonString(Options) ?? "null";
    }

    /// <summary>严格解析大小受限的标准 JSON。</summary>
    /// <param name = "text">UTF-8 文本。</param>
    /// <returns>JSON 根节点。</returns>
    /// <exception cref = "InvalidDataException">正文超限。</exception>
    internal static JsonNode? Parse(string text)
    {
        if (Encoding.UTF8.GetByteCount(text) > MaximumBytes)
        {
            throw new InvalidDataException("JSON exceeds 4 MiB");
        }

        return JsonNode.Parse(text, documentOptions: new JsonDocumentOptions { MaxDepth = 256 });
    }

    /// <summary>读取必需对象。</summary>
    /// <param name = "value">JSON 值。</param>
    /// <returns>对象节点。</returns>
    /// <exception cref = "InvalidDataException">类型错误。</exception>
    internal static JsonObject Obj(JsonNode? value)
    {
        return value as JsonObject ?? throw new InvalidDataException("Expected JSON object");
    }

    /// <summary>读取必需数组。</summary>
    /// <param name = "value">JSON 值。</param>
    /// <returns>数组节点。</returns>
    /// <exception cref = "InvalidDataException">类型错误。</exception>
    internal static JsonArray Arr(JsonNode? value)
    {
        return value as JsonArray ?? throw new InvalidDataException("Expected JSON array");
    }

    /// <summary>读取字符串，null 使用调用方给定默认文本。</summary>
    /// <param name = "value">字符串节点。</param>
    /// <param name = "fallback">null 默认文本。</param>
    /// <returns>文本。</returns>
    internal static string Text(JsonNode? value, string fallback = "")
    {
        return value is null ? fallback : value.GetValue<string>();
    }

    /// <summary>统一读取 JSON 数值，拒绝布尔、NaN 与 Infinity。</summary>
    /// <param name = "value">待检查值。</param>
    /// <param name = "number">成功时的数值。</param>
    /// <returns>是否为有限数值。</returns>
    internal static bool Number(JsonNode? value, out double number)
    {
        number = 0;
        return value?.GetValueKind() == JsonValueKind.Number && double.TryParse(value.ToJsonString(), NumberStyles.Float, CultureInfo.InvariantCulture, out number) && double.IsFinite(number);
    }

    /// <summary>按 JSON 数值语义比较 enum 或 RPC ID。</summary>
    /// <param name = "a">左值。</param>
    /// <param name = "b">右值。</param>
    /// <returns>是否相等。</returns>
    internal static bool Equal(JsonNode? a, JsonNode? b)
    {
        if (Number(a, out var x) && Number(b, out var y))
        {
            return x == y;
        }

        return JsonNode.DeepEquals(a, b);
    }

    /// <summary>检查前置条件。</summary>
    /// <param name = "ok">条件。</param>
    /// <param name = "error">错误说明。</param>
    /// <exception cref = "InvalidDataException">条件不成立。</exception>
    internal static void Require(bool ok, string error)
    {
        if (!ok)
        {
            throw new InvalidDataException(error);
        }
    }
}

/// <summary>工具 JSON Schema 子集校验器；未知约束明确失败。</summary>
public static class Schema
{
    /// <summary>检查参数，支持基本类型、对象/数组、anyOf、enum、范围和长度约束。</summary>
    /// <param name = "schema">Schema 对象或布尔值。</param>
    /// <param name = "value">参数值。</param>
    /// <param name = "path">诊断路径。</param>
    /// <exception cref = "InvalidDataException">参数或约束无效。</exception>
    public static void Validate(JsonNode? schema, JsonNode? value, string path = "$")
    {
        if (schema?.GetValueKind() == JsonValueKind.True || schema?.GetValueKind() == JsonValueKind.False)
        {
            J.Require(schema.GetValue<bool>(), path + " rejected by schema");
            return;
        }

        var s = J.Obj(schema);
        var supported = new HashSet<string>("type properties required additionalProperties items anyOf enum minimum maximum minItems maxItems minLength maxLength minProperties maxProperties description title default $schema examples".Split(' '));
        foreach (var pair in s)
        {
            J.Require(supported.Contains(pair.Key), "Unsupported schema keyword: " + pair.Key);
        }

        if (s.ContainsKey("anyOf"))
        {
            var matched = false;
            foreach (var option in J.Arr(s["anyOf"]))
            {
                try
                {
                    Validate(option, value, path);
                    matched = true;
                    break;
                }
                catch (InvalidDataException)
                {
                }
            }

            J.Require(matched, path + " does not match anyOf");
        }

        if (s.ContainsKey("enum"))
        {
            J.Require(J.Arr(s["enum"]).Any(item => J.Equal(item, value)), path + " invalid enum");
        }

        if (s.ContainsKey("type"))
        {
            var type = J.Text(s["type"]);
            var matches = type switch
            {
                "object" => value is JsonObject,
                "array" => value is JsonArray,
                "string" => value?.GetValueKind() == JsonValueKind.String,
                "boolean" => value?.GetValueKind()is JsonValueKind.True or JsonValueKind.False,
                "null" => value is null,
                "number" => J.Number(value, out _),
                "integer" => J.Number(value, out var n) && Math.Floor(n) == n,
                _ => false
            };
            J.Require(matches, path + " expected " + type);
        }

        if (J.Number(value, out var number))
        {
            if (s.ContainsKey("minimum"))
            {
                J.Require(J.Number(s["minimum"], out var min) && number >= min, path + " below minimum");
            }

            if (s.ContainsKey("maximum"))
            {
                J.Require(J.Number(s["maximum"], out var max) && number <= max, path + " above maximum");
            }
        }

        if (value?.GetValueKind() == JsonValueKind.String)
        {
            Bounds(s, J.Text(value).EnumerateRunes().Count(), "Length", path);
        }

        if (value is JsonArray array)
        {
            Bounds(s, array.Count, "Items", path);
            if (s.ContainsKey("items"))
            {
                foreach (var item in array)
                {
                    Validate(s["items"], item, path + "[]");
                }
            }
        }

        if (value is JsonObject obj)
        {
            Bounds(s, obj.Count, "Properties", path);
            if (s["required"] is JsonArray keys)
            {
                foreach (var key in keys)
                {
                    J.Require(obj.ContainsKey(J.Text(key)), path + " missing " + J.Text(key));
                }
            }

            var properties = s["properties"] as JsonObject;
            foreach (var pair in obj)
            {
                if (properties?.ContainsKey(pair.Key) == true)
                {
                    Validate(properties[pair.Key], pair.Value, path + "." + pair.Key);
                }
                else if (s.ContainsKey("additionalProperties"))
                {
                    Validate(s["additionalProperties"], pair.Value, path + "." + pair.Key);
                }
            }
        }
    }

    /// <summary>检查数量上下限。</summary>
    /// <param name = "schema">约束对象。</param>
    /// <param name = "count">数量。</param>
    /// <param name = "suffix">Length/Items/Properties。</param>
    /// <param name = "path">诊断路径。</param>
    private static void Bounds(JsonObject schema, int count, string suffix, string path)
    {
        if (schema.ContainsKey("min" + suffix))
        {
            J.Require(J.Number(schema["min" + suffix], out var min) && count >= min, path + " below min" + suffix);
        }

        if (schema.ContainsKey("max" + suffix))
        {
            J.Require(J.Number(schema["max" + suffix], out var max) && count <= max, path + " above max" + suffix);
        }
    }
}
