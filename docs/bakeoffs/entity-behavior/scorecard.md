# Where entity behaviour attaches

A snapshot row can be read. Reaching tmux from it — `session.windows()`,
`pane.send_text(...)` — needs the row to know the connection that produced it.
Five ways to arrange that were built against a real tmux server and measured.

## Contenders

**Operations on Server.** Rows stay inert; `server.windows_of(session)`,
`server.rename(window, name)`. No lifetime question at all, and no way to write
what the design promised: traverse the hierarchy and act through it.

**Bound view.** The row carries a `const Server*`, and the list that produced
it owns the server copy that pointer refers to. Ergonomic, but an entity is
only valid while the list it came from is, so it cannot be returned or stored.

**Owning value.** The entity copies its field strings and shares the
connection. Storable, no rules to remember, one allocation per field per row.

**Handle layer.** Rows stay inert; a separate `WindowHandle{server, id}` type
carries identity and is the only thing that can act. Two vocabularies for one
tmux object, and every traversal converts between them.

**Shared snapshot.** The entity is a `shared_ptr` to the snapshot plus a row
index. Storable like an owning value, allocation-free like a view.

## What the measurements said

Allocations for one listing on a real server, counted through a global
`operator new`. The view column was measured before that design was replaced.

| | bare `run` | row view | shared snapshot | owning value |
|---|---|---|---|---|
| 21 windows | 254 | 293 | 293 | 315 |
| 201 windows | 254 | 476 | 477 | 679 |
| entity size | — | 16 bytes | 24 bytes | 40 bytes |

One tmux invocation costs 254 allocations before any entity exists, and the
process dominates the wall clock by two orders of magnitude. Sharing the
snapshot costs nothing measurable over viewing it; owning every field costs one
allocation per row. Entity representation is not where this library's cost
lives.

That settled it. The zero-allocation view was buying an unmeasurable win at the
price of an entity that cannot outlive the call that produced it — in a library
whose consumers are a tool server and a workspace builder, both of which hold
objects across calls. The shared snapshot was selected.

## What the choice bought

`EntityList` is gone: a listing returns `std::vector<Session>`, which composes
with the standard algorithms and needs no wrapper to keep storage alive. An
entity is 24 bytes, copies for one reference count, and keeps alive both the
bytes it reads and the connection it acts through, so there is no rule about
what must outlive what.

What it costs: one stale pane keeps its whole listing alive, and an entity
reads the moment it was listed, not the present. Both are visible in the API —
`refresh` exists and returns a new value rather than mutating the old one.

## Grafts taken from the losing designs

**Lookup by target**, from the handle layer. A tool server receives `"%3"` from
a model and has no entity yet. `Server::pane(target)` answers with one command
rather than a listing and a scan.

**Recorded snapshots**, from the owning value. `Snapshot::from_recording` reads
literal tmux output into a snapshot with no connection behind it. Filters and
field accessors work; anything that would run a command reports that there is
no server, which keeps offline tests and replayed output on the same code path
as live listings.

## The tmux behaviour that shaped the result

`display-message -p -t <target>` answers a format query about any object, which
makes every to-one link and every lookup one command instead of a listing and a
filter. It also answers a query about an object that no longer exists by
printing empty fields and exiting zero:

```console
$ tmux display-message -p -t @999 '#{window_id}#{window_name}'
```

That prints an empty line. `list-windows -t nosuch` exits 1 and says `can't
find session`, but the cheap single-command path fails silently. Every use of
it checks that the identity column came back non-empty and reports a missing
object, because the alternative is an entity whose id is the empty string that
later targets whatever tmux thinks the empty string means.
