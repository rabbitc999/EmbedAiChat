"""有状态 MCP fixture：后续调用必须使用前一次执行才生成的 ticket。"""
import json
import secrets
import sys
import time

if "--hang" in sys.argv:
    time.sleep(60)

sys.stdin.reconfigure(encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8")
tickets = set()
empty = {"type": "object", "additionalProperties": False}

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request.get("method")
    if method == "initialize":
        result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}}, "serverInfo": {"name": "workflow-fixture", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [
            {"name": "issue_ticket", "description": "Generate and return a new unpredictable ticket. No arguments.", "inputSchema": empty},
            {"name": "confirm_ticket", "description": "Confirm a ticket returned by issue_ticket and return a new receipt.", "inputSchema": {"type": "object", "properties": {"ticket": {"type": "string"}}, "required": ["ticket"], "additionalProperties": False}},
            {"name": "fail", "description": "Return a deterministic tool execution error.", "inputSchema": empty},
        ]}
        if "--invalid-schema" in sys.argv:
            result["tools"][1]["inputSchema"] = {"type": "object", "$ref": "unsupported"}
        if "--cycle" in sys.argv:
            result["nextCursor"] = "repeated-cursor"
    elif method == "tools/call":
        name = request["params"]["name"]
        if name == "issue_ticket":
            ticket = "ticket_" + secrets.token_hex(12)
            tickets.add(ticket)
            data = {"ticket": ticket}
            result = {"content": [{"type": "text", "text": json.dumps(data)}], "structuredContent": data}
        elif name == "confirm_ticket" and request["params"]["arguments"].get("ticket") in tickets:
            ticket = request["params"]["arguments"]["ticket"]
            tickets.remove(ticket)
            data = {"ticket": ticket, "receipt": "receipt_" + secrets.token_hex(12)}
            result = {"content": [{"type": "text", "text": json.dumps(data)}], "structuredContent": data}
        else:
            result = {"isError": True, "content": [{"type": "text", "text": "fixture failure: invalid ticket or requested failure"}]}
    else:
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {"code": -32601, "message": "Unknown method"}}), flush=True)
        continue
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
