# Dispatching over a control connection

The library talks to tmux through one private interface. The default
implementation launches tmux per command. The second keeps one control-mode
client open and writes commands to it, so a command costs a write and a read
rather than a process.

`Server::over_control(session)` returns a Server whose entities dispatch that
way. Nothing above the transport changes: the entities, filters, options and
failures are the same types, and the same test file drives both.

One request may produce several reply blocks. Command aliases, `source-file`,
and commands that insert other commands all do this. The low-level
`Connection::execute` returns those blocks in wire order. The Server surface
joins successful bodies, matching subprocess output without pretending tmux
identified which input operation produced each block.

## What it is worth

One hundred `windows()` listings against a server holding 21 windows,
release build, tmux 3.7b:

| transport | per listing | rows returned |
|---|---|---|
| subprocess | 8.3 ms | 2100 |
| control connection | 1.8 ms | 2100 |

Both answers are identical; only the way of asking differs.

## What it costs

**A private end boundary.** tmux guards contain a time, a global command
number, and flags, but no request or command-group identity. Equivalent grouped
and separate commands can therefore produce identical guards. The connection
appends one random unknown command to each request and completes on that exact
guarded parse error. This costs one extra reply block per request. The random
name prevents accidental alias collisions; it is not a security boundary
against a same-user process that can inspect or alter the connection.

**Concurrent callers keep separate deadlines.** A mutex orders complete writes
to the shared stream, then releases. Callers may wait for their own private
boundaries concurrently, and one caller timing out before it acquires the
writer does not consume another caller's deadline or poison its request.

**A different way to ask the same question.** `tmux -V` is a flag of the
binary, not a command a connection can carry, so the control backend reads
`#{version}` as a format instead. That is why the version question lives on the
transport interface: only the transport knows how to ask it.

**Blocks instead of invented operation IDs.** A subprocess reports one exit
status. A control request reports every `%end` or `%error` block before its
private boundary. tmux does not expose the parse group's internal operation
IDs, so the library preserves the evidence it has rather than assigning blocks
to operations by a guessed count.

**A connection can die.** The subprocess transport fails one command at a time.
A broken connection fails every later command, and reports the transport
failure rather than a tmux refusal.
