# libtmux-mcp-server

A [Model Context Protocol](https://modelcontextprotocol.io) server for
[tmux](https://github.com/tmux/tmux), built on [libtmux](../../README.md) — the
C++ one. It speaks JSON-RPC over stdio and needs nothing on the machine but
tmux.

Give an agent hands inside the terminal, from a single static binary with no
runtime.

> [!WARNING]
> **Alpha prerelease.** Built on a library whose API is not stable yet; the
> tool names and their arguments may change between alphas. See the
> [library README](../../README.md).

> [!NOTE]
> A small surface, deliberately. This exists to put weight on the library
> from the side a model sees — every argument arrives as an untyped string — and it is
> kept small enough to read in one sitting. For the full surface, use the
> Python [libtmux-mcp](https://github.com/tmux-python/libtmux-mcp), which
> covers far more of tmux and is the one to install if you want coverage
> rather than a reference.

## Tools

| Tool | Arguments | Answers with |
|---|---|---|
| `list_sessions` | — | One session name per line |
| `list_panes` | — | Pane id, window id and running command, tab separated |
| `capture_pane` | `target` | The visible contents of that pane |
| `send_text` | `target`, `text` | The pane id written to; the text is typed literally, interpreting no key names |
| `new_window` | `session`, `name` | The new window's id |
| `send_keys` | `target`, `keys` | The pane id; presses named keys such as `Enter` and `C-c`, checking every name before sending any |
| `wait_for_text` | `target`, `text`, `timeout_ms?` | The pane contents once the text appears, or a report that it did not |
| `search_panes` | `text` | One matching pane id per line with the line it matched, or nothing |

Each tool is one library call. A model cannot compose a tmux command that the
[typed surface](../../include/libtmux/) would not have allowed.

The three at the bottom are what let a model do more than look: press a key,
wait for something to finish rather than capturing repeatedly and guessing,
and find which pane is showing something. `send_text` types characters and
`send_keys` presses keys — a model that wants `C-c` needs the second.

Every argument carries a description in `tools/list`, saying what it accepts
and showing one — `target` takes a pane id such as `%1`, or anything tmux
resolves to a pane. A model that has read the schema does not have to guess,
and `consumer.mcp.protocol` fails if any argument stops describing itself.

`inputSchema` sets `additionalProperties: false`, so a misspelt argument is
reported rather than ignored.

## Quickstart

**Requirements:** a C++23 toolchain, CMake 3.25, tmux on `$PATH`.

Build only the server — the tests and examples are a much larger build and you
do not need them:

```console
$ cmake -S . -B build/mcp -DLIBTMUX_BUILD_MCP_SERVER=ON -DLIBTMUX_FETCH_DEPS=ON -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/mcp
```

```console
$ cmake --install build/mcp --prefix ~/.local
```

That puts `libtmux-mcp-server` in `~/.local/bin`. `LIBTMUX_FETCH_DEPS=ON` is
what fetches the JSON parser; this program is the only thing here that needs
one, which is why it is off by default.

### Pointing a client at it

The server takes the tmux socket as its only argument. Given none, it uses the
server it was started inside, and failing that the one tmux itself would pick.

```console
$ claude mcp add tmux -- ~/.local/bin/libtmux-mcp-server
```

```console
$ codex mcp add tmux -- ~/.local/bin/libtmux-mcp-server
```

For Claude Desktop, add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "tmux": {
      "command": "/home/you/.local/bin/libtmux-mcp-server",
      "args": ["/tmp/tmux-1000/default"]
    }
  }
}
```

To point every installed agent CLI at a build in this checkout — and put them
all back afterwards — use [`tools/mcp/mcp_swap.py`](../../tools/README.md).

### Checking it by hand

The protocol is line-delimited JSON-RPC, so a pipe is a complete client:

```console
$ echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | libtmux-mcp-server
```

## What it does when things go wrong

Two failures that look alike to a client are kept apart, because a model should
retry one and not the other.

**A malformed request** — an unknown tool, a missing required argument — is a
JSON-RPC error. The model asked for something that does not exist, and asking
again the same way will not help.

**tmux refusing a well-formed request** is a tool result with `isError` set and
tmux's own message as the text. The request was legal and the answer is
information: `tmux has no pane %999` is something a model can read and act on.

Nothing throws. The library reports every failure
[by value](../../README.md#what-the-design-commits-to), and this program has no
other way to fail.

## Layout

| Path | What it is |
|---|---|
| [`include/libtmux_consumers/mcp.hpp`](include/libtmux_consumers/mcp.hpp) | The tool surface. No JSON appears here: a tool takes named strings and returns text, so it can be tested without a protocol. |
| [`src/server.cpp`](src/server.cpp) | The JSON-RPC loop over stdio, and the only file that knows what JSON is. |
| [`tests/mcp_test.cpp`](tests/mcp_test.cpp) | Tests against real tmux, driving the tool set directly. |

That split is the point: the surface a model sees is a value you can call from
a test, and the protocol is a thin shell around it.

## Related

- [libtmux (C++)](../../README.md) — the library this is built on
- [libtmux-mcp](https://github.com/tmux-python/libtmux-mcp) — the Python
  server, with the full tool surface
- [libtmux (Python)](https://libtmux.git-pull.com) — the original
- [tmuxp](https://tmuxp.git-pull.com) — session manager; its documents are read
  by [`examples/workspace`](../../examples/README.md)

## License

MIT. See [LICENSE](../../LICENSE).
