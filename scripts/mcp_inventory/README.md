# MCP README inventory

Keeps the tool table in [`apps/mcp/README.md`](../../apps/mcp/README.md) equal
to what the server advertises. The script starts a built `libtmux-mcp-server`
with every toolset on, reads `tmux://capabilities`, and renders the tools
grouped by toolset between the two generated-block markers in the README.

CTest runs it as `consumer.mcp.readme` with `--check`, which fails when the
README differs. Without `--check` it rewrites the block:

```console
$ uv run scripts/mcp_inventory/mcp_inventory.py \
    --server build/cxx-dev/apps/mcp/libtmux-mcp-server \
    --readme apps/mcp/README.md
```

The server starts without a socket selector or tool filter from the caller's
environment, and no command reaches tmux.

## Tests

```console
$ uv run --group dev pytest scripts/mcp_inventory
```

The tests use a stand-in server that echoes the environment it saw.
