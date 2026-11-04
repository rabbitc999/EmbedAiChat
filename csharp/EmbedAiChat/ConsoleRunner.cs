using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace EmbedAi;
/// <summary>独立控制台宿主演示；真实宿主可只使用 Engine。</summary>
public static class ConsoleRunner
{
    /// <summary>运行共享控制台命令并清理自身创建的 MCP 会话。</summary>
    /// <param name = "args">--config 和 --ask 参数。</param>
    /// <returns>成功 0，启动或单次请求失败 1。</returns>
    public static async Task<int> RunAsync(string[] args)
    {
        Console.InputEncoding = new UTF8Encoding(false);
        Console.OutputEncoding = new UTF8Encoding(false);
        var clients = new List<McpClient>();
        try
        {
            var e = new Engine();
            var state = J.O("title", "Demo host", "value", 0);
            // 只注册示例宿主的白名单能力；返回实际状态副本。
            e.RegisterTool(new Tool("host_get_state", "Read demo host state", J.O("type", "object", "additionalProperties", false), _ => Task.FromResult<JsonNode?>(state.DeepClone())));
            e.RegisterTool(new Tool("host_set_title", "Set demo host title", J.O("type", "object", "properties", J.O("title", J.O("type", "string", "minLength", 1, "maxLength", 120)), "required", J.A("title"), "additionalProperties", false), a =>
            {
                state["title"] = a["title"]?.DeepClone();
                return Task.FromResult<JsonNode?>(state.DeepClone());
            }));
            e.RegisterTool(new Tool("host_set_value", "Set demo host numeric value", J.O("type", "object", "properties", J.O("value", J.O("type", "number", "minimum", 0, "maximum", 100)), "required", J.A("value"), "additionalProperties", false), a =>
            {
                state["value"] = a["value"]?.DeepClone();
                return Task.FromResult<JsonNode?>(state.DeepClone());
            }));
            var config = "";
            var ask = "";
            for (var i = 0; i < args.Length; i++)
            {
                J.Require((args[i] == "--config" || args[i] == "--ask") && i + 1 < args.Length, "Usage: EmbedAiChatCSharp [--config file] [--ask text]");
                var name = args[i++];
                if (name == "--config")
                {
                    config = args[i];
                }
                else
                {
                    ask = args[i];
                }
            }

            if (config.Length > 0)
            {
                var path = Path.GetFullPath(config);
                var cfg = ReadObject(path);
                J.Require(!cfg.ContainsKey("key") && !cfg.ContainsKey("api_key"), "Use /key, not config credentials");
                e.Config.Provider = J.Text(cfg["provider"], "openai");
                e.Config.BaseUrl = J.Text(cfg["base_url"], "https://api.openai.com/v1");
                e.Config.Model = J.Text(cfg["model"]);
                if (cfg.ContainsKey("compression_threshold"))
                {
                    e.Config.CompressionThreshold = cfg["compression_threshold"]!.GetValue<int>();
                }
                if (cfg.ContainsKey("max_summary_tokens"))
                {
                    e.Config.MaxSummaryTokens = cfg["max_summary_tokens"]!.GetValue<int>();
                }
                if (cfg.ContainsKey("max_rounds"))
                {
                    e.Config.MaxRounds = cfg["max_rounds"]!.GetValue<int>();
                }
                if (cfg.ContainsKey("max_tokens"))
                {
                    e.Config.MaxTokens = cfg["max_tokens"]!.GetValue<int>();
                }
                if (cfg.ContainsKey("auto_compact"))
                {
                    e.Config.AutoCompact = cfg["auto_compact"]!.GetValue<bool>();
                }
                if (cfg.ContainsKey("context_budget"))
                {
                    e.Config.ContextBudget = cfg["context_budget"]!.GetValue<int>();
                }

                e.AllowTools = cfg["allow_tools"]?.GetValue<bool>() ?? false;
                if (cfg["skills"] is JsonArray skills)
                {
                    foreach (var skill in skills)
                    {
                        e.LoadSkill(Path.Combine(Path.GetDirectoryName(path)!, J.Text(skill)));
                    }
                }

                e.ActivateSkill(J.Text(cfg["active_skill"]));
                if (cfg["mcp"] is JsonArray servers)
                {
                    foreach (var item in servers)
                    {
                        var server = J.Obj(item);
                        var client = await McpClient.ConnectAsync(Arguments(J.Arr(server["command"])), server["timeout_ms"]?.GetValue<int>() ?? 15000).ConfigureAwait(false);
                        clients.Add(client);
                        await e.AttachMcpAsync(J.Text(server["prefix"]), client).ConfigureAwait(false);
                    }
                }
            }

            if (ask.Length > 0)
            {
                await e.SendWithEventsAsync(ask, evt => { if (evt["type"]!.GetValue<string>() == "assistant") Console.WriteLine(evt["text"]!.GetValue<string>()); return Task.CompletedTask; }).ConfigureAwait(false);
                Console.WriteLine(J.Dump(J.O("host", state, "history", e.ExportHistory())));
                return 0;
            }

            Console.WriteLine("EmbedAiChat C# 控制台；/help 查看命令，/key 输入会话密钥。");
            while (true)
            {
                Console.Write("> ");
                var line = Console.ReadLine();
                if (line is null || line == "/exit")
                {
                    return 0;
                }

                try
                {
                    switch (line)
                    {
                        case "/help":
                            Console.WriteLine("/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n/mcp <prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n/save <文件> | /load <文件> | /clear | /audit | /exit");
                            continue;
                        case "/key":
                            e.Config.Key = ReadKey();
                            continue;
                        case "/tools":
                            Console.WriteLine(J.Dump(e.Tools()));
                            continue;
                        case "/host":
                            Console.WriteLine(J.Dump(state));
                            continue;
                        case "/audit":
                            Console.WriteLine(J.Dump(e.ExportHistory()["audit"]));
                            continue;
                        case "/clear":
                            e.Clear();
                            continue;
                    }

                    if (line.StartsWith("/allow ", StringComparison.Ordinal))
                    {
                        var v = line[7..];
                        J.Require(v is "on" or "off", "Use /allow on|off");
                        e.AllowTools = v == "on";
                        continue;
                    }

                    if (line.StartsWith("/load-skill ", StringComparison.Ordinal))
                    {
                        Console.WriteLine(e.LoadSkill(line[12..]));
                        continue;
                    }

                    if (line.StartsWith("/skill ", StringComparison.Ordinal))
                    {
                        e.ActivateSkill(line[7..]);
                        continue;
                    }

                    if (line.StartsWith("/save ", StringComparison.Ordinal))
                    {
                        Save(line[6..], e.ExportHistory());
                        continue;
                    }

                    if (line.StartsWith("/load ", StringComparison.Ordinal))
                    {
                        e.ImportHistory(ReadObject(line[6..]));
                        continue;
                    }

                    if (line.StartsWith("/mcp ", StringComparison.Ordinal) || line.StartsWith("/call ", StringComparison.Ordinal))
                    {
                        var mcp = line.StartsWith("/mcp ", StringComparison.Ordinal);
                        var rest = line[(mcp ? 5 : 6)..];
                        var split = rest.IndexOf(' ');
                        J.Require(split > 0, "Supply name and JSON arguments");
                        var name = rest[..split];
                        var value = J.Parse(rest[(split + 1)..]);
                        if (mcp)
                        {
                            var client = await McpClient.ConnectAsync(Arguments(J.Arr(value))).ConfigureAwait(false);
                            clients.Add(client);
                            await e.AttachMcpAsync(name, client).ConfigureAwait(false);
                        }
                        else
                        {
                            Console.WriteLine(J.Dump(await e.InvokeAsync(name, J.Obj(value)).ConfigureAwait(false)));
                        }

                        continue;
                    }

                    J.Require(!line.StartsWith('/'), "Unknown command; /help");
                    if (line.Length > 0)
                    {
                        await e.SendWithEventsAsync(line, evt => { if (evt["type"]!.GetValue<string>() == "assistant") Console.WriteLine(evt["text"]!.GetValue<string>()); return Task.CompletedTask; }).ConfigureAwait(false);
                    }
                }
                catch (Exception error)
                {
                    Console.Error.WriteLine("Error: " + error.Message);
                }
            }
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("Error: " + error.Message);
            return 1;
        }
        finally
        {
            foreach (var client in clients)
            {
                client.Dispose();
            }
        }
    }

