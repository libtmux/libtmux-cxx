# The control connection boundary

`Server` and `Connection` expose different tmux contracts.

`Server` launches one tmux client per invocation. That client remains attached
until its command queue finishes, including foreground jobs and asynchronous
file work, and returns tmux's final exit status. Typed entity operations use
this path.

`Connection` keeps one control client open. It exposes guarded reply blocks and
outside-block events in wire order. This is the right surface for notification
streams, pane output, and protocol tooling. It is not a second typed command
backend.

## Why a guarded block is not a result

tmux may write `%end` before a command that returned `CMD_RETURN_WAIT` has
finished. A job or file callback later writes output or an error without a
guard or request identifier, then continues the queue. `run-shell`,
`load-buffer`, `save-buffer`, and `source-file` all have such paths.

Local command classification cannot repair this:

- whether a command waits can depend on its arguments and tmux version;
- server-global aliases expand during parsing and can replace any command;
- hooks and conditionals may insert more commands; and
- real notifications share the same outside-block stream as delayed output.

Assigning the next unguarded line to a request steals notifications. Treating
every unguarded line as failure rejects valid requests. The library therefore
keeps the evidence raw rather than inventing final status or attribution.

## What `Connection::execute` guarantees

A connection serializes complete writes. Each request ends with a private
boundary, and `execute` returns every guarded block observed before that
boundary. The boundary distinguishes concurrent callers even though tmux's
guards contain no request id.

Deadlines remain caller-relative while waiting to write. A deadline reached
after bytes were dispatched cannot cancel server-side effects; the connection
fails closed because it can no longer prove later reply ownership.

`take_notifications` and `NotificationWatch` expose the bounded outside-block
log. Most entries are tmux notifications. Unknown entries may instead be
delayed command output, so callers must preserve unknown bytes rather than
guess their origin.

## Where the refusal stops

The reasoning above is about commands in general, and it holds there. It does
not reach every command, and the difference is worth stating because it decides
what a caller may build.

A guarded block is an incomplete result only for a command tmux can leave
running after `%end`. Exactly twelve can: `command-prompt`, `confirm-before`,
`display-menu`, `display-message`, `if-shell`, `load-buffer`, `run-shell`,
`save-buffer`, `source-file`, `split-window`, `wait-for`, and the queue itself
(`rg -l CMD_RETURN_WAIT 'cmd-*.c'` in tmux). Being in that list is not the
same as deferring: several do so only under a flag. `display-message` defers
only under `-I`, and `split-window` only under `-I` (read standard input into
the pane) or `-W` (wait for the pane's command to exit) — `cmd-split-window.c`
returns `CMD_RETURN_WAIT` on those two paths and no other. The library sends
neither. Every listing it issues is outside the set outright —
`list-sessions`, `list-windows`, `list-panes`, `list-clients`, `list-buffers`,
`list-commands`. For those commands, sent as the library sends them, the
guarded block is the whole answer.

Alias substitution remains unprovable: `CMD_PARSE_NOALIAS` exists but tmux sets
it only while expanding an alias, so no client can ask for it. It is also not a
difference between the transports, because a subprocess runs the same alias. A
transport that serves listings therefore fails closed — a substituted command
answers something that does not parse as separated rows, and falling back to a
launch costs only the launch it was avoiding.

None of this makes `Connection` a typed command backend. It makes the listing
half of one reachable, which is what `Server::over` and `CommandExecutor` let a
caller build; `tests/executor_seam_test.cpp` does exactly that and matches the
launching path row for row.

## Choosing the surface

| Need | Surface |
|---|---|
| Final command success, typed entities, batches, observers | `Server` |
| Guarded blocks and exact control-wire ordering | `Connection::execute` |
| Notifications, pane output, readiness descriptor | `Connection` |

Sequential `Server` calls preserve program order because one finishes before
the next starts. Concurrent calls are separate tmux clients and promise no
total order. A `Connection` is one FIFO client, so a waiting command also
blocks later work on that connection.
