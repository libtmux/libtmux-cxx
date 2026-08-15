# Dispatching over a control connection

The library talks to tmux through one private interface. The default
implementation launches tmux per command. The second keeps one control-mode
client open and writes commands to it, so a command costs a write and a read
rather than a process.

`Server::over_control(session)` returns a Server whose entities dispatch that
way. Nothing above the transport changes: the entities, filters, options and
failures are the same types, and the same test file drives both.

## What it is worth

One hundred `windows()` listings against a server holding 21 windows,
release build, tmux 3.7b:

| transport | per listing | rows returned |
|---|---|---|
| subprocess | 8.3 ms | 2100 |
| control connection | 1.8 ms | 2100 |

Both answers are identical; only the way of asking differs.

## What it costs

**One conversation at a time.** Control mode matches replies to commands by
order, so the backend holds a mutex across a command. Two Servers over the
same socket are two conversations and do not contend. A caller who wants
parallelism opens more connections, which is the honest shape of the protocol
rather than a limitation of this class.

**A different way to ask the same question.** `tmux -V` is a flag of the
binary, not a command a connection can carry, so the control backend reads
`#{version}` as a format instead. That is why the version question lives on the
transport interface: only the transport knows how to ask it.

**Attribution instead of exit codes.** A subprocess reports an exit status; a
control connection reports a reply block that ends in `%end` or `%error`, and
an attribution saying whether that block belongs to the command that was sent.
A block that cannot be attributed is reported as a timeout with
`dispatched` set, because the command reached tmux and what it did is unknown —
the same answer a subprocess timeout gives, for the same reason.

**A connection can die.** The subprocess transport fails one command at a time.
A broken connection fails every later command, and reports the transport
failure rather than a tmux refusal.