    /// <summary>读取有界 JSON 文件。</summary>
    /// <param name = "path">配置或归档路径。</param>
    /// <returns>对象根节点。</returns>
    private static JsonObject ReadObject(string path)
    {
        J.Require(new FileInfo(path).Length <= J.MaximumBytes, "File too large");
        return J.Obj(J.Parse(File.ReadAllText(path, new UTF8Encoding(false, true))));
    }

    /// <summary>提取严格字符串 argv，不执行 shell 展开。</summary>
    /// <param name = "array">参数数组。</param>
    /// <returns>有序参数。</returns>
    private static string[] Arguments(JsonArray array)
    {
        return array.Select(v => v?.GetValue<string>() ?? throw new InvalidDataException("Null argv")).ToArray();
    }

    /// <summary>真实终端隐藏密钥，重定向输入不回显原文。</summary>
    /// <returns>当前会话密钥。</returns>
    private static string ReadKey()
    {
        if (Console.IsInputRedirected)
        {
            return Console.ReadLine() ?? "";
        }

        Console.Write("API Key (hidden): ");
        var value = new StringBuilder();
        while (true)
        {
            var key = Console.ReadKey(intercept: true);
            if (key.Key == ConsoleKey.Enter)
            {
                Console.WriteLine();
                return value.ToString();
            }

            if (key.Key == ConsoleKey.Backspace)
            {
                if (value.Length > 0)
                {
                    value.Length--;
                }
            }
            else if (!char.IsControl(key.KeyChar))
            {
                value.Append(key.KeyChar);
            }
        }
    }

    /// <summary>写同目录临时归档，完成后替换目标。</summary>
    /// <param name = "path">归档路径。</param>
    /// <param name = "archive">完整历史。</param>
    private static void Save(string path, JsonObject archive)
    {
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, J.Dump(archive), new UTF8Encoding(false));
        File.Move(temporary, path, overwrite: true);
    }
}
