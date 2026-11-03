package embedai

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"golang.org/x/term"
	"os"
	"path/filepath"
	"strings"
)

// readJSONFile 读取受大小约束的配置或归档，拒绝非对象根节点。
func readJSONFile(path string) (Object, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if info.Size() > maximumBytes {
		return nil, fmt.Errorf("File too large")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	value, err := Parse(data)
	if err != nil {
		return nil, err
	}
	result := object(value)
	if result == nil {
		return nil, fmt.Errorf("Expected JSON object")
	}
	return result, nil
}

// saveHistory 先写同目录临时文件，再替换完整归档；不包含模型配置及密钥。
func saveHistory(path string, value Object) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	temporary := path + ".tmp"
	if err = os.WriteFile(temporary, data, 0600); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

// RunConsole 是独立宿主演示入口；实际宿主只需 Engine。args 支持 --config 与 --ask。
// 返回启动或单次请求错误；交互命令错误打印后继续，允许用户检查实际状态。
func RunConsole(args []string) error {
	e := New(nil)
	state := Object{"title": "Demo host", "value": 0}
	ctx := context.Background()
	// 回调模拟真实宿主；返回副本，避免后续修改使已经记录的工具结果发生变化。
	_ = e.RegisterTool(Tool{"host_get_state", "Read demo host state", Object{"type": "object", "additionalProperties": false}, func(Object) (any, error) { return copyJSON(state), nil }})
	_ = e.RegisterTool(Tool{"host_set_title", "Set demo host title", Object{"type": "object", "properties": Object{"title": Object{"type": "string", "minLength": 1, "maxLength": 120}}, "required": []any{"title"}, "additionalProperties": false}, func(a Object) (any, error) { state["title"] = a["title"]; return copyJSON(state), nil }})
	_ = e.RegisterTool(Tool{"host_set_value", "Set demo host numeric value", Object{"type": "object", "properties": Object{"value": Object{"type": "number", "minimum": 0, "maximum": 100}}, "required": []any{"value"}, "additionalProperties": false}, func(a Object) (any, error) { state["value"] = a["value"]; return copyJSON(state), nil }})
	clients := []*McpClient{}
	defer func() {
		for _, c := range clients {
			c.Close()
		}
	}()
	configuration, ask := "", ""
	for i := 0; i < len(args); i++ {
		if (args[i] == "--config" || args[i] == "--ask") && i+1 < len(args) {
			name := args[i]
			i++
			if name == "--config" {
				configuration = args[i]
			} else {
				ask = args[i]
			}
		} else {
			return fmt.Errorf("Usage: EmbedAiChatGo [--config file] [--ask text]")
		}
	}
	if configuration != "" {
		cfg, err := readJSONFile(configuration)
		if err != nil {
			return err
		}
		if _, yes := cfg["key"]; yes {
			return fmt.Errorf("Use /key, not config credentials")
		}
		if _, yes := cfg["api_key"]; yes {
			return fmt.Errorf("Use /key, not config credentials")
		}
		if err = json.Unmarshal([]byte(jsonText(cfg)), &e.Config); err != nil {
			return err
		}
		var options struct {
			Allow  bool     `json:"allow_tools"`
			Skills []string `json:"skills"`
			Active string   `json:"active_skill"`
			MCP    []struct {
				Prefix  string   `json:"prefix"`
				Command []string `json:"command"`
				Timeout *int     `json:"timeout_ms"`
			} `json:"mcp"`
		}
		if err = json.Unmarshal([]byte(jsonText(cfg)), &options); err != nil {
			return err
		}
		e.AllowTools = options.Allow
		base, err := filepath.Abs(configuration)
		if err != nil {
			return err
		}
		for _, skill := range options.Skills {
			if !filepath.IsAbs(skill) {
				skill = filepath.Join(filepath.Dir(base), skill)
			}
			if _, err = e.LoadSkill(skill); err != nil {
				return err
			}
		}
		if err = e.ActivateSkill(options.Active); err != nil {
			return err
		}
		for _, server := range options.MCP {
			timeout := 15000
			if server.Timeout != nil {
				timeout = *server.Timeout
			}
			client, err := NewMcpClient(server.Command, timeout)
			if err != nil {
				return err
			}
			clients = append(clients, client)
			if err = e.AttachMCP(server.Prefix, client); err != nil {
				return err
			}
		}
	}
	if ask != "" {
		answer, err := e.Send(ctx, ask)
		if err != nil {
			return err
		}
		fmt.Println(answer)
		fmt.Println(jsonText(Object{"host": state, "history": e.ExportHistory()}))
		return nil
	}
	fmt.Println("EmbedAiChat Go 控制台；/help 查看命令，/key 输入会话密钥。")
	reader := bufio.NewReader(os.Stdin)
	for {
		fmt.Print("> ")
		line, err := reader.ReadString('\n')
		if err != nil && line == "" {
			return nil
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "/exit" {
			return nil
		}
		commandError := func() error {
			switch line {
			case "/help":
				fmt.Println("/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n/mcp <prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n/save <文件> | /load <文件> | /clear | /audit | /exit")
				return nil
			case "/key":
				if term.IsTerminal(int(os.Stdin.Fd())) {
					fmt.Print("API Key (hidden): ")
					key, err := term.ReadPassword(int(os.Stdin.Fd()))
					fmt.Println()
					if err != nil {
						return err
					}
					e.Config.Key = string(key)
					for i := range key {
						key[i] = 0
					}
				} else {
					key, err := reader.ReadString('\n')
					if err != nil && key == "" {
						return err
					}
					e.Config.Key = strings.TrimRight(key, "\r\n")
				}
				return nil
			case "/tools":
				fmt.Println(jsonText(e.Tools()))
				return nil
			case "/host":
				fmt.Println(jsonText(state))
				return nil
			case "/audit":
				fmt.Println(jsonText(e.audit))
				return nil
			case "/clear":
				e.Clear()
				return nil
			}
			if strings.HasPrefix(line, "/allow ") {
				v := strings.TrimPrefix(line, "/allow ")
				if v != "on" && v != "off" {
					return fmt.Errorf("Use /allow on|off")
				}
				e.AllowTools = v == "on"
				return nil
			}
			if strings.HasPrefix(line, "/load-skill ") {
				name, err := e.LoadSkill(strings.TrimPrefix(line, "/load-skill "))
				if err == nil {
					fmt.Println(name)
				}
				return err
			}
			if strings.HasPrefix(line, "/skill ") {
				return e.ActivateSkill(strings.TrimPrefix(line, "/skill "))
			}
			if strings.HasPrefix(line, "/save ") {
				return saveHistory(strings.TrimPrefix(line, "/save "), e.ExportHistory())
			}
			if strings.HasPrefix(line, "/load ") {
				archive, err := readJSONFile(strings.TrimPrefix(line, "/load "))
				if err != nil {
					return err
				}
				return e.ImportHistory(archive)
			}
			if strings.HasPrefix(line, "/mcp ") || strings.HasPrefix(line, "/call ") {
				mcp := strings.HasPrefix(line, "/mcp ")
				rest := strings.TrimPrefix(line, "/call ")
				if mcp {
					rest = strings.TrimPrefix(line, "/mcp ")
				}
				name, raw, ok := strings.Cut(rest, " ")
				if !ok {
					return fmt.Errorf("Supply name and JSON arguments")
				}
				value, err := Parse([]byte(raw))
				if err != nil {
					return err
				}
				if mcp {
					argv := []string{}
					items, ok := value.([]any)
					if !ok {
						return fmt.Errorf("Expected argv array")
					}
					for _, item := range items {
						s, ok := item.(string)
						if !ok {
							return fmt.Errorf("Expected string argv")
						}
						argv = append(argv, s)
					}
					client, err := NewMcpClient(argv, 15000)
					if err != nil {
						return err
					}
					clients = append(clients, client)
					return e.AttachMCP(name, client)
				}
				if object(value) == nil {
					return fmt.Errorf("Expected argument object")
				}
				result, err := e.Invoke(name, object(value))
				if err == nil {
					fmt.Println(jsonText(result))
				}
				return err
			}
			if strings.HasPrefix(line, "/") {
				return fmt.Errorf("Unknown command; /help")
			}
			if line != "" {
				answer, err := e.Send(ctx, line)
				if err != nil {
					return err
				}
				fmt.Println(answer)
			}
			return nil
		}()
		if commandError != nil {
			fmt.Fprintln(os.Stderr, "Error:", commandError)
		}
	}
}
