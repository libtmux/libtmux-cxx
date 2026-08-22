# Documentation

Design notes, measurements, and the generated reference. Nothing here is
required reading to use the library — [the README](../README.md) is — but this
is where the reasoning lives when you want to know why something is the way it
is.

## Reference

**[`api.md`](api.md)** — every public type and call, with the prose from its
header. Generated, and continuous integration fails if it drifts:

```console
$ python3 tools/docs/api_index.py --include include/libtmux --output docs/api.md
```

**[`api-testing.md`](api-testing.md)** — the same for `libtmux::testing`, the
tmux fixture installed alongside the library:

```console
$ python3 tools/docs/api_index.py --include include/libtmux/testing --output docs/api-testing.md --page testing
```

## The MCP server

**[`apps/mcp/README.md`](../apps/mcp/README.md)** — the twelve tools, the five
protocol versions it negotiates, JSON-RPC batching, progress and cancellation,
how a socket is selected, and what a client is told when a call fails. It is a
separate program in its own package, and nothing links it.

## Packaging

**[`vcpkg-registry.md`](vcpkg-registry.md)** — this repository is a vcpkg git
registry. What that means, the order a release has to keep, and the check that
catches a versions database drifting from the ports beside it:

```console
$ python3 -m tools.vcpkg check
```

## Design notes

Why a thing is the way it is, written when the decision was made.

| Note | Settles |
|---|---|
| [`design/pane-output-streaming.md`](design/pane-output-streaming.md) | What a control connection may subscribe to, and what tmux does when the reader falls behind |
| [`design/control-transport.md`](design/control-transport.md) | Dispatching entities over one held-open connection, and what the protocol asks for in return |
| [`design/cxx20-fallback.md`](design/cxx20-fallback.md) | Why the C++20 build exists, and what the second ABI namespace costs |
| [`design/engine-ops-study.md`](design/engine-ops-study.md) | What the Python operations experiment taught this library, and what was declined |
| [`design/parity-gaps.md`](design/parity-gaps.md) | What the Python surface has that this does not, yet |
| [`design/workspace-corpus.md`](design/workspace-corpus.md) | What running real tmuxp documents through the surface found |
| [`design/windows-psmux.md`](design/windows-psmux.md) | What the Windows preview serves, what it refuses, and why refusing beats a near-enough answer |
| [`design/bakeoff-and-rewrite.md`](design/bakeoff-and-rewrite.md) | How the comparisons below were run |

## Bakeoffs

Where a decision was measured rather than argued.

| Bakeoff | Question |
|---|---|
| [`bakeoffs/entity-behavior/scorecard.md`](bakeoffs/entity-behavior/scorecard.md) | The five ways an entity could have reached tmux, and what measuring them settled |
| [`bakeoffs/transport/scorecard.md`](bakeoffs/transport/scorecard.md) | Subprocess against control mode, per listing and at scale |

## Evidence

What was checked, by whom, and what it found — including the parts that were
wrong.

| Record | Covers |
|---|---|
| [`evidence/library-review.md`](evidence/library-review.md) | The adversarial reviews and their findings |
| [`evidence/prerelease-audit.md`](evidence/prerelease-audit.md) | Eight independent passes over the package before its first release |
| [`evidence/contract-and-harness-review.md`](evidence/contract-and-harness-review.md) | The contract tests and the harness that runs them |
| [`evidence/control-mode-writer-residency.md`](evidence/control-mode-writer-residency.md) | Where the control-mode writer lives, and why |
| [`evidence/transport-decision-audit.md`](evidence/transport-decision-audit.md) | Whether the transport decision survived scrutiny |
| [`evidence/session-review.md`](evidence/session-review.md) | Session-level review notes |
| [`evidence/spike-deletion-freeze.md`](evidence/spike-deletion-freeze.md) | What was deleted after the spikes, and what was kept |

## Decisions

[`decisions/`](decisions/) holds numbered decision records — the short,
dated kind, written once and not revised.

- [`0001-what-a-gate-run-proves.md`](decisions/0001-what-a-gate-run-proves.md)

## Plans

[`plans/`](plans/) is the working history: what was intended, in order, before
it was built. Kept because it explains the shape of what exists, not because it
describes the present.

## Related

- [The library](../README.md)
- [`include/libtmux/`](../include/libtmux/README.md) — the surface the reference documents
- [`tools/`](../tools/README.md) — what generates and checks the files here
