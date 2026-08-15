# Spike deletion freeze

Before deleting `cxx/spikes/`, this records what that tree still provides and
what would be lost with it. The answer changed the plan.

## What the suite actually covers

Of 228 registered tests:

| Bound to | Tests |
|---|---|
| `cxx/spikes/transport/*` (contract and contenders) | 131 |
| `cxx/spikes/grafts/*` (control mode, engine ops) | 50 |
| `cxx/tests/support` fixture self-tests | 40 |
| The production library and its consumers | 5 |
| Other | 2 |

Deleting the spike tree today removes 181 tests and leaves the production
library with five. That is the finding: the retained acceptance contract has
not been re-established against `libtmux::libtmux`. It still lives against the
prototypes.

## Why that is not merely bookkeeping

The transport contract suite is where process behaviour is actually pinned —
spawn and pre-exec failures kept distinct, timeout certainty, descriptor leaks,
byte-preserving capture, concurrency under thread sanitizer. `Server` inherits
that behaviour by construction, because it calls the same kernel, but nothing
would prove it after the prototypes that carry those tests are gone.

The same holds for the control-mode graft. Its connection now ships in the
library, and one production test exercises a reply block against real tmux. The
other fifty cover shutdown races, writer ownership, partial writes, and
attribution after connection loss. Those behaviours are in the shipped code and
would go untested.

## What deletion also breaks

`cxx/docs/bakeoffs/transport/decision.json` records a source digest for each
contender, and `verify_decision` re-derives those digests from the live files.
Once the contenders are gone the decision cannot be re-verified from the tree,
which is why the plan calls for a single audited deletion that supersedes
live-file verification rather than an ordinary `rm`.

## Revised order

1. Re-point the transport contract suite at `libtmux::libtmux`, so process
   behaviour is pinned against the shipped kernel rather than the prototype
   that shares its source.
2. Re-point the control-mode graft tests the same way.
3. Correct the `control_mode_release_deadline` rationale, whose cause is
   recorded in `control-mode-writer-residency.md`, and re-freeze the decision.
4. Delete the contenders and grafts, and record the audit that replaces
   live-file verification for the decision.
5. Re-run both hard gates against the result.

## Progress

Step 1 has begun. `libtmux.server_contract` pins, against the shipped library,
the behaviours the transport suite proved against the prototype: arguments
reach tmux without a shell, output bytes survive, a nonzero exit is a reply
carrying its code and diagnostic, an unreachable socket fails rather than
hangs, and repeated runs leak no descriptors.

The error taxonomy that suite could not reach is now public. `CommandFailure`
carries a `FailureKind`, so a caller can tell a rejected argument from a spawn
failure from a timeout from a refusal, and `run` takes a per-call timeout. The
timeout rides on the call rather than the server because how long a caller
waits is a property of what they asked for.

Two behaviours are pinned that the prototype suite proved indirectly: a
timeout is its own kind and still reports `dispatched`, since tmux may already
have acted; and an empty batch is `validation` with `dispatched` false, which
no tmux refusal can be confused with.

Step 2 is done. The control-mode contract — parser framing and the connection's
shutdown races, writer ownership, partial writes, and attribution after a lost
connection — now runs against `libtmux::libtmux`.

One adaptation was needed rather than a straight port. The prototype pinned one
exact tmux binary by digest, which is right for frozen bakeoff evidence and
wrong for a library claiming a version range: it would refuse to run anywhere
else. The library suite resolves tmux, records which binary it ran against, and
requires a version the library actually supports.

The rest is bookkeeping that depends on the work above.
Deleting before them would trade a verified library for a smaller repository.
