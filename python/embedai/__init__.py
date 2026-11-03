"""EmbedAiChat Python：同步聊天、MCP stdio、Skill 和宿主白名单工具。"""
from .engine import Engine, ModelConfig, Tool, post_json
from .mcp import McpClient

__all__ = ["Engine", "ModelConfig", "Tool", "McpClient", "post_json"]
