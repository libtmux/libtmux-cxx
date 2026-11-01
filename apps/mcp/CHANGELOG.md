# Changelog

Notable changes to `libtmux-mcp-server`.

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](../../.github/WRITING.md#changelog).
This file covers the MCP server.

The application is released with the library under one version, so every
release has a section here. Changes to the library, and changes that span the
library and the applications, such as documentation, CI and release tooling,
are in the [`libtmux` changelog](../../CHANGELOG.md). `tmux-workspace` keeps
[its own](../workspace/CHANGELOG.md).

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

## 0.1.0 (2026-11-01)

### Breaking

- `libtmux-mcp-server` with no selector no longer starts a server of its own on
  the socket `libtmux-mcp`. It passes tmux no socket unless
  `LIBTMUX_SOCKET_PATH` or `LIBTMUX_SOCKET` names one, so it drives the server
  `TMUX` names inside tmux and the default socket elsewhere.
  Behaviour-breaking; flags and options are unchanged. (#128)

  Before: `libtmux-mcp-server`
  After: `libtmux-mcp-server --socket-name libtmux-mcp` (the server it starts
  now reads your tmux configuration, is not stopped at exit, and has no
  `teardown` toolset unless `LIBTMUX_TOOLSETS` grants it)

- `tmux://capabilities` reports the selection provenance as `explicit`,
  `env-socket-path`, `env-socket` or `tmux-choice`, in place of
  `operator-current` and `default-dedicated`. A launch that names no socket has
  the selector `none`, and the server state is `existing` or `absent`;
  `created` is gone. Behaviour-breaking: a client that matches the old strings
  stops matching. (#128)

  Before: `"selectionProvenance": "operator-current"`
  After: `"selectionProvenance": "explicit"` for an option or flag,
  `"env-socket-path"` or `"env-socket"` for a variable

- A server `libtmux-mcp-server` starts on `--socket-name`, `--socket-path` or a
  socket variable no longer runs with `exit-empty off`: it reads your tmux
  configuration and exits with its last session. The installed
  `libtmux-mcp-minimal.conf` is removed. Behaviour-breaking; the command line
  is unchanged. (#128)

  Before: `libtmux-mcp-server --socket-name agent`
  After: `libtmux-mcp-server --socket-name agent --tmux-config /path/keep.conf`,
  with `set -s exit-empty off` in the file

- `libtmux-mcp-server --socket inherit` stops startup with status 2, naming
  `TMUX`, when `TMUX` names no socket, and reads neither socket variable.
  Before, it fell back to the default socket. Behaviour-breaking. (#128)

### MCP server

- `libtmux-mcp-server` speaks newline-delimited JSON-RPC over stdio and ships
  as one native executable, built with `-DLIBTMUX_BUILD_MCP_SERVER=ON`. A
  native Windows build advertises the same startup-frozen tool manifest, and a
  command psmux cannot serve fails at dispatch. (#4)
- One immutable capability registry governs registration, calls, schemas,
  descriptions, annotations and disclosure, and every tool belongs to one of
  four toolsets: `inspect`, `manage`, `execute` and `teardown`.
  `LIBTMUX_TOOLSETS` selects toolsets, `LIBTMUX_TOOLS` adds exact names and
  `LIBTMUX_EXCLUDE_TOOLS` removes them, all frozen at startup. (#4)
- Tools publish an `outputSchema` and answer with `structuredContent`. A tool
  that acts on one pane takes `paneId`, a canonical `%`-prefixed id; only
  `wait_for_text`, `show_option` and `show_hooks` keep `target`, which is any
  tmux target expression. A refusal is a tool error result, not a protocol
  error. (#4, #18)
- The static `tmux://capabilities` resource reports the startup-frozen
  effective tool rows, the connection's provenance and the common process
  boundary, and every tool carries matching
  `com.git-pull.libtmux-mcp/capability` metadata. (#4)
- The server supports protocol `2026-07-28` and the `2025-11-25`, `2025-06-18`,
  `2025-03-26` and `2024-11-05` initialize lifecycles. `server/discover`
  answers a current client with the capabilities and a public cache lifetime,
  and JSON-RPC batches are accepted under the versions that allow them and
  answered in input order. (#4)
- Tool calls run on four workers with a bounded number in flight, and a call
  past the bound answers `-32003`. `notifications/progress` follows a
  `progressToken`, and `notifications/cancelled` cancels a queued or running
  call. A request that finished before stdin closed is answered. (#4, #6)
- A server selects its socket once, before protocol startup, and takes no
  per-call override. `--socket-name`, `--socket-path`, `--socket SELECTOR`
  (`name:NAME`, `path:/ABSOLUTE/PATH` or `inherit`) and a positional path come
  first. Otherwise `LIBTMUX_SOCKET_PATH` wins over `LIBTMUX_SOCKET`, and with
  neither the server passes tmux no socket. Both variables set no longer stops
  startup, and a variable that is empty or only whitespace is unset instead of
  an error. A relative `LIBTMUX_SOCKET_PATH` is accepted and reported as an
  absolute path when that fits a socket address, and a variable tmux cannot use
  stops startup with status 2, naming the variable. (#128)
- A server a tool starts, because a session was created on an absent socket,
  reads your tmux configuration, or the absolute path in `--tmux-config` or
  `LIBTMUX_TMUX_CONFIG`.
- Pane-input tools check immediately before dispatch that each configured
  target is live, input-enabled, outside a human-owned mode and an attached
  client's current view, and distinct from the caller. `send_keys` and each
  `send_keys_batch` row check the effective synchronised cohort, and
  `paste_text` uses one private target-only buffer. Process-wide pane leases
  keep overlapping input from interleaving. (#4)
- `run_shell_command` requires one target running a supported POSIX foreground
  shell and frames completion collision-free in a subshell, keeping Bash and
  Zsh error and debug traps. A timed-out or cancelled run releases its pane once
  the command finishes. (#4, #18)
- `wait_for_text` discounts echoed input from `send_keys`, `paste_text` and
  `run_shell_command`, including submitted commands and wrapped lines. It uses
  control-mode events when it can open a connection and falls back to polling,
  fails with the output so far when the pane or server goes away, and reports
  the path in `mode`. (#18, #69)
- A session argument resolves by exact name, so a mistyped name reports no such
  session instead of acting on a differently named one. (#17)
- `call_read_tools_batch` runs one through 16 serial inspect calls within a
  1,000,000-byte JSON-RPC response, `search_panes` is deterministic and
  bounded, and `set_synchronize_panes` declares that it amplifies later pane
  input. `get_server_info` reports the configured socket path and refuses a
  daemon older than 3.3a. (#4, #68)

## 0.1.0-alpha.14 (2026-10-25)

No entries for `libtmux-mcp-server`.

## 0.1.0-alpha.13 (2026-10-24)

### Breaking

- The MCP `paste_text` tool is refused before it stages a buffer on a daemon
  older than tmux 3.4. Behaviour-breaking. (#69)

### MCP server

- `wait_for_text` fails with the output so far when the pane or its server
  goes away mid-wait, instead of timing out. (#69)

## 0.1.0-alpha.12 (2026-10-18)

### MCP server

- `get_server_info` returns an error for a daemon older than 3.3a instead
  of `running: false`. (#68)
- The server instructions, the `get_server_info` description and
  `tmux://capabilities` (`minimumTmuxVersion`) name the 3.3a floor. (#68)

## 0.1.0-alpha.11 (2026-09-26)

### MCP server

- `kill_session`, `rename_session`, `list_windows`, `get_session_info`,
  `set_history_limit` and `create_window` resolve their `session` argument to
  an exact match by listing sessions and comparing names, rather than handing
  tmux a bare or `=`-prefixed target that can resolve by prefix; a stale or
  mistyped name now reports no such session instead of acting on a
  differently named one. (#17)

## 0.1.0-alpha.10 (2026-09-19)

No entries for `libtmux-mcp-server`.

## 0.1.0-alpha.9 (2026-09-19)

### MCP server

- `wait_for_text` discounts echoed input from `send_keys`, `paste_text` and
  `run_shell_command`, including submitted commands and wrapped lines. (#18)
- Timed-out or cancelled `run_shell_command` calls now release the pane once
  the command finishes. (#18)
- Tool refusals now return tool error results instead of protocol errors.
  (#18)

## 0.1.0-alpha.8 (2026-09-12)

No entries for `libtmux-mcp-server`.

## 0.1.0-alpha.7 (2026-09-06)

### Breaking

- `enter_copy_mode` and `exit_copy_mode` are gone from the MCP tool surface.
  Wire-breaking for MCP clients; the core `Pane` operations are unchanged.

  Before: `{"name": "enter_copy_mode", "arguments": {"target": "%1"}}`
  After:  `{"name": "capture_pane", "arguments": {"paneId": "%1"}}`

- A tool that acts on one pane names that argument `paneId` rather than
  `target`, and takes a canonical `%`-prefixed id. Wire-breaking for MCP
  clients. `wait_for_text`, `show_option` and `show_hooks` keep `target`, which
  is any tmux target expression rather than one pane.

  Before: `{"name": "capture_pane", "arguments": {"target": "%1"}}`
  After:  `{"name": "capture_pane", "arguments": {"paneId": "%1"}}`

- `LIBTMUX_SAFETY` stops MCP startup with a migration error rather than being
  honoured. Behavioural.

  Before: `LIBTMUX_SAFETY=read-only libtmux-mcp-server`
  After:  `LIBTMUX_TOOLSETS=inspect libtmux-mcp-server`

### MCP server

- Replace the legacy tool catalog with one immutable 45-tool capability
  registry that governs registration, calls, schemas, descriptions, internal
  input-sink claims, annotations, nested authority, and disclosure.
- Remove `enter_copy_mode` and `exit_copy_mode` from the MCP surface. Use
  `capture_pane`, `capture_since`, `snapshot_pane`, or `search_panes` for
  terminal text; the core `Pane` operations remain available.
- Add startup-frozen `LIBTMUX_TOOLSETS`, `LIBTMUX_TOOLS`, and
  `LIBTMUX_EXCLUDE_TOOLS` selection. `LIBTMUX_SAFETY` now stops startup with a
  migration error.
- Add one process-wide socket selected by `LIBTMUX_SOCKET` or
  `LIBTMUX_SOCKET_PATH`. A new product-dedicated socket uses the bundled
  minimal configuration unless `LIBTMUX_TMUX_CONFIG` names an absolute path;
  only its authenticated creator enables teardown by default and stops it when
  stdio closes.
- Add the static `tmux://capabilities` resource and matching
  `com.git-pull.libtmux-mcp/capability` metadata to every advertised tool.
  Public rows expose schema-keyed input literalization while sink claims remain
  internal validation data.
- Add bounded typed read batching that retains every executed row within a
  1,000,000-byte newline-terminated JSON-RPC response, deterministic bounded
  pane search, and synchronized-pane target disclosure. Request IDs over 512
  KiB fail before dispatch rather than consuming that response budget.
  `set_synchronize_panes` declares that it amplifies subsequent pane input.
- Pane-input tools reject malformed state snapshots and require their
  configured targets to be live, input-enabled, outside human-owned modes and
  terminal attention, and distinct from the caller immediately before
  dispatch. `send_keys` and each `send_keys_batch` row preflight the effective
  synchronized cohort, while `paste_text` uses one private target-only buffer
  for text and optional Enter. Process-wide pane leases prevent overlapping
  input and remain held for uncertain shell runs. `run_shell_command` also
  requires one configured target running a supported POSIX foreground shell
  and uses collision-free, subshell-isolated completion framing through the
  exact tmux endpoint while preserving bounded Bash and Zsh error/debug traps.

### MCP switcher

- Replace `tools/mcp/mcp_swap.py` with a native `mcp-swap`, built beside the
  tests. It preserves the formatting of each agent config it edits, locks
  across concurrent runs, and records one checksummed transaction before any
  write so a failure rolls back in reverse order.
- `get_server_info` reports the socket path the handle was configured with, and
  `wait_for_text` opens its control connection on that path. tmux escapes a
  non-printable byte in the path it stores at server start, so asking it for
  `#{socket_path}` returned a path naming no file on tmux 3.4 and 3.5 — reported
  to the caller as the resolved socket, and enough to make streaming fall back
  to polling. 3.3a and earlier answer with the raw byte, and 3.6 does again.

- `mcp-swap use-local` validates every selected agent config and backup
  destination before changing any config. A malformed later config leaves the
  whole selection unchanged.

## 0.1.0-alpha.6 (2026-08-31)

No entries for `libtmux-mcp-server`.

## 0.1.0-alpha.5 (2026-08-22)

No entries for `libtmux-mcp-server`.

## 0.1.0-alpha.4 (2026-08-22)

### MCP server

- A request that finished before stdin closed is answered rather than
  discarded. Shutdown cancelled every request still in flight, and the reply a
  worker had already produced went with it, so a client that wrote its
  requests and closed the pipe lost answers for whatever was still running.
  (#6)

## 0.1.0-alpha.3 (2026-08-22)

### MCP server

- The tool catalog grows from five tools to twelve. `inspect_tmux`,
  `list_windows`, `list_session_panes`, `create_session`, `send_keys`,
  `wait_for_text` and `search_panes` join the original five, which are
  unchanged. (#4)
- Tools publish an `outputSchema` and answer with `structuredContent`, so a
  client validates a reply instead of parsing prose out of it. (#4)
- Accept JSON-RPC batches under the protocol versions that allow them, and
  answer in input order. A member that is a notification, or not a request at
  all, contributes no reply. (#4)
- Negotiate five protocol versions: `2026-07-28`, and the legacy
  `2025-11-25`, `2025-06-18`, `2025-03-26` and `2024-11-05`. (#4)
- Add `server/discover`, which answers the modern discovery request with the
  catalog and its cache policy. (#4)
- Run calls on a worker pool with a bounded number in flight. Past the bound
  the server answers `-32003` rather than queueing without limit. (#4)
- Emit `notifications/progress` for a call carrying a `progressToken`, and
  honour `notifications/cancelled` for one already running. (#4)
- Take `--socket-name` or `--socket-path`. Without a selector only a valid
  inherited `TMUX` route is accepted, so the server never silently drives a
  tmux the caller did not name. (#4)
- A wait whose deadline expired during target lookup no longer publishes an
  empty `pane_id`, which its own output schema refuses. The field is emitted
  only once a target resolves, and is no longer required. (#4)

## 0.1.0-alpha.2 (2026-08-17)

### MCP server

- `--prefix` now searches `tools/libtmux/` as well as `bin/`. The vcpkg port
  installs the server there, because a static triplet has no `bin/` for it, so
  pointing an agent CLI at a vcpkg install failed with `is not an executable`
  naming a path the package never wrote.

## 0.1.0-alpha.1 (2026-08-16)

- An MCP server, so an agent can drive tmux directly.
