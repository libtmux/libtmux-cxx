# psmux fixtures

Runs the Windows psmux example and the MCP server against a psmux session each
run owns, and proves it removed everything it created. psmux reads aliases from
its server configuration and scans a shared registry under the user's profile
before it parses `-L`, so a private socket name is not enough: every run gets an
empty configuration file, no inherited routing variables, and an exact cleanup.

| Script | Used by |
|---|---|
| [`psmux_example.py`](psmux_example.py) | CTest `example.07-windows-psmux`, and anyone hosting the example or the MCP server on psmux by hand; see [`examples/windows/`](../../examples/windows/README.md) |
| [`psmux_mcp_smoke.py`](psmux_mcp_smoke.py) | CTest `windows.mcp.psmux-smoke`: the pinned tool catalog, the capability resource, and four tools that read the fixture |
| [`psmux_fixture.py`](psmux_fixture.py) | Both: the isolated environment, profile and registry lookup, the exact kill and the registry proof |

Neither CI lane starts psmux. They run by hand and in CTest on a Windows build
with psmux installed.

`Server::new_session` refuses psmux, because concurrent creators cannot prove
ownership, so the scripts create the one session with `tmux.exe` and kill it the
same way.

```console
$ uv run scripts/psmux/psmux_mcp_smoke.py \
    --server build/windows-psmux/apps/mcp/Debug/libtmux-mcp-server.exe
```

## Tests

```console
$ uv run --group dev pytest scripts/psmux
```

The tests use stand-ins for `tmux.exe` and the server, so they run on Linux. They
cover a config that already exists, a refused session, a failed kill, registry
files that outlive the session, a server that breaks the contract or never
stops, and an environment that must not leak.
