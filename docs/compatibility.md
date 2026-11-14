# Compatibility

The tmux releases the library accepts, how it refuses the rest, and what it
cannot promise. The [README](../README.md#compatibility) keeps the floor; this
page holds the rest.

## The floor

tmux 3.3a and newer. The floor is the complete release: tmux 3.3 is below it.
[`Version`][ref-version] orders releases correctly: `3.7 < 3.7a < 3.7b`, and `next-X.Y` and
`X.Y-rcN` precede `X.Y`. CI tests the oldest and newest released tmux, 3.3a
and 3.8, on every pull request and master change. A release builds and tests
pinned compatibility cells from 3.3a through 3.8, plus `master`.

tmux release candidates are not supported: a feature can require one only until
its release ships, and a candidate such as `X.Y-rcN` parses and ranks below
`X.Y`. See
[Release candidates](https://libtmux.org/en/tmux/guides/tmux-support/#release-candidates).

## How a release is refused

A command is checked against the version of the daemon it reaches. The
first command to a server asks the daemon for its version without running a
hook, and a release below the floor, an unreadable version, `next-3.3a` and an
undated `master` are refused with `FailureKind::unsupported` before the command
has any effect. A tmux older than 3.2 cannot run that probe: a 3.0a daemon
answers "server version is too old for client", a 3.1c daemon prints the
`run-shell` usage, and a 3.1c or older client rejects `-N`. All three are
refused the same way, promptly, though those daemons keep the client's stdout
open after it exits. The message says to upgrade tmux and restart the server,
or stay on the previous libtmux release. Every later command to that daemon
carries a check that fails the whole command list if another daemon has taken
its place. `-V` and `-h` stay available below the floor as diagnostics and
authorize nothing, and `tmux_version()` reports a daemon from 3.2 up that is
below it. Cleanup of an endpoint that is already gone, `kill-server` included,
is a no-op. A program that launches tmux itself asks `Server::check_release()`
first, which refuses a release below the floor as the first command would and
sends nothing.

This assumes tmux's built-in commands keep their meaning: a `command-alias`
that redefines one of the commands the library uses to ask is outside what is
qualified.

## Optional features by release

A feature that needs a newer daemon says so, is refused before it has an
effect, and does not raise the floor for anything else. The daemon decides, not
the client executable.

| Feature | Daemon | Below it |
|---|---|---|
| Control mode: `Server::control`, `control_with_options`, `over_control`, `Connection::connect` | 3.7 | Refused, naming 3.7, before any client attaches ([why](#control-mode)). Attach on 3.7 and newer, idle only |
| `wait_for_text` streaming | 3.7 | Polls the pane; control mode is never tried |
| `Buffer::remove`, a literal `delete-buffer -b` or `set-buffer -b -n`, the MCP `paste_text` staging buffer | 3.4 | `FailureKind::unsupported`, before any buffer is touched ([why](#named-buffers)) |
| `:` or `.` in session and window names, typed or literal | not exactly 3.7 | `FailureKind::validation`, before dispatch ([why](#names-with-a-colon-or-a-period)) |
| Mirrored `main-*` presets, typed or in a literal `select-layout` | 3.5 | `FailureKind::validation`, before dispatch |
| JSON layouts, typed or in a literal `select-layout` | 3.8 | `FailureKind::validation`, before dispatch |
| A layout string tmux cannot read, typed or literal | never sent | `FailureKind::validation` on every release, before dispatch ([why](#unreadable-layout-strings)) |
| A literal `refresh-client -l`, under any alias or unique prefix | 3.6a | `FailureKind::unsupported`, before dispatch; no typed call ([why](#clipboard-query)) |
| A literal `break-pane` without `-n`, through `Server::run`, a batch or `Connection::execute` | not exactly 3.7 | `FailureKind::validation`, in any spelling and in a `;` list; `Pane::break_out` names the window itself ([why](#break-pane)) |

The gates that have a machine-readable entry are in
[`capabilities.json`](../capabilities.json).

## What differs across the range

tmux does not answer identically across the range, and where it cannot the
library cannot either. Each of these is tested only from the release that
provides it:

| Needs | Capability |
|---|---|
| tmux 3.4 | `%message` delivered to an attached control client |
| tmux 3.4 | Notification-shaped command output kept in the block body |
| tmux 3.6 | The created-window report when breaking a window's only pane |
| tmux 3.8 | JSON saved layouts, `{"V":2,...` |

Two are tmux defects that a later release fixed, so the affected window is
closed at both ends:

| Broken on | What tmux does |
|---|---|
| tmux 3.4 – 3.5 | Echoes a non-UTF-8 byte back as its octal escape rather than the byte |
| tmux 3.4 | Adds a backslash before `$` when an option value it printed is set again |

A split carrying a percentage is spelled `-l 25%` rather than the `-p 25` that
tmux 3.4 removed, so it works on every supported release. Python libtmux still
emits `-p`.

`Server::run_shell` does not capture the job's output, so the 3.3 to 3.4
`run-shell` stdout defect does not reach it.

## Why a feature is refused

The table above lists each optional feature and the release it needs. The
causes sit here, one per section.

### Control mode

Short-lived control attaches crash 3.2a and 3.6b servers. Control mode is
refused below 3.7, so no such attach is made, and `wait_for_text` polls there.

### Named buffers

Below 3.4, `delete-buffer -b NAME` and `set-buffer -b NAME -n` act on the
newest buffer when `NAME` does not exist. Nothing crashes; the wrong buffer is
deleted or renamed.

### Names with a colon or a period

Exactly 3.7 refuses `:` and `.` in session and window names. On a private 3.7
socket, `new-session -d -s a.b` and `new-window -n a.b` answer "invalid session
name: a.b" and "invalid window name: a.b" and create nothing; `rename-window`
answers the same and changes nothing. The library refuses such a name first,
before dispatch.

### Unreadable layout strings

On tmux 3.3 and 3.3a, a layout string tmux cannot parse ends the server. 3.4
answers "invalid layout". The library refuses such a string before dispatch on
every release.

### Clipboard query

`refresh-client -l` with an attached client ends a 3.3a server at once when the
client's terminal has the `Ms` clipboard capability, such as `xterm-256color` or
`tmux-256color`, and a 3.4 to 3.6 server when the client leaves before its
terminal answers. 3.6a is the first release without the crash.

### Break pane

Exact tmux 3.7 tests the window name inverted, so a `break-pane` without `-n`
on a window with more than one pane ends the server, and a given name is
ignored. 3.7a fixes both ([`CHANGES`](https://github.com/tmux/tmux/blob/3.7a/CHANGES#L3),
[3.7 source](https://github.com/tmux/tmux/blob/3.7/cmd-break-pane.c#L117-L124)).
`Pane::break_out` names the window itself.

## Commands the checks do not read

A literal command is read the way tmux reads it: under its alias or any unique
prefix, and in each position of a `;` list. That holds for `Server::run`,
batches and chains, asynchronous submissions, `Server::over_control` and
`Connection::execute`.

A command carried inside a string that tmux parses itself is sent as written:
the body of `if-shell`, the argument of `run-shell -C`, a brace block, a hook, a
key binding or a sourced file. For example, `if-shell -F 1 'break-pane -d'`
passes the checks unchanged. The affected commands:

| Command | Releases | What happens |
|---|---|---|
| `break-pane` without `-n` | exactly 3.7 | The server exits; 3.7a fixes it |
| `select-layout` with an unparseable string | 3.3, 3.3a | The server exits; 3.4 answers "invalid layout" |
| `refresh-client -l` | below 3.6a, with an attached client | The server exits ([why](#clipboard-query)) |
| `delete-buffer -b NAME`, `set-buffer -b NAME -n` | below 3.4 | The wrong buffer is changed ([why](#named-buffers)) |
| `split-window -p` | exactly 3.4 | tmux answers "size missing"; nothing changes |
| `:` or `.` in a name | exactly 3.7 | tmux answers "invalid session name" or "invalid window name"; nothing changes |

The library does not refuse a raw `split-window -p`; typed splits send
`-l N%`. On exact 3.7, name every `break-pane`, nested or not, or upgrade to
3.7a. The [default `prefix !` binding](https://github.com/tmux/tmux/blob/3.7/key-bindings.c#L358)
is itself a nameless `break-pane`, which no library check can reach.

[ref-version]: https://libtmux.org/en/cxx/latest/reference/libtmux-version/
