# Native Windows psmux runner

The runner gives the example or MCP server a high-entropy `-L` namespace, an
empty configuration file created for that run, and no inherited tmux or psmux
routing variables. After a fixture launch is attempted, it kills only the exact
fixture session and waits for that session's exact registry files to disappear.
It removes the configuration file only after both steps are proven; otherwise
the error reports the exact socket, session, and config path needed for recovery.
It never invokes psmux session cleanup before a launch attempt, never runs
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
for the fixture it creates. Supplying only a private socket name is not enough:
psmux expands aliases from server configuration, and psmux 3.3.7 scans its
shared registry before it parses `-L`. Use the pinned binary documented by the
project, and do not point this wrapper at a pre-existing namespace.
