package embedai

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
	"time"
)

// McpClient 拥有一个 stdio MCP 会话。所有调用串行；超时关闭会话且绝不重试操作。
type McpClient struct {
	command  *exec.Cmd
	input    io.WriteCloser
	output   *bufio.Reader
	timeout  time.Duration
	sequence int64
	mutex    sync.Mutex
	once     sync.Once
	closed   bool
}

// NewMcpClient 使用独立 argv 启动服务并完成握手；timeoutMs 为 1–60000 毫秒。
// 返回已握手客户端；失败清理自身启动的进程。命令不会经过 shell。
func NewMcpClient(argv []string, timeoutMs int) (*McpClient, error) {
	if len(argv) == 0 || timeoutMs < 1 || timeoutMs > 60000 {
		return nil, fmt.Errorf("Invalid MCP command/timeout")
	}
	command := exec.Command(argv[0], argv[1:]...)
	configureProcess(command)
	command.Stderr = os.Stderr
	input, err := command.StdinPipe()
	if err != nil {
		return nil, err
	}
	output, err := command.StdoutPipe()
	if err != nil {
		input.Close()
		return nil, err
	}
	if err = command.Start(); err != nil {
		input.Close()
		output.Close()
		return nil, err
	}
	c := &McpClient{command: command, input: input, output: bufio.NewReader(output), timeout: time.Duration(timeoutMs) * time.Millisecond}
	result, err := c.rpc("initialize", Object{"protocolVersion": "2025-11-25", "capabilities": Object{}, "clientInfo": Object{"name": "EmbedAiChat", "version": "0.1.0"}})
	if err == nil {
		version := text(result["protocolVersion"])
		if version != "2025-11-25" && version != "2025-06-18" && version != "2024-11-05" {
			err = fmt.Errorf("Unsupported negotiated MCP version")
		}
	}
	if err == nil {
		err = c.write(Object{"jsonrpc": "2.0", "method": "notifications/initialized", "params": Object{}})
	}
	if err != nil {
		c.Close()
		return nil, err
	}
	return c, nil
}

// Close 幂等关闭本客户端的进程树与管道，释放等待中的读取任务。
func (c *McpClient) Close() {
	c.once.Do(func() { c.closed = true; killProcess(c.command); c.input.Close(); _ = c.command.Wait() })
}

// write 写一条 UTF-8 JSON-RPC 消息，限制包含换行后的总大小为 32 KiB。
func (c *McpClient) write(message Object) error {
	data, err := json.Marshal(message)
	if err != nil {
		return err
	}
	if len(data)+1 > 32768 {
		return fmt.Errorf("MCP request exceeds 32 KiB")
	}
	_, err = c.input.Write(append(data, '\n'))
	return err
}

// readLine 逐段累积一条消息，对未遇到换行的缓冲同样设限。
func (c *McpClient) readLine() (Object, error) {
	var data []byte
	for {
		part, err := c.output.ReadSlice('\n')
		if len(data)+len(part) > maximumBytes {
			return nil, fmt.Errorf("MCP line exceeds 4 MiB")
		}
		data = append(data, part...)
		if err == bufio.ErrBufferFull {
			continue
		}
		if err != nil {
			return nil, err
		}
		break
	}
	value, err := Parse(data)
	if err != nil {
		return nil, err
	}
	result := object(value)
	if result == nil {
		return nil, fmt.Errorf("Invalid MCP message")
	}
	return result, nil
}

// rpc 在同一个期限内写入并等待匹配 ID，处理插入的服务端通知和不支持的请求。
func (c *McpClient) rpc(method string, params Object) (Object, error) {
	c.mutex.Lock()
	defer c.mutex.Unlock()
	if c.closed {
		return nil, fmt.Errorf("MCP connection closed")
	}
	c.sequence++
	id := c.sequence
	type answer struct {
		value Object
		err   error
	}
	done := make(chan answer, 1)
	go func() {
		err := c.write(Object{"jsonrpc": "2.0", "id": id, "method": method, "params": params})
		if err != nil {
			done <- answer{err: err}
			return
		}
		for {
			reply, err := c.readLine()
			if err != nil {
				done <- answer{err: err}
				return
			}
			if text(reply["jsonrpc"]) != "2.0" {
				done <- answer{err: fmt.Errorf("Invalid MCP JSON-RPC")}
				return
			}
			if _, exists := reply["method"]; exists {
				if requestID, yes := reply["id"]; yes {
					if err = c.write(Object{"jsonrpc": "2.0", "id": requestID, "error": Object{"code": -32601, "message": "Client feature unsupported"}}); err != nil {
						done <- answer{err: err}
						return
					}
				}
				continue
			}
			if !equalJSON(reply["id"], id) {
				done <- answer{err: fmt.Errorf("Mismatched MCP response ID")}
				return
			}
			if problem, exists := reply["error"]; exists {
				done <- answer{err: fmt.Errorf("MCP RPC error: %s", jsonText(problem))}
				return
			}
			result := object(reply["result"])
			if result == nil {
				done <- answer{err: fmt.Errorf("Invalid MCP result")}
				return
			}
			done <- answer{value: result}
			return
		}
	}()
	timer := time.NewTimer(c.timeout)
	defer timer.Stop()
	select {
	case result := <-done:
		if result.err != nil {
			c.Close()
		}
		return result.value, result.err
	case <-timer.C:
		c.Close()
		return nil, fmt.Errorf("MCP timed out; outcome unknown; inspect host before another edit")
	}
}

// Tools 分页发现所有工具，重复名称、循环游标或超过 100 页时返回错误。
func (c *McpClient) Tools() ([]any, error) {
	result := []any{}
	params := Object{}
	names := map[string]bool{}
	cursors := map[string]bool{}
	for page := 0; page < 100; page++ {
		reply, err := c.rpc("tools/list", params)
		if err != nil {
			return nil, err
		}
		items, ok := reply["tools"].([]any)
		if !ok {
			return nil, fmt.Errorf("Invalid MCP tools")
		}
		for _, item := range items {
			name := text(object(item)["name"])
			if name == "" || names[name] {
				return nil, fmt.Errorf("Duplicate/invalid MCP tool")
			}
			names[name] = true
			result = append(result, item)
		}
		cursor, exists := reply["nextCursor"]
		if !exists {
			return result, nil
		}
		key, ok := cursor.(string)
		if !ok || cursors[key] {
			return nil, fmt.Errorf("Repeated/invalid MCP cursor")
		}
		cursors[key] = true
		params = Object{"cursor": key}
	}
	return nil, fmt.Errorf("Too many MCP pages")
}

// Call 调用服务端原始工具名；返回包含 content、structuredContent 或 isError 的原始结果。
func (c *McpClient) Call(name string, args Object) (any, error) {
	return c.rpc("tools/call", Object{"name": name, "arguments": args})
}
