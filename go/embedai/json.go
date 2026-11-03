// Package embedai 提供独立的 AI 聊天、MCP、Skill 与宿主白名单控制。
// Engine 的配置与聊天接口必须串行使用；宿主回调在 Send 的调用线程执行。
package embedai

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"reflect"
	"unicode/utf8"
)

// Object 是通用 JSON 对象；数字解码为 json.Number，避免历史中的整数先转为 float64。
type Object = map[string]any

const maximumBytes = 4 * 1024 * 1024

// Parse 解析一份 UTF-8 标准 JSON；超限、额外正文或语法无效时返回错误。
func Parse(data []byte) (any, error) {
	if len(data) > maximumBytes || !utf8.Valid(data) {
		return nil, fmt.Errorf("Invalid JSON size or UTF-8")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		return nil, err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return nil, fmt.Errorf("Trailing JSON input")
	}
	return value, nil
}

// jsonText 仅序列化框架已验证的 JSON 数据；公开边界另外检查 Marshal 错误。
func jsonText(value any) string { data, _ := json.Marshal(value); return string(data) }

// copyJSON 返回独立的树副本，避免宿主修改归档或工具返回对象影响后续消息。
func copyJSON(value any) any {
	decoder := json.NewDecoder(bytes.NewBufferString(jsonText(value)))
	decoder.UseNumber()
	var result any
	_ = decoder.Decode(&result)
	return result
}

// object 读取动态对象；类型不符返回 nil，由调用边界负责明确拒绝。
func object(value any) Object { result, _ := value.(map[string]any); return result }

// array 读取动态数组；类型不符返回 nil，不能据此把无效类型当成有效数组。
func array(value any) []any { result, _ := value.([]any); return result }

// text 读取字符串；仅用于允许默认值的字段，必需字段在调用处检查类型。
func text(value any) string { result, _ := value.(string); return result }

// numeric 统一读取解码数字和宿主回调中的 Go 数字，拒绝 bool、NaN 与 Infinity。
func numeric(value any) (float64, bool) {
	var number float64
	switch v := value.(type) {
	case json.Number:
		var err error
		number, err = v.Float64()
		if err != nil {
			return 0, false
		}
	case float64:
		number = v
	case float32:
		number = float64(v)
	case int:
		number = float64(v)
	case int64:
		number = float64(v)
	case uint64:
		number = float64(v)
	default:
		return 0, false
	}
	return number, !math.IsNaN(number) && !math.IsInf(number, 0)
}

// equalJSON 按 JSON 值比较 enum 成员；数字 1 与 1.0 采用相同语义。
func equalJSON(a, b any) bool {
	if x, ok := numeric(a); ok {
		y, yes := numeric(b)
		return yes && x == y
	}
	if x, ok := a.(map[string]any); ok {
		y, yes := b.(map[string]any)
		if !yes || len(x) != len(y) {
			return false
		}
		for k, v := range x {
			peer, exists := y[k]
			if !exists || !equalJSON(v, peer) {
				return false
			}
		}
		return true
	}
	if x, ok := a.([]any); ok {
		y, yes := b.([]any)
		if !yes || len(x) != len(y) {
			return false
		}
		for i := range x {
			if !equalJSON(x[i], y[i]) {
				return false
			}
		}
		return true
	}
	return reflect.DeepEqual(a, b)
}

// Validate 校验参数及受支持的 JSON Schema 子集。未知约束或不匹配返回错误。
// 支持 type、anyOf、enum、对象/数组约束、数值范围、长度与字段数量。
func Validate(schema, value any, path string) error {
	if enabled, ok := schema.(bool); ok {
		if !enabled {
			return fmt.Errorf("%s rejected by schema", path)
		}
		return nil
	}
	s := object(schema)
	if s == nil {
		return fmt.Errorf("Invalid schema")
	}
	supported := map[string]bool{}
	for _, key := range []string{"type", "properties", "required", "additionalProperties", "items", "anyOf", "enum", "minimum", "maximum", "minItems", "maxItems", "minLength", "maxLength", "minProperties", "maxProperties", "description", "title", "default", "$schema", "examples"} {
		supported[key] = true
	}
	for key := range s {
		if !supported[key] {
			return fmt.Errorf("Unsupported schema keyword: %s", key)
		}
	}
	if branches, exists := s["anyOf"]; exists {
		matched := false
		for _, branch := range array(branches) {
			if Validate(branch, value, path) == nil {
				matched = true
				break
			}
		}
		if !matched {
			return fmt.Errorf("%s does not match anyOf", path)
		}
	}
	if choices, exists := s["enum"]; exists {
		found := false
		for _, choice := range array(choices) {
			if equalJSON(value, choice) {
				found = true
			}
		}
		if !found {
			return fmt.Errorf("%s invalid enum", path)
		}
	}
	if raw, exists := s["type"]; exists {
		kind, ok := raw.(string)
		if !ok {
			return fmt.Errorf("Unsupported schema type")
		}
		valid := false
		switch kind {
		case "object":
			_, valid = value.(map[string]any)
		case "array":
			_, valid = value.([]any)
		case "string":
			_, valid = value.(string)
		case "boolean":
			_, valid = value.(bool)
		case "null":
			valid = value == nil
		case "number":
			_, valid = numeric(value)
		case "integer":
			n, ok := numeric(value)
			valid = ok && math.Floor(n) == n
		}
		if !valid {
			return fmt.Errorf("%s expected %s", path, kind)
		}
	}
	if n, ok := numeric(value); ok {
		if v, exists := s["minimum"]; exists {
			limit, yes := numeric(v)
			if !yes || n < limit {
				return fmt.Errorf("%s below minimum", path)
			}
		}
		if v, exists := s["maximum"]; exists {
			limit, yes := numeric(v)
			if !yes || n > limit {
				return fmt.Errorf("%s above maximum", path)
			}
		}
	}
	if v, ok := value.(string); ok {
		if err := bounds(s, utf8.RuneCountInString(v), "Length", path); err != nil {
			return err
		}
	}
	if v, ok := value.([]any); ok {
		if err := bounds(s, len(v), "Items", path); err != nil {
			return err
		}
		if items, exists := s["items"]; exists {
			for _, item := range v {
				if err := Validate(items, item, path+"[]"); err != nil {
					return err
				}
			}
		}
	}
	if v, ok := value.(map[string]any); ok {
		if err := bounds(s, len(v), "Properties", path); err != nil {
			return err
		}
		for _, key := range array(s["required"]) {
			if _, exists := v[text(key)]; !exists {
				return fmt.Errorf("%s missing %s", path, text(key))
			}
		}
		properties := object(s["properties"])
		for key, item := range v {
			rule, exists := properties[key]
			if !exists {
				rule, exists = s["additionalProperties"]
			}
			if exists {
				if err := Validate(rule, item, path+"."+key); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// bounds 检查数量上下限，suffix 指定 Length、Items 或 Properties。
func bounds(schema Object, count int, suffix, path string) error {
	for _, prefix := range []string{"min", "max"} {
		if raw, exists := schema[prefix+suffix]; exists {
			limit, ok := numeric(raw)
			if !ok || (prefix == "min" && float64(count) < limit) || (prefix == "max" && float64(count) > limit) {
				return fmt.Errorf("%s outside %s%s", path, prefix, suffix)
			}
		}
	}
	return nil
}
