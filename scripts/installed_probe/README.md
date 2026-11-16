# Installed program probes

Starts the installed `tmux-workspace` and `libtmux-mcp-server` the way a user
does and checks what they answer. CI runs them in the `package` job against the
installed prefix, and `release.yml` runs `mcp-catalog` against the binary it is
about to publish. None needs a tmux server: the protocol layer answers.

| Command | What it checks |
|---|---|
| `workspace-tree` | `tmux-workspace --command-tree` lists the root commands and the import sources |
| `mcp-catalog` | The server initializes, names itself `libtmux-cxx`, and advertises tools; with `--check-arguments` a `tools/call` with a bad argument is refused with the error that names it, so the call and its `_meta` arrived intact |
| `mcp-hostile` | Each line of valid JSON of the wrong shape gets an error reply, and the good request after them is still answered |
| `workspace-completion` | The bash completion `tmux-workspace --generate-completion bash` prints completes `import tm` to `tmuxinator`; this probe runs a short bash program, because the script under test is bash |
| `package-layout` | An install for C++20 carries the `tl::expected` fallback header and one for C++23 does not; neither carries a private CMake package for it |
| `testing-component` | A consumer that asks a Windows package for the POSIX-only `testing` component is refused with the diagnostic that says so; the cmake options after `--` select the package |

```console
$ uv run scripts/installed_probe/installed_probe.py workspace-tree \
    --binary build/prefix/bin/tmux-workspace
```

```console
$ uv run scripts/installed_probe/installed_probe.py mcp-catalog \
    --server build/prefix/bin/libtmux-mcp-server \
    --socket-name probe \
    --check-arguments
```

```console
$ uv run scripts/installed_probe/installed_probe.py mcp-hostile \
    --server build/prefix/bin/libtmux-mcp-server \
    --socket-name probe
```

```console
$ uv run scripts/installed_probe/installed_probe.py workspace-completion \
    --binary build/prefix/bin/tmux-workspace
```

```console
$ uv run scripts/installed_probe/installed_probe.py package-layout \
    --prefix build/prefix \
    --standard 20
```

On Windows, `mcp-catalog --expect-tools` checks the advertised tools against
the exact list in [`windows_mcp_tools.txt`](windows_mcp_tools.txt). Windows
advertises the same registry as POSIX and lets psmux refuse what it cannot do,
rather than hiding those tools; see
[the Windows design](../../docs/design/windows-psmux.md). Teardown is absent
because an explicit socket selector excludes it.

```console
$ uv run scripts/installed_probe/installed_probe.py mcp-catalog \
    --server build/windows-prefix/bin/libtmux-mcp-server.exe \
    --socket-name probe \
    --expect-tools scripts/installed_probe/windows_mcp_tools.txt
```

```console
$ uv run scripts/installed_probe/installed_probe.py testing-component \
    -- -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=build/windows-prefix
```

Each prints what it saw and exits nonzero on a failure. The MCP probes read
the server's stdout and stderr on their own threads, so they run on Linux,
macOS and Windows and never wait on a reply that is already in a buffer.

## Tests

```console
$ uv run --group dev pytest scripts/installed_probe
```

The tests run each probe against a stand-in program that can fail in each way
the probe looks for.
