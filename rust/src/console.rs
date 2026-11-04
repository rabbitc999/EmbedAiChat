//! 独立控制台宿主演示；实际宿主只需引用 Engine。
use crate::{ensure, Engine, McpClient, Result, Tool};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::io::{BufRead, BufReader, IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::{Arc, Mutex};

/// 读取最大 4 MiB 的 UTF-8 JSON 对象文件，返回解析或 I/O 错误。
fn read_object(path: &Path) -> Result<Value> {
    ensure(
        std::fs::metadata(path).map_err(|e| e.to_string())?.len() <= 4 * 1024 * 1024,
        "File too large",
    )?;
    let data = std::fs::read(path).map_err(|e| e.to_string())?;
    ensure(data.len() <= 4 * 1024 * 1024, "File too large")?;
    let result: Value = serde_json::from_slice(&data).map_err(|e| e.to_string())?;
    ensure(result.is_object(), "Expected JSON object")?;
    Ok(result)
}
/// 读取可选文本配置，已有字段类型不正确时返回错误。
fn setting<'a>(cfg: &'a Value, key: &str, fallback: &'a str) -> Result<&'a str> {
    cfg.get(key)
        .map(|v| {
            v.as_str()
                .ok_or_else(|| format!("Invalid config field: {key}"))
        })
        .unwrap_or(Ok(fallback))
}
/// 先写同目录临时文件，再替换完整归档；不会持久化模型凭据。
fn save(path: &Path, value: &Value) -> Result<()> {
    let temp = PathBuf::from(format!("{}.tmp", path.display()));
    std::fs::write(&temp, value.to_string()).map_err(|e| e.to_string())?;
    std::fs::rename(temp, path).map_err(|e| e.to_string())
}
/// 读取当前会话密钥；真实终端隐藏，重定向 stdin 不回显输入。
fn read_key(input: &mut impl BufRead) -> Result<String> {
    if std::io::stdin().is_terminal() {
        print!("API Key (hidden): ");
        std::io::stdout().flush().map_err(|e| e.to_string())?;
        return rpassword::read_password().map_err(|e| e.to_string());
    }
    let mut line = String::new();
    input.read_line(&mut line).map_err(|e| e.to_string())?;
    Ok(line.trim_end_matches(['\r', '\n']).to_owned())
}
/// 运行与其他版本一致的控制台命令；返回启动或单次模型请求错误。
pub fn run(args: &[String]) -> Result<()> {
    let mut engine = Engine::default();
    let state = Rc::new(RefCell::new(json!({"title":"Demo host","value":0})));
    // 示例仅通过三个带 schema 的回调访问内存宿主，实际接入时替换这些回调。
    let host = state.clone();
    engine.register_tool(Tool {
        name: "host_get_state".into(),
        description: "Read demo host state".into(),
        schema: json!({"type":"object","additionalProperties":false}),
        handler: Box::new(move |_| Ok(host.borrow().clone())),
    })?;
    let host = state.clone();
    engine.register_tool(Tool{name:"host_set_title".into(),description:"Set demo host title".into(),schema:json!({"type":"object","properties":{"title":{"type":"string","minLength":1,"maxLength":120}},"required":["title"],"additionalProperties":false}),handler:Box::new(move|a|{host.borrow_mut()["title"]=a["title"].clone();Ok(host.borrow().clone())})})?;
    let host = state.clone();
    engine.register_tool(Tool{name:"host_set_value".into(),description:"Set demo host numeric value".into(),schema:json!({"type":"object","properties":{"value":{"type":"number","minimum":0,"maximum":100}},"required":["value"],"additionalProperties":false}),handler:Box::new(move|a|{host.borrow_mut()["value"]=a["value"].clone();Ok(host.borrow().clone())})})?;
    let mut clients = Vec::new();
    let mut configuration = String::new();
    let mut ask = String::new();
    let mut index = 0;
    while index < args.len() {
        ensure(
            (args[index] == "--config" || args[index] == "--ask") && index + 1 < args.len(),
            "Usage: EmbedAiChatRust [--config file] [--ask text]",
        )?;
        if args[index] == "--config" {
            configuration = args[index + 1].clone();
        } else {
            ask = args[index + 1].clone();
        }
        index += 2;
    }
    if !configuration.is_empty() {
        let full = std::path::absolute(&configuration).map_err(|e| e.to_string())?;
        let cfg = read_object(&full)?;
        ensure(
            cfg.get("key").is_none() && cfg.get("api_key").is_none(),
            "Use /key, not config credentials",
        )?;
        engine.config.provider = setting(&cfg, "provider", "openai")?.into();
        engine.config.base_url = setting(&cfg, "base_url", "https://api.openai.com/v1")?.into();
        engine.config.model = setting(&cfg, "model", "")?.into();
        if let Some(raw) = cfg.get("max_rounds") {
            engine.config.max_rounds = raw
                .as_u64()
                .ok_or("Invalid max_rounds")?
                .try_into()
                .map_err(|_| "max_rounds overflow")?;
        }
        if let Some(raw) = cfg.get("compression_threshold") {
            engine.config.compression_threshold = raw
                .as_u64()
                .ok_or("Invalid compression_threshold")?
                .try_into()
                .map_err(|_| "compression_threshold overflow")?;
        }
        if let Some(raw) = cfg.get("max_summary_tokens") {
            engine.config.max_summary_tokens = raw
                .as_u64()
                .ok_or("Invalid max_summary_tokens")?
                .try_into()
                .map_err(|_| "max_summary_tokens overflow")?;
        }
        if let Some(raw) = cfg.get("max_tokens") {
            engine.config.max_tokens = raw
                .as_u64()
                .ok_or("Invalid max_tokens")?
                .try_into()
                .map_err(|_| "max_tokens overflow")?;
        }
        if let Some(raw) = cfg.get("auto_compact") {
            engine.config.auto_compact = raw.as_bool().ok_or("Invalid auto_compact")?;
        }
        if let Some(raw) = cfg.get("context_budget") {
            engine.config.context_budget = raw
                .as_u64()
                .ok_or("Invalid context budget")?
                .try_into()
                .map_err(|_| "Context budget overflow")?;
        }
        if let Some(raw) = cfg.get("allow_tools") {
            engine.allow_tools = raw.as_bool().ok_or("Invalid allow_tools")?;
        }
        if let Some(skills) = cfg.get("skills") {
            for skill in skills.as_array().ok_or("Invalid skills")? {
                let path = full
                    .parent()
                    .ok_or("No config directory")?
                    .join(skill.as_str().ok_or("Invalid skill path")?);
                engine.load_skill(&path)?;
            }
        }
        engine.activate_skill(setting(&cfg, "active_skill", "")?)?;
        if let Some(servers) = cfg.get("mcp") {
            for server in servers.as_array().ok_or("Invalid MCP config")? {
                let command: Vec<String> =
                    serde_json::from_value(server["command"].clone()).map_err(|e| e.to_string())?;
                let timeout = server
                    .get("timeout_ms")
                    .map(|v| v.as_u64().ok_or("Invalid MCP timeout"))
                    .transpose()?
                    .unwrap_or(15000);
                let client = Arc::new(Mutex::new(McpClient::connect(&command, timeout)?));
                clients.push(client.clone());
                engine.attach_mcp(
                    server["prefix"].as_str().ok_or("Missing MCP prefix")?,
                    client,
                )?;
            }
        }
    }
    if !ask.is_empty() {
        engine.send_with_events(&ask, &mut |event| {
            if event["type"] == "assistant" {
                println!("{}", event["text"].as_str().unwrap_or(""));
            }
            Ok(())
        })?;
        println!(
            "{}",
            json!({"host":state.borrow().clone(),"history":engine.export_history()})
        );
        return Ok(());
    }
    println!("EmbedAiChat Rust 控制台；/help 查看命令，/key 输入会话密钥。");
    let mut input = BufReader::new(std::io::stdin());
    loop {
        print!("> ");
        std::io::stdout().flush().map_err(|e| e.to_string())?;
        let mut line = String::new();
        if input.read_line(&mut line).map_err(|e| e.to_string())? == 0 {
            break;
        }
        let line = line.trim_end_matches(['\r', '\n']);
        if line == "/exit" {
            break;
        }
        let result = (|| -> Result<()> {
            match line {
                "/help" => {
                    println!("/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n/mcp <prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n/save <文件> | /load <文件> | /clear | /audit | /exit");
                    return Ok(());
                }
                "/key" => {
                    engine.config.key = read_key(&mut input)?;
                    return Ok(());
                }
                "/tools" => {
                    println!("{}", json!(engine.tools()));
                    return Ok(());
                }
                "/host" => {
                    println!("{}", state.borrow());
                    return Ok(());
                }
                "/audit" => {
                    println!("{}", engine.export_history()["audit"]);
                    return Ok(());
                }
                "/clear" => {
                    engine.clear();
                    return Ok(());
                }
                _ => {}
            }
            if let Some(v) = line.strip_prefix("/allow ") {
                ensure(v == "on" || v == "off", "Use /allow on|off")?;
                engine.allow_tools = v == "on";
                return Ok(());
            }
            if let Some(path) = line.strip_prefix("/load-skill ") {
                println!("{}", engine.load_skill(Path::new(path))?);
                return Ok(());
            }
            if let Some(name) = line.strip_prefix("/skill ") {
                return engine.activate_skill(name);
            }
            if let Some(path) = line.strip_prefix("/save ") {
                return save(Path::new(path), &engine.export_history());
            }
            if let Some(path) = line.strip_prefix("/load ") {
                return engine.import_history(&read_object(Path::new(path))?);
            }
            if line.starts_with("/mcp ") || line.starts_with("/call ") {
                let mcp = line.starts_with("/mcp ");
                let rest = &line[if mcp { 5 } else { 6 }..];
                let (name, raw) = rest
                    .split_once(' ')
                    .ok_or("Supply name and JSON arguments")?;
                let args: Value = serde_json::from_str(raw).map_err(|e| e.to_string())?;
                if mcp {
                    let argv: Vec<String> =
                        serde_json::from_value(args).map_err(|e| e.to_string())?;
                    let client = Arc::new(Mutex::new(McpClient::connect(&argv, 15000)?));
                    clients.push(client.clone());
                    engine.attach_mcp(name, client)?;
                } else {
                    println!("{}", engine.invoke(name, &args)?);
                }
                return Ok(());
            }
            ensure(!line.starts_with('/'), "Unknown command; /help")?;
            if !line.is_empty() {
                engine.send_with_events(line, &mut |event| {
                    if event["type"] == "assistant" {
                        println!("{}", event["text"].as_str().unwrap_or(""));
                    }
                    Ok(())
                })?;
            }
            Ok(())
        })();
        if let Err(error) = result {
            eprintln!("Error: {error}");
        }
    }
    Ok(())
}
