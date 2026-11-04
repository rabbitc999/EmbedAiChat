package io.embedai;

import static io.embedai.EmbedAiChat.*;

import com.google.gson.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** 可直接运行的控制台宿主演示。真实宿主只需 Engine，不依赖此入口。 配置不保存密钥；所有宿主控制通过显式注册的三个回调实现。 */
public final class Console {

  /** 阻止控制台入口工具类被实例化。 */
  private Console() {}

  /**
   * 启动独立控制台。
   *
   * @param args --config 无密钥JSON配置；--ask 单次请求（适用于本地测试服务）
   * @throws Exception 启动配置无效或一次性聊天失败
   */
  public static void main(String[] args) throws Exception {
    System.setOut(new PrintStream(System.out, true, StandardCharsets.UTF_8));
    System.setErr(new PrintStream(System.err, true, StandardCharsets.UTF_8));
    Engine engine = new Engine();
    JsonObject state = object("title", "Demo host", "value", 0);
    // 宿主适配段：替换下列回调，保留 schema 与白名单边界即可对接实际程序。
    engine.registerTool(
        new Tool(
            "host_get_state",
            "Read demo host state",
            object("type", "object", "additionalProperties", false),
            a -> state.deepCopy()));
    engine.registerTool(
        new Tool(
            "host_set_title",
            "Set demo host title",
            object(
                "type",
                "object",
                "properties",
                object("title", object("type", "string", "minLength", 1, "maxLength", 120)),
                "required",
                array("title"),
                "additionalProperties",
                false),
            a -> {
              state.add("title", a.get("title"));
              return state.deepCopy();
            }));
    engine.registerTool(
        new Tool(
            "host_set_value",
            "Set demo host numeric value",
            object(
                "type",
                "object",
                "properties",
                object("value", object("type", "number", "minimum", 0, "maximum", 100)),
                "required",
                array("value"),
                "additionalProperties",
                false),
            a -> {
              state.add("value", a.get("value"));
              return state.deepCopy();
            }));
    // 控制台持有自己创建的连接，退出或启动失败时由 finally 统一关闭。
    List<McpClient> clients = new ArrayList<>();
    String config = "", ask = "";
    for (int i = 0; i < args.length; i++) {
      if (args[i].equals("--config") && i + 1 < args.length) {
        config = args[++i];
      } else if (args[i].equals("--ask") && i + 1 < args.length) {
        ask = args[++i];
      } else {
        throw new IllegalArgumentException("Usage: Console [--config file] [--ask text]");
      }
    }
    // 配置路径相对配置文件解析 Skill；MCP argv 由用户显式提供，从不交给 shell。
    try {
      if (!config.isEmpty()) {
        Path path = Path.of(config).toAbsolutePath();
        JsonObject c =
            JsonParser.parseString(Files.readString(path, StandardCharsets.UTF_8))
                .getAsJsonObject();
        if (c.has("key") || c.has("api_key")) {
          throw new IllegalArgumentException("Do not put keys in config; use /key");
        }
        engine.config.provider = get(c, "provider", "openai");
        engine.config.baseUrl = get(c, "base_url", "https://api.openai.com/v1");
        engine.config.model = get(c, "model", "");
        if (c.has("compression_threshold")) {
          engine.config.compressionThreshold = c.get("compression_threshold").getAsInt();
        }
        if (c.has("max_summary_tokens")) {
          engine.config.maxSummaryTokens = c.get("max_summary_tokens").getAsInt();
        }
        if (c.has("max_rounds")) {
          engine.config.maxRounds = c.get("max_rounds").getAsInt();
        }
        if (c.has("max_tokens")) {
          engine.config.maxTokens = c.get("max_tokens").getAsInt();
        }
        if (c.has("auto_compact")) {
          engine.config.autoCompact = c.get("auto_compact").getAsBoolean();
        }
        if (c.has("context_budget")) {
          engine.config.contextBudget = c.get("context_budget").getAsInt();
        }
        if (c.has("allow_tools")) {
          engine.allowTools = c.get("allow_tools").getAsBoolean();
        }
        if (c.has("skills")) {
          for (JsonElement s : c.getAsJsonArray("skills")) {
            engine.loadSkill(path.getParent().resolve(s.getAsString()));
          }
        }
        engine.activateSkill(get(c, "active_skill", ""));
        if (c.has("mcp")) {
          for (JsonElement item : c.getAsJsonArray("mcp")) {
            JsonObject server = item.getAsJsonObject();
            McpClient client =
                new McpClient(
                    argv(server.getAsJsonArray("command")),
                    server.has("timeout_ms") ? server.get("timeout_ms").getAsInt() : 15000);
            clients.add(client);
            engine.attachMcp(server.get("prefix").getAsString(), client);
          }
        }
      }
      if (!ask.isEmpty()) {
        engine.send(ask, event -> { if (event.get("type").getAsString().equals("assistant")) System.out.println(event.get("text").getAsString()); });
        System.out.println(object("host", state, "history", engine.exportHistory()));
        return;
      }
      System.out.println("EmbedAiChat Java 控制台；/help 查看命令，/key 输入会话密钥。");
      BufferedReader input =
          new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
      String line;
      while (true) {
        System.out.print("> ");
        line = input.readLine();
        if (line == null || line.equals("/exit")) {
          break;
        }
        try {
          if (line.equals("/help")) {
            System.out.println(
                "/key | /tools | /allow on|off | /load-skill <目录> | /skill <名称>\n"
                    + "/mcp <prefix> <JSON argv数组> | /call <name> <JSON对象> | /host\n"
                    + "/save <文件> | /load <文件> | /clear | /audit | /exit\n"
                    + "模型连接通过 --config 配置；普通文字直接发送。");
            continue;
          }
          if (line.equals("/key")) {
            java.io.Console terminal = System.console();
            if (isTerminal(terminal)) {
              char[] key = terminal.readPassword("API Key (hidden): ");
              engine.config.key = key == null ? "" : new String(key);
              // 临时字符数组用完立即清空；会话所需的密钥仅保留在引擎配置内。
              if (key != null) {
                Arrays.fill(key, '\0');
              }
            } else {
              System.out.println("API Key (stdin redirected, do not log input):");
              engine.config.key = Objects.requireNonNullElse(input.readLine(), "");
            }
            continue;
          }
          if (line.equals("/tools")) {
            System.out.println(engine.tools());
            continue;
          }
          if (line.equals("/host")) {
            System.out.println(state);
            continue;
          }
          if (line.equals("/audit")) {
            System.out.println(engine.exportHistory().get("audit"));
            continue;
          }
          if (line.equals("/clear")) {
            engine.clear();
            continue;
          }
          if (line.startsWith("/allow ")) {
            String v = line.substring(7);
            if (!Set.of("on", "off").contains(v)) {
              throw new IllegalArgumentException("Use /allow on|off");
            }
            engine.allowTools = v.equals("on");
            continue;
          }
          if (line.startsWith("/load-skill ")) {
            System.out.println(engine.loadSkill(Path.of(line.substring(12))));
            continue;
          }
          if (line.startsWith("/skill ")) {
            engine.activateSkill(line.substring(7));
            continue;
          }
          if (line.startsWith("/save ")) {
            save(Path.of(line.substring(6)), engine.exportHistory());
            continue;
          }
          if (line.startsWith("/load ")) {
            engine.importHistory(
                JsonParser.parseString(
                        Files.readString(Path.of(line.substring(6)), StandardCharsets.UTF_8))
                    .getAsJsonObject());
            continue;
          }
          if (line.startsWith("/mcp ") || line.startsWith("/call ")) {
            boolean mcp = line.startsWith("/mcp ");
            String rest = line.substring(mcp ? 5 : 6);
            int split = rest.indexOf(' ');
            if (split < 0) {
              throw new IllegalArgumentException("Supply name and JSON arguments");
            }
            String name = rest.substring(0, split);
            JsonElement a = JsonParser.parseString(rest.substring(split + 1));
            if (mcp) {
              McpClient client = new McpClient(argv(a.getAsJsonArray()), 15000);
              clients.add(client);
              engine.attachMcp(name, client);
            } else {
              System.out.println(engine.invoke(name, a.getAsJsonObject()));
            }
            continue;
          }
          if (line.startsWith("/")) {
            throw new IllegalArgumentException("Unknown command; /help");
          }
          if (!line.isEmpty()) {
            engine.send(line, event -> { if (event.get("type").getAsString().equals("assistant")) System.out.println(event.get("text").getAsString()); });
          }
        } catch (Exception error) {
          System.err.println("Error: " + error.getMessage());
        }
      }
    } finally {
      // 包含正常退出和异常退出两种路径，避免 stdio 服务继续驻留。
      clients.forEach(McpClient::close);
    }
  }

