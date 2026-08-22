# Windows through psmux

tmux does not build for Windows, so the port reaches a Windows terminal
through [psmux](https://github.com/psmux/psmux), a separate program that
speaks a tmux-shaped command line. The backend behind it is a preview, and it
is bounded on purpose: it serves what it can prove and refuses the rest.

## Why a call reports `unsupported`

Where psmux does not answer the way tmux does, the backend refuses the call
instead of returning a near-enough answer. The refusal is
`FailureKind::unsupported`, which says nothing was dispatched — a different
thing from tmux running the command and rejecting it.

`ServerCapabilities` says the same in the type system. It describes the backend
compiled in, not the executable on `PATH`, and a backend it does not recognise
reports no features, so `supports()` returns false for everything rather than
assuming. A caller asks what a route serves before choosing one.

## What the preview serves

- Exact session, window and pane listings, traversal, refresh, and format
  reads.
- `Server::window` and `Server::pane` without an owning session handle.
- Typed session creation with an attributable result.
- Cleanup of the selected namespace under the server's execution policy.

## What it refuses

- **Typed option and hook access.** psmux keeps that state in separate
  per-session servers and cannot target it atomically, so a read or write
  cannot be attributed to the scope the caller named.
- **Control mode.** `control_with_options` and `over_control_with_options` are
  rejected before a control client is launched.
- **Pane input, capture, search and streaming**, which is why the MCP server
  advertises only `inspect_tmux`, `list_sessions`, `list_windows` and
  `list_session_panes` there.

## An id is only unique beside its session

A psmux session keeps its own server, so a pane or window id repeats across
sessions. `same_entity_id` compares the session id as well on Windows and the
id alone on POSIX, which keeps entity equality meaning the same thing on both.

## Addressing a server

Socket names only. `kSocketPathLimit` is zero where paths do not apply, and
asking for one reports `SocketError::path_unsupported`. `Server::at_socket_name`
emits the separated `-L name` form psmux parses, and `Server::from_env`
recovers that selector inside a psmux pane.

An explicit `-L default` is rejected: psmux gives it the same environment
identity as its unselected default, so accepting it would let two spellings
address one server while looking like two.

## Two environment hazards

An argument carrying `;`, a carriage return or a newline is refused before
dispatch — psmux's command line would otherwise read it as a second command.

WSL can launch the native library with a `PATHEXT` that omits `.EXE`, which
stops psmux finding PowerShell or `cmd`. The subprocess backend repairs only
the psmux child's copy and preserves any custom extensions the caller set.

## What is not packaged

The vcpkg port declares `!windows & !mingw` until a tagged release clears the
Windows publication gate. MinGW, UWP, Xbox and non-x64 Windows triplets stay
excluded regardless; the packaged targets are Linux, macOS, and — once that
gate passes — x64 desktop Windows.
