//! 有界 stdio MCP 传输，超时后关闭会话，绝不自动重试宿主操作。
use crate::{ensure, Result};
use serde_json::{json, Value};
use std::collections::HashSet;
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Sender};
use std::time::Duration;

/// I/O 线程任务；`id=None` 表示无需服务端响应的通知。
struct Task {
    id: Option<u64>,
    method: String,
    params: Value,
    reply: Sender<Result<Value>>,
}
/// 拥有一个 stdio 子进程；所有方法通过 `&mut self` 串行执行。
pub struct McpClient {
    child: Child,
    sender: Option<Sender<Task>>,
    sequence: u64,
    timeout: Duration,
    closed: bool,
}
impl McpClient {
    /// 采用独立 argv 启动服务并握手；`timeout_ms` 必须在 1–60000。
    /// # Errors
    /// 命令、进程或协议错误；失败清理本客户端启动的进程。
    pub fn connect(argv: &[String], timeout_ms: u64) -> Result<Self> {
        ensure(
            !argv.is_empty() && (1..=60000).contains(&timeout_ms),
            "Invalid MCP command/timeout",
        )?;
        let mut command = Command::new(&argv[0]);
        command
            .args(&argv[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit());
        configure(&mut command);
        let mut child = command.spawn().map_err(|e| e.to_string())?;
        let input = child.stdin.take().ok_or("Missing MCP stdin")?;
        let output = child.stdout.take().ok_or("Missing MCP stdout")?;
        let (sender, receiver) = mpsc::channel::<Task>();
        // 独立线程负责阻塞 I/O；所有权留在客户端，超时杀死进程后读写会解除阻塞。
        std::thread::spawn(move || {
            let mut input = input;
            let mut output = BufReader::new(output);
            for task in receiver {
                let result = exchange(&mut input, &mut output, &task);
                let _ = task.reply.send(result);
            }
        });
        let mut client = Self {
            child,
            sender: Some(sender),
            sequence: 0,
            timeout: Duration::from_millis(timeout_ms),
            closed: false,
        };
        let init=client.rpc("initialize",json!({"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"EmbedAiChat","version":"0.1.0"}}))?;
        ensure(
            ["2025-11-25", "2025-06-18", "2024-11-05"]
                .contains(&init["protocolVersion"].as_str().unwrap_or("")),
            "Unsupported negotiated MCP version",
        )?;
        client.perform("notifications/initialized", json!({}), false)?;
        Ok(client)
    }
    /// 把任务提交给唯一 I/O 线程；期限覆盖写入和完整响应等待。
    fn perform(&mut self, method: &str, params: Value, response: bool) -> Result<Value> {
        ensure(!self.closed, "MCP connection closed")?;
        self.sequence += 1;
        let (reply, wait) = mpsc::channel();
        let task = Task {
            id: response.then_some(self.sequence),
            method: method.into(),
            params,
            reply,
        };
        let sent = self
            .sender
            .as_ref()
            .ok_or("MCP connection closed")?
            .send(task);
        let result = if sent.is_err() {
            Err("MCP worker closed".into())
        } else {
            wait.recv_timeout(self.timeout).unwrap_or_else(|_| {
                Err("MCP timed out; outcome unknown; inspect host before another edit".into())
            })
        };
        if result.is_err() {
            self.close();
        }
        result
    }
    /// 发送带请求 ID 的 RPC；不重试失败调用。
    fn rpc(&mut self, method: &str, params: Value) -> Result<Value> {
        self.perform(method, params, true)
    }
    /// 分页发现全部工具；循环游标、重复名称或超过 100 页返回错误。
    pub fn tools(&mut self) -> Result<Vec<Value>> {
        let mut result = Vec::new();
        let mut params = json!({});
        let mut cursors = HashSet::new();
        let mut names = HashSet::new();
        for _ in 0..100 {
            let reply = self.rpc("tools/list", params)?;
            for tool in reply["tools"].as_array().ok_or("Invalid MCP tools")? {
                let name = tool["name"].as_str().ok_or("Invalid MCP tool name")?;
                ensure(names.insert(name.to_owned()), "Duplicate MCP tool")?;
                result.push(tool.clone());
            }
            if let Some(cursor) = reply.get("nextCursor") {
                let cursor = cursor.as_str().ok_or("Invalid cursor")?;
                ensure(cursors.insert(cursor.to_owned()), "Repeated MCP cursor")?;
                params = json!({"cursor":cursor});
            } else {
                return Ok(result);
            }
        }
        Err("Too many MCP pages".into())
    }
    /// 调用服务端原始名称；保留 content、structuredContent 和 isError。
    pub fn call(&mut self, name: &str, args: &Value) -> Result<Value> {
        self.rpc("tools/call", json!({"name":name,"arguments":args}))
    }
    /// 幂等关闭本客户端创建的进程树并释放工作线程通道。
    pub fn close(&mut self) {
        if self.closed {
            return;
        }
        self.closed = true;
        terminate(&mut self.child);
        self.sender.take();
    }
}
impl Drop for McpClient {
    /// RAII 释放连接；不会关闭其他宿主程序。
    fn drop(&mut self) {
        self.close();
    }
}
/// 逐片读取单条消息，尚未遇到换行时也执行 4 MiB 上限。
fn read_message(reader: &mut impl BufRead) -> Result<Value> {
    let mut data = Vec::new();
    loop {
        let part = reader.fill_buf().map_err(|e| e.to_string())?;
        ensure(!part.is_empty(), "MCP exited")?;
        let newline = part.iter().position(|b| *b == b'\n');
        let size = newline.map_or(part.len(), |n| n + 1);
        ensure(
            data.len() + size <= 4 * 1024 * 1024,
            "MCP line exceeds 4 MiB",
        )?;
        data.extend_from_slice(&part[..size]);
        reader.consume(size);
        if newline.is_some() {
            break;
        }
    }
    serde_json::from_slice(&data).map_err(|e| e.to_string())
}
/// 写出单条 JSON-RPC；32 KiB 限制包括协议换行。
fn write_message(writer: &mut impl Write, value: &Value) -> Result<()> {
    let mut data = serde_json::to_vec(value).map_err(|e| e.to_string())?;
    data.push(b'\n');
    ensure(data.len() <= 32768, "MCP request exceeds 32 KiB")?;
    writer
        .write_all(&data)
        .and_then(|_| writer.flush())
        .map_err(|e| e.to_string())
}
/// 在同一工作任务内处理主动消息并等待匹配 ID；不把通知误认为响应。
fn exchange(writer: &mut impl Write, reader: &mut impl BufRead, task: &Task) -> Result<Value> {
    let mut request = json!({"jsonrpc":"2.0","method":task.method,"params":task.params});
    if let Some(id) = task.id {
        request["id"] = json!(id);
    }
    write_message(writer, &request)?;
    let Some(id) = task.id else {
        return Ok(Value::Null);
    };
    loop {
        let reply = read_message(reader)?;
        ensure(reply["jsonrpc"] == "2.0", "Invalid MCP JSON-RPC")?;
        if reply.get("method").is_some() {
            if let Some(id) = reply.get("id") {
                write_message(
                    writer,
                    &json!({"jsonrpc":"2.0","id":id,"error":{"code":-32601,"message":"Client feature unsupported"}}),
                )?;
            }
            continue;
        }
        ensure(
            crate::schema::equal(&reply["id"], &json!(id)),
            "Mismatched MCP response ID",
        )?;
        if let Some(error) = reply.get("error") {
            return Err(format!("MCP RPC error: {error}"));
        }
        let result = reply.get("result").ok_or("Missing MCP result")?;
        ensure(result.is_object(), "Invalid MCP result")?;
        return Ok(result.clone());
    }
}
/// 使用平台进程参数控制窗口或独立进程组，不经过 shell。
fn configure(command: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
}
/// 只清理当前 Child 所属进程树，最后等待直接子进程释放。
fn terminate(child: &mut Child) {
    #[cfg(windows)]
    {
        let mut kill = Command::new("taskkill");
        kill.args(["/PID", &child.id().to_string(), "/T", "/F"])
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        configure(&mut kill);
        let _ = kill.status();
    }
    #[cfg(unix)]
    {
        // SAFETY: 进程组在本客户端启动时创建，child.id 为仍持有的正 PID。
        unsafe {
            libc::kill(-(child.id() as i32), libc::SIGKILL);
        }
    }
    let _ = child.kill();
    let _ = child.wait();
}