  // JDK 22 会为重定向流返回非空 Console；反射检查保持 JDK 17 源码兼容。
  /**
   * 兼容不同 JDK 判断输入是否来自真实终端。
   *
   * @param console 系统提供的 Console，允许为 null。
   * @return 存在真实终端时为 true；反射检查失败时为 false。
   */
  private static boolean isTerminal(java.io.Console console) {
    if (console == null) {
      return false;
    }
    try {
      return (boolean) java.io.Console.class.getMethod("isTerminal").invoke(console);
    } catch (NoSuchMethodException olderJdk) {
      return true;
    } catch (ReflectiveOperationException failure) {
      return false;
    }
  }

  /**
   * 读取可选的配置文本字段。
   *
   * @param o 配置 JSON 对象。
   * @param key 字段名称。
   * @param fallback 字段不存在时的默认文本。
   * @return 配置字段文本或默认文本。
   */
  private static String get(JsonObject o, String key, String fallback) {
    return o.has(key) ? o.get(key).getAsString() : fallback;
  }

  /**
   * 把 JSON 参数数组转换成独立的进程参数列表。
   *
   * @param a 由用户明确提供的可执行文件及参数数组。
   * @return 保持输入顺序、未经 shell 解释的参数列表。
   */
  private static List<String> argv(JsonArray a) {
    List<String> values = new ArrayList<>();
    a.forEach(v -> values.add(v.getAsString()));
    return values;
  }

  // 先写同目录临时文件，再替换归档，避免正常中断留下半截 JSON。
  /**
   * 通过同目录临时文件完整替换聊天归档。
   *
   * @param target 目标文件路径。
   * @param value 由引擎导出的历史对象。
   * @throws IOException 写文件或移动替换失败。
   */
  private static void save(Path target, JsonObject value) throws IOException {
    Path temporary = target.resolveSibling(target.getFileName() + ".tmp");
    Files.writeString(
        temporary,
        new GsonBuilder().setPrettyPrinting().create().toJson(value),
        StandardCharsets.UTF_8);
    try {
      Files.move(
          temporary, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
      // 部分文件系统不支持原子移动；仅在该情况退回完整文件的替换操作。
    } catch (AtomicMoveNotSupportedException e) {
      Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING);
    }
  }
}
