//! 有明确边界的 JSON Schema 子集校验；不支持的约束不能静默忽略。
use crate::{ensure, Result};
use serde_json::Value;

/// 验证参数；`schema` 为对象或布尔值，`path` 用于错误定位。
/// 返回成功或具体约束错误，不触发任何宿主操作。
pub fn validate(schema: &Value, value: &Value, path: &str) -> Result<()> {
    if let Some(enabled) = schema.as_bool() {
        return ensure(enabled, &format!("{path} rejected by schema"));
    }
    let s = schema.as_object().ok_or("Invalid schema")?;
    let supported = [
        "type",
        "properties",
        "required",
        "additionalProperties",
        "items",
        "anyOf",
        "enum",
        "minimum",
        "maximum",
        "minItems",
        "maxItems",
        "minLength",
        "maxLength",
        "minProperties",
        "maxProperties",
        "description",
        "title",
        "default",
        "$schema",
        "examples",
    ];
    for key in s.keys() {
        ensure(
            supported.contains(&key.as_str()),
            &format!("Unsupported schema keyword: {key}"),
        )?;
    }
    if let Some(options) = s.get("anyOf") {
        let options = options.as_array().ok_or("Invalid anyOf")?;
        ensure(
            options.iter().any(|s| validate(s, value, path).is_ok()),
            &format!("{path} does not match anyOf"),
        )?;
    }
    if let Some(options) = s.get("enum") {
        ensure(
            options
                .as_array()
                .ok_or("Invalid enum")?
                .iter()
                .any(|v| equal(v, value)),
            &format!("{path} invalid enum"),
        )?;
    }
    if let Some(kind) = s.get("type") {
        let kind = kind.as_str().ok_or("Invalid schema type")?;
        let matches = match kind {
            "object" => value.is_object(),
            "array" => value.is_array(),
            "string" => value.is_string(),
            "boolean" => value.is_boolean(),
            "null" => value.is_null(),
            "number" => value.as_f64().is_some_and(f64::is_finite),
            "integer" => value
                .as_f64()
                .is_some_and(|n| n.is_finite() && n.floor() == n),
            _ => false,
        };
        ensure(matches, &format!("{path} expected {kind}"))?;
    }
    if let Some(number) = value.as_f64() {
        if let Some(min) = s.get("minimum") {
            ensure(
                number >= min.as_f64().ok_or("Invalid minimum")?,
                &format!("{path} below minimum"),
            )?;
        }
        if let Some(max) = s.get("maximum") {
            ensure(
                number <= max.as_f64().ok_or("Invalid maximum")?,
                &format!("{path} above maximum"),
            )?;
        }
    }
    if let Some(text) = value.as_str() {
        bounds(schema, text.chars().count(), "Length", path)?;
    }
    if let Some(items) = value.as_array() {
        bounds(schema, items.len(), "Items", path)?;
        if let Some(rule) = s.get("items") {
            for item in items {
                validate(rule, item, &format!("{path}[]"))?;
            }
        }
    }
    if let Some(object) = value.as_object() {
        bounds(schema, object.len(), "Properties", path)?;
        if let Some(keys) = s.get("required") {
            for key in keys.as_array().ok_or("Invalid required")? {
                let key = key.as_str().ok_or("Invalid required key")?;
                ensure(object.contains_key(key), &format!("{path} missing {key}"))?;
            }
        }
        for (key, item) in object {
            let rule = s
                .get("properties")
                .and_then(|p| p.get(key))
                .or_else(|| s.get("additionalProperties"));
            if let Some(rule) = rule {
                validate(rule, item, &format!("{path}.{key}"))?;
            }
        }
    }
    Ok(())
}
/// 按 JSON 数值语义比较 RPC ID 和 enum；对象与数组递归比较。
pub(crate) fn equal(a: &Value, b: &Value) -> bool {
    if let (Some(x), Some(y)) = (a.as_f64(), b.as_f64()) {
        return x == y;
    }
    match (a, b) {
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(a, b)| equal(a, b))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).is_some_and(|other| equal(v, other)))
        }
        _ => a == b,
    }
}
/// 校验数量范围；`suffix` 为 Length、Items 或 Properties，错误包含字段路径。
fn bounds(schema: &Value, count: usize, suffix: &str, path: &str) -> Result<()> {
    if let Some(min) = schema.get(format!("min{suffix}")) {
        ensure(
            count as f64 >= min.as_f64().ok_or("Invalid size bound")?,
            &format!("{path} below min{suffix}"),
        )?;
    }
    if let Some(max) = schema.get(format!("max{suffix}")) {
        ensure(
            count as f64 <= max.as_f64().ok_or("Invalid size bound")?,
            &format!("{path} above max{suffix}"),
        )?;
    }
    Ok(())
}
