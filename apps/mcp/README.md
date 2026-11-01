<!-- libtmux-logo -->
<div align="center">
  <picture>
    <source srcset="../../assets/mcp.svg" type="image/svg+xml">
    <img src="../../assets/mcp.png" width="128" height="128" alt="libtmux for C++ MCP">
  </picture>
</div>
<!-- /libtmux-logo -->

<div align="center">

# libtmux-mcp-server

[![libtmux.org](https://img.shields.io/badge/docs-libtmux.org-0b7285)](https://libtmux.org/en/cxx/latest/mcp/)

A [Model Context Protocol](https://modelcontextprotocol.io) server for
[tmux](https://github.com/tmux/tmux), built on the C++
[libtmux](../../README.md). It speaks newline-delimited JSON-RPC over stdio and
ships as one native executable.

</div>

- An alpha interface. The same startup-frozen tool manifest is advertised on
  POSIX and native Windows.
- On Windows, psmux support is command-dependent: unsupported operations fail
  at dispatch, and every tool stays in the manifest.
- Full contract: the [C++ MCP overview][mcp-index], the [tool pages][mcp-tools]
  and the [topics][mcp-topics]; the protocol, failure semantics and Windows
  wrapper are in [`docs/mcp-server.md`](../../docs/mcp-server.md).

## Quick start

Build and install the server. It is off by default and needs a JSON parser;
the core library does not:

```console
$ cmake \
    -S . \
    -B build/mcp \
    -DLIBTMUX_BUILD_MCP_SERVER=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/mcp
```

```console
$ cmake --install build/mcp --prefix ~/.local
```

This installs `libtmux-mcp-server` in the selected binary directory. Check it
without contacting tmux:

```console
$ libtmux-mcp-server --version
```

Register it with Claude Code. Without a selector or a socket variable the server
drives the server tmux picks: the one `TMUX` names, else the default socket.
See [Sockets](#sockets) to choose another:

```console
$ claude mcp add tmux -- ~/.local/bin/libtmux-mcp-server
```

## Tools

The server registers its tools in four toolsets. Each name links to its page,
with the arguments and results:

<!-- BEGIN GENERATED TOOL INVENTORY -->
<!-- Generated from tmux://capabilities; run this script to update. -->
| Toolset | Tools |
|---|---|
| `inspect` | [`list_sessions`][tool-list_sessions], [`list_windows`][tool-list_windows], [`list_panes`][tool-list_panes], [`get_server_info`][tool-get_server_info], [`get_session_info`][tool-get_session_info], [`get_window_info`][tool-get_window_info], [`get_pane_info`][tool-get_pane_info], [`capture_pane`][tool-capture_pane], [`capture_since`][tool-capture_since], [`snapshot_pane`][tool-snapshot_pane], [`search_panes`][tool-search_panes], [`find_pane_by_position`][tool-find_pane_by_position], [`wait_for_text`][tool-wait_for_text], [`get_tmux_variables`][tool-get_tmux_variables], [`show_option`][tool-show_option], [`show_environment`][tool-show_environment], [`show_hooks`][tool-show_hooks], [`call_read_tools_batch`][tool-call_read_tools_batch] |
| `manage` | [`rename_session`][tool-rename_session], [`rename_window`][tool-rename_window], [`select_window`][tool-select_window], [`select_pane`][tool-select_pane], [`select_layout`][tool-select_layout], [`resize_window`][tool-resize_window], [`resize_pane`][tool-resize_pane], [`move_window`][tool-move_window], [`swap_pane`][tool-swap_pane], [`set_pane_title`][tool-set_pane_title], [`wait_for_channel`][tool-wait_for_channel], [`signal_channel`][tool-signal_channel], [`set_mouse_enabled`][tool-set_mouse_enabled], [`set_history_limit`][tool-set_history_limit] |
| `execute` | [`create_session`][tool-create_session], [`create_window`][tool-create_window], [`split_window`][tool-split_window], [`respawn_pane`][tool-respawn_pane], [`run_shell_command`][tool-run_shell_command], [`send_keys`][tool-send_keys], [`send_keys_batch`][tool-send_keys_batch], [`paste_text`][tool-paste_text], [`set_synchronize_panes`][tool-set_synchronize_panes] |
| `teardown` | [`clear_pane_scrollback`][tool-clear_pane_scrollback], [`kill_pane`][tool-kill_pane], [`kill_window`][tool-kill_window], [`kill_session`][tool-kill_session] |

[tool-list_sessions]: https://libtmux.org/en/cxx/latest/mcp/tools/list_sessions/
[tool-list_windows]: https://libtmux.org/en/cxx/latest/mcp/tools/list_windows/
[tool-list_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/list_panes/
[tool-get_server_info]: https://libtmux.org/en/cxx/latest/mcp/tools/get_server_info/
[tool-get_session_info]: https://libtmux.org/en/cxx/latest/mcp/tools/get_session_info/
[tool-get_window_info]: https://libtmux.org/en/cxx/latest/mcp/tools/get_window_info/
[tool-get_pane_info]: https://libtmux.org/en/cxx/latest/mcp/tools/get_pane_info/
[tool-capture_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/capture_pane/
[tool-capture_since]: https://libtmux.org/en/cxx/latest/mcp/tools/capture_since/
[tool-snapshot_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/snapshot_pane/
[tool-search_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/search_panes/
[tool-find_pane_by_position]: https://libtmux.org/en/cxx/latest/mcp/tools/find_pane_by_position/
[tool-wait_for_text]: https://libtmux.org/en/cxx/latest/mcp/tools/wait_for_text/
[tool-get_tmux_variables]: https://libtmux.org/en/cxx/latest/mcp/tools/get_tmux_variables/
[tool-show_option]: https://libtmux.org/en/cxx/latest/mcp/tools/show_option/
[tool-show_environment]: https://libtmux.org/en/cxx/latest/mcp/tools/show_environment/
[tool-show_hooks]: https://libtmux.org/en/cxx/latest/mcp/tools/show_hooks/
[tool-call_read_tools_batch]: https://libtmux.org/en/cxx/latest/mcp/tools/call_read_tools_batch/
[tool-rename_session]: https://libtmux.org/en/cxx/latest/mcp/tools/rename_session/
[tool-rename_window]: https://libtmux.org/en/cxx/latest/mcp/tools/rename_window/
[tool-select_window]: https://libtmux.org/en/cxx/latest/mcp/tools/select_window/
[tool-select_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/select_pane/
[tool-select_layout]: https://libtmux.org/en/cxx/latest/mcp/tools/select_layout/
[tool-resize_window]: https://libtmux.org/en/cxx/latest/mcp/tools/resize_window/
[tool-resize_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/resize_pane/
[tool-move_window]: https://libtmux.org/en/cxx/latest/mcp/tools/move_window/
[tool-swap_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/swap_pane/
[tool-set_pane_title]: https://libtmux.org/en/cxx/latest/mcp/tools/set_pane_title/
[tool-wait_for_channel]: https://libtmux.org/en/cxx/latest/mcp/tools/wait_for_channel/
[tool-signal_channel]: https://libtmux.org/en/cxx/latest/mcp/tools/signal_channel/
[tool-set_mouse_enabled]: https://libtmux.org/en/cxx/latest/mcp/tools/set_mouse_enabled/
[tool-set_history_limit]: https://libtmux.org/en/cxx/latest/mcp/tools/set_history_limit/
[tool-create_session]: https://libtmux.org/en/cxx/latest/mcp/tools/create_session/
[tool-create_window]: https://libtmux.org/en/cxx/latest/mcp/tools/create_window/
[tool-split_window]: https://libtmux.org/en/cxx/latest/mcp/tools/split_window/
[tool-respawn_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/respawn_pane/
[tool-run_shell_command]: https://libtmux.org/en/cxx/latest/mcp/tools/run_shell_command/
[tool-send_keys]: https://libtmux.org/en/cxx/latest/mcp/tools/send_keys/
[tool-send_keys_batch]: https://libtmux.org/en/cxx/latest/mcp/tools/send_keys_batch/
[tool-paste_text]: https://libtmux.org/en/cxx/latest/mcp/tools/paste_text/
[tool-set_synchronize_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/set_synchronize_panes/
[tool-clear_pane_scrollback]: https://libtmux.org/en/cxx/latest/mcp/tools/clear_pane_scrollback/
[tool-kill_pane]: https://libtmux.org/en/cxx/latest/mcp/tools/kill_pane/
[tool-kill_window]: https://libtmux.org/en/cxx/latest/mcp/tools/kill_window/
[tool-kill_session]: https://libtmux.org/en/cxx/latest/mcp/tools/kill_session/
<!-- END GENERATED TOOL INVENTORY -->

### Core workflow

Start with [`list_sessions`][tool-list_sessions],
[`list_windows`][tool-list_windows] and [`list_panes`][tool-list_panes], then
keep the stable IDs they return. These are the common workflows:

| Tool | Arguments | Result |
|---|---|---|
| [`get_server_info`][tool-get_server_info] | — | Server liveness and tmux version; a daemon older than 3.3a is refused, with the instruction to upgrade tmux and restart the server |
| [`list_sessions`][tool-list_sessions] | — | Stable session IDs, names, paths, client counts, and window counts |
| [`list_windows`][tool-list_windows] | `session` | Windows belonging to one exact session |
| [`list_panes`][tool-list_panes] | — | Every pane with stable pane, window, and session IDs |
| [`create_session`][tool-create_session] | `name?`, `windowName?`, `startDirectory?`, `width?`, `height?` | New session ID and name |
| [`create_window`][tool-create_window] | `session`, `name?`, `startDirectory?` | New window ID and owning session ID |
| [`capture_pane`][tool-capture_pane] | `paneId`, `history?` | Visible or retained pane text |
| [`capture_since`][tool-capture_since] | `paneId`, `cursor?` | Text after a byte cursor and a replacement cursor |
| [`snapshot_pane`][tool-snapshot_pane] | `paneId` | Pane metadata and visible text from one call |
| [`paste_text`][tool-paste_text] | `paneId`, `text` | Pane ID after pasting literal text |
| [`send_keys`][tool-send_keys] | `paneId`, `keys` | Pane ID and every resolved synchronized target |
| [`wait_for_text`][tool-wait_for_text] | `target`, `text`, `timeout_ms?` | Match, timeout, elapsed time, transport mode, and final capture |
| [`search_panes`][tool-search_panes] | `pattern` | Matching pane IDs and matching lines |
| [`call_read_tools_batch`][tool-call_read_tools_batch] | `operations`, `onError?` | Ordered nested results, counters, and explicit truncation state |

### What to know before an agent drives it

- Read terminal text without changing pane modes: [`capture_pane`][tool-capture_pane]
  with `history` for scrollback, [`capture_since`][tool-capture_since] to follow
  new output, and [`search_panes`][tool-search_panes] to find lines across panes.
- Creation and respawn tools accept no command or environment payload;
  [`run_shell_command`][tool-run_shell_command] runs an explicit shell command.
- [`send_keys`][tool-send_keys], [`send_keys_batch`][tool-send_keys_batch] and
  [`paste_text`][tool-paste_text] send input to a pane's existing process.
- Input tools require the addressed pane, and any synchronized peers, to be
  live, input-enabled, outside human-owned mode and not the calling pane.
- [`set_synchronize_panes`][tool-set_synchronize_panes] is the only tool marked
  as amplifying future input.
- [`wait_for_text`][tool-wait_for_text] waits on control-mode output, falling
  back to capture polling, under one deadline.
- [`call_read_tools_batch`][tool-call_read_tools_batch] takes one through 16
  serial inspect calls.
- Annotations describe requested effects; they are not a sandbox against
  `command-alias` or hostile local reconfiguration.

The [full contract](../../docs/mcp-server.md#core-workflow) covers the guards,
synchronized panes and the response bounds.

### Moving from the twelve-tool alpha

Eight route names remain, though callers must use their current schemas. The
other four map as follows:

| Earlier route | Current route |
|---|---|
| `inspect_tmux` | [`list_sessions`][tool-list_sessions] + [`list_windows`][tool-list_windows] + [`list_panes`][tool-list_panes] |
| `list_session_panes` | [`list_panes`][tool-list_panes]; filter returned `session_id` |
| `new_window` | [`create_window`][tool-create_window]; `session` and `name` remain |
| `send_text` | [`paste_text`][tool-paste_text]; `target` becomes `paneId` |

## Choose what it serves

### Toolsets

- `LIBTMUX_TOOLSETS` is a comma-separated subset of `inspect`, `manage`,
  `execute` and `teardown`; all 16 subsets are valid.
- `LIBTMUX_TOOLS` adds exact names, then `LIBTMUX_EXCLUDE_TOOLS` removes exact
  names last. Unknown names fail startup.
- `LIBTMUX_SAFETY` is retired; its presence stops startup with a migration error.

The [toolset topics][mcp-topics] cover what each toolset holds. Serve only the
[`inspect`][mcp-toolset-inspect] toolset:

```console
$ claude mcp add tmux \
    -e LIBTMUX_TOOLSETS=inspect \
    -- ~/.local/bin/libtmux-mcp-server --socket-name libtmux-agent
```

Remove tools by name, after the toolsets and `LIBTMUX_TOOLS`:

```console
$ claude mcp add tmux \
    -e LIBTMUX_EXCLUDE_TOOLS=send_keys,send_keys_batch,paste_text \
    -- ~/.local/bin/libtmux-mcp-server --socket-name libtmux-agent
```

### Sockets

The process resolves one socket before protocol startup and never accepts a
per-call override:

| Variable | Meaning |
|---|---|
| `LIBTMUX_SOCKET` | Exact tmux socket name |
| `LIBTMUX_SOCKET_PATH` | Exact socket path; wins when `LIBTMUX_SOCKET` is also set |
| `LIBTMUX_TMUX_CONFIG` | Explicit absolute tmux configuration path |

- With no selector, `LIBTMUX_SOCKET_PATH` names the socket, else
  `LIBTMUX_SOCKET`. With neither set the server passes tmux no socket, so tmux
  picks one. The server starts no server of its own and ends none.
- A variable that is empty or only whitespace counts as unset, each one alone.
- A relative `LIBTMUX_SOCKET_PATH` reaches what `tmux -S` reaches from the
  server's working directory. It is reported as an absolute path when that
  fits a socket address.
- `--socket SELECTOR` takes `name:NAME`, `path:/ABSOLUTE/PATH` or `inherit`;
  `--socket-path PATH` is tmux `-S` on POSIX, `--socket-name NAME` is `-L`, and
  one positional `PATH` stays as a POSIX spelling.
- An option or flag comes before both variables, which are then not read.
  `--socket inherit` uses the `TMUX` socket and stops startup with status 2
  when `TMUX` names none.
- `--tmux-config /ABSOLUTE/PATH` selects an explicit tmux configuration, read
  only by a server this process starts.
- Conflicting options, an empty option value, a variable tmux cannot use, and an
  empty or relative `LIBTMUX_TMUX_CONFIG` fail before tmux opens. The message
  names the variable and never quotes its value.
- [`tmux://capabilities`][mcp-capabilities] reports the selector, its
  provenance, the server state and the effective tools.

```console
$ libtmux-mcp-server --socket-path /tmp/tmux-1000/agent
```

```console
$ libtmux-mcp-server --socket-name agent
```

The [full socket rules](../../docs/mcp-server.md#socket-selection) cover
provenance and teardown defaults. Inspect the command line without contacting
tmux:

```console
$ libtmux-mcp-server --help
```

## Connect a client

For an isolated POSIX socket, with Codex:

```console
$ codex mcp add tmux -- \
    ~/.local/bin/libtmux-mcp-server \
    --socket-path /tmp/tmux-1000/agent
```

With Claude Code:

```console
$ claude mcp add \
    --transport stdio \
    --scope local \
    tmux -- \
    ~/.local/bin/libtmux-mcp-server \
    --socket-path /tmp/tmux-1000/agent
```

For Claude Desktop, the equivalent entry carries the same variables in `env`:

```json
{
  "mcpServers": {
    "tmux": {
      "command": "/home/you/.local/bin/libtmux-mcp-server",
      "args": ["--socket-name", "libtmux-agent"],
      "env": { "LIBTMUX_TOOLSETS": "inspect,manage" }
    }
  }
}
```

[Connecting a client][mcp-guide] covers the rest.

## Native Windows with psmux

- Windows MCP uses the same tool manifest through a trusted psmux route;
  unsupported psmux commands fail explicitly.
- Build the `windows-psmux` preset with the audited psmux version in
  [the library README](../../README.md#windows-through-psmux).
- psmux 3.3.7 ignores `PSMUX_DATA_DIR`: use a high-entropy `--socket-name`, and
  set `PSMUX_CONFIG_FILE` to an explicit audited file.
- The audited PowerShell fixture creates the socket, configuration and state
  file before an agent connects:
  [`docs/mcp-server.md`](../../docs/mcp-server.md#native-windows-with-psmux).
- On POSIX, [`mcp-swap`](../../tools/mcp/README.md) swaps installed agent
  configurations to a checkout; it does not manage the Windows wrapper.

## Protocol and failures

- The server speaks newline-delimited JSON-RPC over stdio: the current
  `2026-07-28` protocol, plus the `2025-11-25`, `2025-06-18`, `2025-03-26` and
  `2024-11-05` initialize lifecycles.
- It advertises tools and one static resource, `tmux://capabilities`; there are
  no prompts, dynamic resources or subscriptions.
- Tool calls run on four workers, at most 64 in flight; replies may arrive out
  of order, and JSON-RPC IDs correlate them.
- A bad argument is a completed result with `isError: true` on `2025-11-25` and
  `2026-07-28`, and `-32602` on the older revisions.
- tmux refusing a well-formed call is a tool result with `isError: true`. An
  unexpected C++ failure becomes a generic internal JSON-RPC error, and the
  detail goes to stderr.
- The [protocol](../../docs/mcp-server.md#protocol-and-lifecycle) and
  [failure semantics](../../docs/mcp-server.md#failure-semantics) pages have the
  details.

## Layout

| Path | Purpose |
|---|---|
| [`include/libtmux_consumers/mcp.hpp`](include/libtmux_consumers/mcp.hpp) | Small format-independent tool model and catalog API |
| [`src/tool_model.cpp`](src/tool_model.cpp) | Strict named-argument and UTF-8 code-point validation |
| [`src/tool_catalog.cpp`](src/tool_catalog.cpp) | Capability-aware tool declarations and handlers |
| [`src/wait_for_text.cpp`](src/wait_for_text.cpp) | Shared-deadline control-output and capture-polling wait transport |
| [`src/schema.cpp`](src/schema.cpp) | Tool schemas and era-specific result encoding |
| [`src/protocol_validation.cpp`](src/protocol_validation.cpp) | Request metadata and tool-argument validation |
| [`src/protocol.cpp`](src/protocol.cpp) | Dual-era lifecycle, routing, and call decoding |
| [`src/dispatcher.cpp`](src/dispatcher.cpp) | Bounded workers, cancellation, progress, and serialized replies |
| [`src/batch_response.cpp`](src/batch_response.cpp) | Ordered asynchronous batch aggregation and reservation lifetime |
| [`src/stdio_server.cpp`](src/stdio_server.cpp) | Bounded newline framing and single/batch protocol composition |
| [`src/cli.cpp`](src/cli.cpp) | Fail-closed socket selection |
| [`src/server.cpp`](src/server.cpp) | Executable composition root |
| [`tests/mcp_test.cpp`](tests/mcp_test.cpp) | Direct tool tests against deterministic seams and isolated real tmux |
| [`tests/protocol_test.cpp`](tests/protocol_test.cpp) | Both protocol eras, schemas, backpressure, concurrency, cancellation, framing, and selectors |
| [`tests/windows_psmux_smoke.ps1`](tests/windows_psmux_smoke.ps1) | Native psmux fixture, pinned catalog, explicit unsupported-command behavior, and exact cleanup |

## Related

- [libtmux (C++)](../../README.md) — the library and Windows psmux boundary
- [Changelog](CHANGELOG.md) — what each release changed in the server; a
  release attaches it beside the binaries, and an install puts it in
  `share/doc/libtmux-mcp-server/`
- [libtmux-mcp](https://github.com/tmux-python/libtmux-mcp) — the broader
  Python MCP server
- [libtmux (Python)](https://libtmux.git-pull.com) — the original library

## License

MIT. See [LICENSE](../../LICENSE).

[mcp-index]: https://libtmux.org/en/cxx/latest/mcp/
[mcp-tools]: https://libtmux.org/en/cxx/latest/mcp/tools/
[mcp-topics]: https://libtmux.org/en/cxx/latest/mcp/topics/
[mcp-guide]: https://libtmux.org/en/cxx/latest/mcp/guides/
[mcp-capabilities]: https://libtmux.org/en/cxx/latest/mcp/resources/tmux-capabilities/
[mcp-toolset-inspect]: https://libtmux.org/en/cxx/latest/mcp/reference/libtmux-mcp-toolset-inspect/
