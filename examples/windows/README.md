# Native Windows psmux runner

[`07-windows-psmux.cpp`](../07-windows-psmux.cpp) reads a psmux session through
the library. The runner,
[`scripts/psmux/psmux_example.py`](../../scripts/psmux/psmux_example.py), gives
the example or the MCP server a high-entropy `-L` namespace, an empty
configuration file created for that run, and no inherited tmux or psmux routing
variables. Then it cleans up:

- It kills only the exact fixture session, waits for that session's registry
  files to disappear, then removes the configuration file. If a step fails, the
  error names the socket, session and config path to recover.
- It never runs psmux session cleanup before a launch attempt, never runs
  `kill-server`, and never deletes the `.psmux` directory.

`Server::new_session` refuses psmux, because concurrent creators cannot prove
ownership, so the runner creates the one session with `tmux.exe` and kills it
the same way. Everything else the example does goes through the library.

Build the `windows-psmux` preset from a Developer PowerShell, then run the
native example through the fixture. The runner uses only the Python standard
library:

```console
$ uv run scripts/psmux/psmux_example.py --example build/windows-psmux/examples/windows/Debug/libtmux_example_07_windows_psmux.exe
```

The same fixture can host the MCP server. This invocation passes the generated
selector explicitly as `--socket-name`; stdin and stdout remain the MCP
transport, and cleanup runs after the client closes stdin:

```console
$ uv run scripts/psmux/psmux_example.py --mcp-server build/windows-psmux/apps/mcp/Debug/libtmux-mcp-server.exe
```

An MCP client can use that command directly. Resolve both paths first: MCP
clients commonly launch from a different working directory, and relying on a
relative path would make the trusted runner or binary depend on that client.

```json
{
  "mcpServers": {
    "tmux": {
      "command": "uv",
      "args": [
        "run",
        "C:\\src\\libtmux-cxx\\scripts\\psmux\\psmux_example.py",
        "--mcp-server",
        "C:\\src\\libtmux-cxx\\build\\windows-psmux\\apps\\mcp\\Debug\\libtmux-mcp-server.exe"
      ]
    }
  }
}
```

The runner's empty `PSMUX_CONFIG_FILE` is the explicit trusted configuration
for the fixture it creates. A private socket name alone is not enough: psmux
expands aliases from server configuration and scans its shared registry before
it parses `-L`. Use the verified binary named in
[`docs/windows.md`](../../docs/windows.md), and do not point this runner at a
pre-existing namespace.

## Tests

The runner and the MCP smoke test have pytest suites that use stand-in programs,
so they run without psmux:

```console
$ uv run --project scripts --locked pytest scripts/psmux
```
