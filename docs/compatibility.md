# Compatibility

The tmux releases the library accepts, how it refuses the rest, and what it
cannot promise. The [README](../README.md#compatibility) keeps the floor, the
capability table and the known defects; this page is the rest of the contract.

## The floor

tmux 3.3a and newer. The floor is the complete release: tmux 3.3 is below it.
[`Version`][ref-version] orders releases correctly: `3.7 < 3.7a < 3.7b`, and `next-X.Y` and
`X.Y-rcN` precede `X.Y`. CI builds and tests pinned compatibility cells from
3.3a through 3.8, plus `master`, on every pull request and master change.

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

## Why a feature is refused

The README lists each optional feature and the release it needs. The causes
sit here, one per section.

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

The literal-command checks read a command's own words. A command carried inside
a string that tmux parses itself is sent as written: the body of `if-shell`,
the argument of `run-shell -C`, a brace block, a hook, a key binding or a
sourced file. For example, `if-shell -F 1 'break-pane -d'` passes the checks
unchanged. The affected commands:

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
