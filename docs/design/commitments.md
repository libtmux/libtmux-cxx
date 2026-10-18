# What the design commits to

Five commitments the public surface keeps. The [README](../../README.md) links
the types; the reasoning and measurements are in the other notes under
[`docs/`](../README.md).

**An entity is one row of a shared snapshot.** It holds the listing it came
from and the index of its row, so it reads its fields without reaching tmux:
the process ran once, when the snapshot was taken.

**Traversal and mutation go through the entity**, addressed by the id tmux gave
the object, so no operation can be redirected by a name containing a target
separator. A command that creates something returns it; a command that changes
something returns nothing, so nothing is re-read that the caller did not ask
for.

**Filters are values, not expression templates.** A `FilterExpr` owns every
operand it compares against, so an expression built from a local string still
works after that string dies.

**One failure type.** Every call reports `CommandFailure`, including the
factories, so `Server::from_env().and_then(...)` composes. The one exception
follows a rule worth stating: a call reports the failure type its *result*
speaks, so `Server::control` — which hands back a `Connection` — reports
[`ProtocolError`][ref-protocolerror] like the `Connection` does.

**The transport is private, and replaceable.** No process type appears in any
installed header. The suite substitutes a scripted executor and runs the whole
public surface with no tmux present, which is also how the argv each operation
sends is pinned.


[ref-protocolerror]: https://libtmux.org/en/cxx/latest/reference/libtmux-protocolerror/
