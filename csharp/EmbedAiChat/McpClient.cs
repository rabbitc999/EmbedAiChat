using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;

namespace EmbedAi;
/// <summary>拥有一个 stdio MCP 子进程，握手和 RPC 超时后关闭会话，不重试操作。</summary>
public sealed class McpClient : IDisposable
{
    private readonly Process process;
    private readonly SemaphoreSlim gate = new(1, 1);
    private readonly int timeout;
    private long sequence;
    private bool closed;
    /// <summary>启动采用独立 argv 的服务进程。</summary>
    /// <param name = "argv">命令及参数。</param>
    /// <param name = "timeoutMs">1–60000 毫秒。</param>
    private McpClient(IReadOnlyList<string> argv, int timeoutMs)
    {
        J.Require(argv.Count > 0 && timeoutMs is> 0 and <= 60000, "Invalid MCP command/timeout");
        timeout = timeoutMs;
        var info = new ProcessStartInfo(argv[0])
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
            StandardInputEncoding = new UTF8Encoding(false),
            StandardOutputEncoding = new UTF8Encoding(false, true)
        };
        foreach (var arg in argv.Skip(1))
        {
            info.ArgumentList.Add(arg);
        }

        process = Process.Start(info) ?? throw new IOException("Cannot start MCP");
    }

    /// <summary>连接 MCP 并完成 initialize/initialized 握手。</summary>
    /// <param name = "argv">可执行文件及参数。</param>
    /// <param name = "timeoutMs">单次 RPC 超时毫秒。</param>
    /// <returns>已握手客户端。</returns>
    /// <exception cref = "Exception">启动或协议错误，失败清理子进程。</exception>
    public static async Task<McpClient> ConnectAsync(IReadOnlyList<string> argv, int timeoutMs = 15000)
    {
        var client = new McpClient(argv, timeoutMs);
        try
        {
            var init = await client.RpcAsync("initialize", J.O("protocolVersion", "2025-11-25", "capabilities", J.O(), "clientInfo", J.O("name", "EmbedAiChat", "version", "0.1.0"))).ConfigureAwait(false);
            J.Require(new[] { "2025-11-25", "2025-06-18", "2024-11-05" }.Contains(J.Text(init["protocolVersion"])), "Unsupported negotiated MCP version");
            using var deadline = new CancellationTokenSource(timeoutMs);
            await client.WriteAsync(J.O("jsonrpc", "2.0", "method", "notifications/initialized", "params", J.O()), deadline.Token).ConfigureAwait(false);
            return client;
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    /// <summary>写一条不超过 32 KiB 的 JSON-RPC 消息。</summary>
    /// <param name = "message">完整消息。</param>
    /// <param name = "token">期限令牌。</param>
    /// <returns>异步写入。</returns>
    private async Task WriteAsync(JsonObject message, CancellationToken token)
    {
        var line = J.Dump(message);
        J.Require(Encoding.UTF8.GetByteCount(line) + 1 <= 32768, "MCP request exceeds 32 KiB");
        await process.StandardInput.WriteLineAsync(line.AsMemory(), token).ConfigureAwait(false);
        await process.StandardInput.FlushAsync(token).ConfigureAwait(false);
    }

    /// <summary>逐字符构造有界消息行，避免先读取无限长正文再设限。</summary>
    /// <param name = "token">整个 RPC 的期限。</param>
    /// <returns>解析后的协议对象。</returns>
    private async Task<JsonObject> ReadAsync(CancellationToken token)
    {
        var line = new StringBuilder();
        var one = new char[1];
        var bytes = 0;
        while (await process.StandardOutput.ReadAsync(one.AsMemory(), token).ConfigureAwait(false) != 0)
        {
            var c = one[0];
            if (c == '\n')
            {
                return J.Obj(J.Parse(line.ToString()));
            }

            bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : char.IsHighSurrogate(c) ? 1 : 3;
            J.Require(bytes <= J.MaximumBytes, "MCP line exceeds 4 MiB");
            line.Append(c);
        }

        throw new EndOfStreamException("MCP exited");
    }

    /// <summary>串行请求并匹配 ID，处理插入通知；失败关闭连接。</summary>
    /// <param name = "method">方法名称。</param>
    /// <param name = "parameters">请求参数。</param>
    /// <returns>RPC result。</returns>
    private async Task<JsonObject> RpcAsync(string method, JsonObject parameters)
    {
        J.Require(!closed, "MCP connection closed");
        using var deadline = new CancellationTokenSource(timeout);
        await gate.WaitAsync(deadline.Token).ConfigureAwait(false);
        try
        {
            var id = ++sequence;
            await WriteAsync(J.O("jsonrpc", "2.0", "id", id, "method", method, "params", parameters), deadline.Token).ConfigureAwait(false);
            while (true)
            {
                var reply = await ReadAsync(deadline.Token).ConfigureAwait(false);
                J.Require(J.Text(reply["jsonrpc"]) == "2.0", "Invalid MCP JSON-RPC");
                if (reply.ContainsKey("method"))
                {
                    if (reply.ContainsKey("id"))
                    {
                        await WriteAsync(J.O("jsonrpc", "2.0", "id", reply["id"], "error", J.O("code", -32601, "message", "Client feature unsupported")), deadline.Token).ConfigureAwait(false);
                    }

                    continue;
                }

                J.Require(J.Equal(reply["id"], JsonValue.Create(id)), "Mismatched MCP response ID");
                J.Require(!reply.ContainsKey("error"), "MCP RPC error: " + J.Dump(reply["error"]));
                return J.Obj(reply["result"]).DeepClone().AsObject();
            }
        }
        catch (OperationCanceledException error)
        {
            Dispose();
            throw new IOException("MCP timed out; outcome unknown; inspect host before another edit", error);
        }
        catch
        {
            Dispose();
            throw;
        }
        finally
        {
            gate.Release();
        }
    }

    /// <summary>发现全部工具页，拒绝重复名称和循环游标。</summary>
    /// <returns>工具定义数组。</returns>
    public async Task<JsonArray> ToolsAsync()
    {
        var result = new JsonArray();
        var parameters = J.O();
        var cursors = new HashSet<string>();
        var names = new HashSet<string>();
        for (var page = 0; page < 100; page++)
        {
            var reply = await RpcAsync("tools/list", parameters).ConfigureAwait(false);
            foreach (var tool in J.Arr(reply["tools"]))
            {
                J.Require(names.Add(J.Text(J.Obj(tool)["name"])), "Duplicate MCP tool");
                result.Add(tool?.DeepClone());
            }

            if (!reply.ContainsKey("nextCursor"))
            {
                return result;
            }

            var cursor = J.Text(reply["nextCursor"]);
            J.Require(cursors.Add(cursor), "Repeated MCP cursor");
            parameters = J.O("cursor", cursor);
        }

        throw new IOException("Too many MCP tool pages");
    }

    /// <summary>调用原始服务端工具名称，保留工具级错误。</summary>
    /// <param name = "name">工具名。</param>
    /// <param name = "arguments">参数。</param>
    /// <returns>原始 MCP 结果。</returns>
    public Task<JsonObject> CallAsync(string name, JsonObject arguments)
    {
        return RpcAsync("tools/call", J.O("name", name, "arguments", arguments));
    }

    /// <summary>幂等关闭自身启动的进程树，不关闭其他宿主程序。</summary>
    public void Dispose()
    {
        if (closed)
        {
            return;
        }

        closed = true;
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(2000);
            }
        }
        catch (InvalidOperationException)
        {
        }
        finally
        {
            process.Dispose();
        }
    }
}
