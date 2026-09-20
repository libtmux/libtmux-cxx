# What this library's own costs are

Two small programs, neither linked into the shipped package
(`LIBTMUX_INSTALL` never touches them; `LIBTMUX_BUILD_BENCHMARKS` gates
building them at all):

```console
$ cmake --preset cxx-dev -DLIBTMUX_BUILD_BENCHMARKS=ON
$ cmake --build --preset cxx-dev --target libtmux_matrix libtmux_operations_bench
```

## [`apps/matrix/`](apps/matrix/README.md) - command dispatch

What each way of reaching tmux costs: one process per command, a `Chain`
batched into one, and the same build over held-open control clients through
`Server::over_control` — one client, a pool of four, and chained over one.
`--check` asserts the table's own claims (every lane must answer the same query
about the same topology); `--json` prints the same rows as one document. See
its own README for what each lane means.

```console
$ ./build/cxx-dev/apps/matrix/libtmux_matrix
```

## [`benchmarks/operations.cpp`](benchmarks/operations.cpp) - everything dispatch rides under

Three costs that sit on top of a single dispatch:

- **Listing depth** - `sessions()`, `windows()`, `panes()` against the same
  fixture, each reading more rows than the one before it, plus one raw
  `list-panes -a` round trip for scale (tmux's own default format, not the
  fields the typed decoder reads, so it bounds decode cost rather than
  isolating it).
- **Control-mode notification throughput** - opens a connection with
  `pane_output` on, resumes delivery for one pane, and drains a 2000-line
  burst, reporting notifications, bytes and effective KiB/s.

```console
$ ./build/cxx-dev/benchmarks/libtmux_operations_bench
```

Dispatch over a control connection is measured in `matrix`, not here, and
only through `Server::over_control`, which sends a command over the wire only
where tmux answers it completely inside its guarded block. A raw
`Connection::execute()` completes at that boundary whatever the command, so on
its own it answers a different question than every other row here.

## Reading the numbers

Both programs print wall-clock milliseconds from a machine and load this
repository does not control, so a number here is a shape, not a promise: run
them yourself before trusting a comparison.

There is deliberately no time budget to regress against, because a wall clock
on a shared runner would fail for reasons that have nothing to do with this
library. What *is* gated is the process column, which is deterministic and the
same everywhere: `matrix --check` asserts that every lane answers the same
query, that the process lane makes exactly one tmux invocation per command,
that the chained lane makes exactly one, and that the three connection lanes
make none. An inequality would not have done —
`chained < process` stays true when a stray launch is added to the typed path,
and the exact counts catch it. The expectation is derived from the workload, so
changing the workload cannot leave a stale number behind.

Both start their own private tmux server (`libtmux::testing`) and never touch
the one you are sitting in.
