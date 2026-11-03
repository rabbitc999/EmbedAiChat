---
name: demo-host
description: Inspect and edit the EmbedAiChat console demo host title and numeric value through registered tools. Use only for this demo host.
---

# Console demo host

Call `host_get_state` before modifying the demo host. Set titles with `host_set_title` and values in 0–100 with `host_set_value`. Read the returned state before claiming success. These callbacks operate on the in-memory demonstration host.

If a tool times out, inspect the host state before considering another change. A Skill provides instructions; it does not grant access to arbitrary scripts or tools.
