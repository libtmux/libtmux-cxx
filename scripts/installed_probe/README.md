# Installed program probes

Starts the installed `tmux-workspace` and `libtmux-mcp-server` the way a user
does and checks what they answer. CI runs them in the `package` job against the
installed prefix, and `release.yml` runs `mcp-catalog` against the binary it is
about to publish. None needs a tmux server: the protocol layer answers.

| Command | What it checks |
|---|---|
| `workspace-tree` | `tmux-workspace --command-tree` lists nine root commands and two import sources |
| `mcp-catalog` | The server initializes, names itself `libtmux-cxx`, and advertises tools; with `--check-arguments` a `tools/call` with a bad argument is refused with the error that names it, so the call and its `_meta` arrived intact |
| `mcp-hostile` | Eleven lines of valid JSON of the wrong shape each get an error reply, and the good request after them is still answered |

```console
$ uv run scripts/installed_probe/installed_probe.py workspace-tree --binary build/prefix/bin/tmux-workspace
```

```console
$ uv run scripts/installed_probe/installed_probe.py mcp-catalog --server build/prefix/bin/libtmux-mcp-server --socket-name probe --check-arguments
```

```console
$ uv run scripts/installed_probe/installed_probe.py mcp-hostile --server build/prefix/bin/libtmux-mcp-server --socket-name probe
```

Each prints what it saw and exits nonzero on a failure. The MCP probes use
`select` on the server's pipes, so they run on Linux and macOS.

## Tests

```console
$ uv run --group dev pytest scripts/installed_probe
```

The tests run each probe against a stand-in program that can fail in each way
the probe looks for.
