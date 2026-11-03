"""严格 JSON 边界和宿主参数校验；仅使用 Python 标准库。"""
import json
import math

MAX_BYTES = 4 * 1024 * 1024


def require(condition, message):
    """检查前置条件，失败抛 ValueError，不进行隐式类型转换。"""
    if not condition:
        raise ValueError(message)


def dumps(value):
    """生成紧凑 UTF-8 JSON；拒绝非有限数值及无效 Unicode。"""
    text = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    text.encode("utf-8")
    return text


def loads(data):
    """解析最多 4 MiB 的严格 JSON，拒绝重复键、NaN/Infinity 和尾随内容。"""
    if isinstance(data, bytes):
        require(len(data) <= MAX_BYTES, "JSON exceeds 4 MiB")
        data = data.decode("utf-8")
    require(isinstance(data, str) and len(data.encode("utf-8")) <= MAX_BYTES, "Invalid JSON size")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON key")
            result[key] = value
        return result

    def constant(_):
        raise ValueError("Non-finite JSON number")

    value = json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    # 重新编码检查指数溢出及转义形式的孤立代理项。
    dumps(value)
    return value


def clone(value):
    """返回独立 JSON 副本；调用者与引擎之间不共享可变节点。"""
    return loads(dumps(value))


def number(value):
    """判断 JSON 数字；Python bool 虽继承 int，在 JSON 中仍是布尔类型。"""
    return type(value) in (int, float) and (not isinstance(value, float) or math.isfinite(value))


def equal(left, right):
    """按 JSON 类型比较枚举值，避免 True 与 1 被 Python 判为相等。"""
    if number(left) and number(right):
        return left == right
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(equal(a, b) for a, b in zip(left, right))
    return left == right


def _check_schema(schema, depth=0):
    """递归预检全部分支；未知关键字不能藏在未匹配的 anyOf 或缺省字段中。"""
    require(isinstance(schema, dict) and depth <= 64, "Invalid schema/depth")
    supported = {"type", "anyOf", "enum", "properties", "required", "additionalProperties", "items",
                 "minimum", "maximum", "minLength", "maxLength", "minItems", "maxItems",
                 "minProperties", "maxProperties", "description", "title", "default"}
    require(not (schema.keys() - supported), "Unsupported schema keyword")
    if "type" in schema:
        require(isinstance(schema["type"], str) and schema["type"] in ("object", "array", "string", "number", "integer", "boolean", "null"), "Unsupported schema type")
    if "anyOf" in schema:
        require(isinstance(schema["anyOf"], list) and schema["anyOf"], "Invalid anyOf")
        for branch in schema["anyOf"]:
            _check_schema(branch, depth + 1)
    if "properties" in schema:
        require(isinstance(schema["properties"], dict), "Invalid properties")
        for branch in schema["properties"].values():
            _check_schema(branch, depth + 1)
    if "items" in schema:
        _check_schema(schema["items"], depth + 1)
    if "required" in schema:
        require(isinstance(schema["required"], list) and all(isinstance(k, str) for k in schema["required"]), "Invalid required")
    if "additionalProperties" in schema:
        require(type(schema["additionalProperties"]) is bool, "Unsupported additionalProperties")
    if "enum" in schema:
        require(isinstance(schema["enum"], list) and schema["enum"], "Invalid enum")
    for key in ("minimum", "maximum", "minLength", "maxLength", "minItems", "maxItems", "minProperties", "maxProperties"):
        if key in schema:
            n = schema[key]
            require(number(n) and (key in ("minimum", "maximum") or n >= 0 and n == int(n)), "Invalid " + key)


def validate(schema, value, path="$", depth=0):
    """校验项目支持的 JSON Schema 子集；未知关键字明确拒绝，不执行外部引用。"""
    if depth == 0:
        _check_schema(schema)
    require(isinstance(schema, dict) and depth <= 64, f"{path}: invalid schema/depth")
    # Schema 已完整预检，下面只检查实际数据，递归时不重复遍历全部定义。
    if "anyOf" in schema:
        matches = False
        for branch in schema["anyOf"]:
            try:
                validate(branch, value, path, depth + 1)
                matches = True
            except ValueError:
                pass
        require(matches, f"{path}: anyOf mismatch")
    types = {"object": isinstance(value, dict), "array": isinstance(value, list), "string": isinstance(value, str),
             "number": number(value), "integer": number(value) and value == int(value),
             "boolean": type(value) is bool, "null": value is None}
    if "type" in schema:
        kind = schema["type"]
        require(types[kind], f"{path}: expected {kind}")
    if "enum" in schema:
        require(any(equal(value, item) for item in schema["enum"]), f"{path}: enum mismatch")
    if number(value):
        for key, ok in (("minimum", value >= schema.get("minimum", value)), ("maximum", value <= schema.get("maximum", value))):
            require(ok, f"{path}: {key}")
    if isinstance(value, (str, list, dict)):
        suffix = "Length" if isinstance(value, str) else "Items" if isinstance(value, list) else "Properties"
        for prefix in ("min", "max"):
            key = prefix + suffix
            if key in schema:
                require(len(value) >= schema[key] if prefix == "min" else len(value) <= schema[key], f"{path}: {key}")
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        require(all(key in value for key in required), f"{path}: required property missing")
        for key, item in value.items():
            if key in properties:
                validate(properties[key], item, f"{path}.{key}", depth + 1)
            else:
                require(schema.get("additionalProperties", True), f"{path}: additionalProperties")
    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            validate(schema["items"], item, f"{path}[{i}]", depth + 1)
