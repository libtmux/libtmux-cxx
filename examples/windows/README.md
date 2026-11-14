# Native Windows psmux runner

The runner gives the example or MCP server a high-entropy `-L` namespace, an
empty configuration file created for that run, and no inherited tmux or psmux
routing variables. Then it cleans up:

- It kills only the exact fixture session, waits for that session's registry
  files to disappear, then removes the configuration file. If a step fails, the
  error names the socket, session and config path to recover.
- It never runs psmux session cleanup before a launch attempt, never runs
  `kill-server`, and never deletes the `.psmux` directory.

Build the `windows-psmux` preset from a Developer PowerShell, then run the
native example through the fixture:

```console
$ powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\examples\windows\run_psmux_example.ps1 -Example .\build\windows-psmux\examples\windows\Debug\libtmux_example_07_windows_psmux.exe
```

The same fixture can host the MCP server. This invocation passes the generated
selector explicitly as `--socket-name`; stdin and stdout remain the MCP
transport, and cleanup runs after the client closes stdin:

```console
$ powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\examples\windows\run_psmux_example.ps1 -McpServer .\build\windows-psmux\apps\mcp\Debug\libtmux-mcp-server.exe
```

An MCP client can use that command directly. Resolve both paths first: MCP
clients commonly launch from a different working directory, and relying on a
relative path would make the trusted wrapper or binary depend on that client.

```json
{
  "mcpServers": {
    "tmux": {
      "command": "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
      "args": [
        "-NoProfile",
        "-File",
        "C:\\src\\libtmux-cxx\\examples\\windows\\run_psmux_example.ps1",
        "-McpServer",
        "C:\\src\\libtmux-cxx\\build\\windows-psmux\\apps\\mcp\\Debug\\libtmux-mcp-server.exe"
      ]
    }
  }
}
```

The wrapper's empty `PSMUX_CONFIG_FILE` is the explicit trusted configuration
for the fixture it creates. A private socket name alone is not enough: psmux
expands aliases from server configuration and scans its shared registry before
it parses `-L`. Use the verified binary named in
[`docs/windows.md`](../../docs/windows.md), and do not point this wrapper at a
pre-existing namespace.
