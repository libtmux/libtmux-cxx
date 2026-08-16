# Working in `cxx/`

Guidance specific to this port. The repository root `AGENTS.md` still
applies.

## Never share a tmux server with anything else

Several libtmux ports live on this machine at once — other languages, other
checkouts, each running its own test suite. They all talk to tmux, and a
tmux server is shared state keyed only by its socket. Two suites on one
socket kill each other's sessions and produce failures that look like real
bugs in whichever one noticed first.

So every tmux this workspace starts gets a socket of its own, under a
directory named for the workspace:

- **Tests** already do this. `ScopedTmuxServer` makes a private tree with
  `mkdtemp` at `$TMPDIR/libtmux-cxx-test-XXXXXX`, points `TMUX_TMPDIR`
  inside it, and addresses the server with `-L server` or `-S <tree>/socket`
  under that tree. It erases `TMUX` and `TMUX_PANE` from the child
  environment so a suite run from inside tmux cannot reach the outer server.
  Use the fixture; do not start tmux another way in a test.
- **Spikes and one-off probes** must do the same by hand:

```console
$ export TMUX_TMPDIR="$(mktemp -d -p /tmp libtmux-cxx-spike-XXXXXX)"
```

  then `unset TMUX TMUX_PANE`, address the server with `-L` or `-S` under
  that directory, `kill-server` when finished, and remove the tree.

Never run a bare `tmux` command against the default server while working
here. It is somebody's real session, and a stray `kill-server` takes their
work with it.

The socket path has to fit in `sun_path`, so keep the prefix short;
`socket_path_fits` in the fixture reports the case where it does not rather
than letting tmux fail obscurely.

## The parity ledger is read here, not written

The ledger in `tools/parity/data` records what each Python `libtmux`
symbol maps to and the evidence for it. Two halves of the tooling read it,
and only one of them can run in this repository:

- `verify`, `gaps`, `coverage` and `record-evidence` read the recorded
  artifacts and this tree. They work, and the gate below runs them.
- `generate` and `drift` observe the Python library at a pinned commit to
  produce the ledger in the first place. They need that repository's files
  *and its history* — `inputs.json` pins `src/libtmux`, `conftest.py` and
  `pyproject.toml` by git object, none of which exist here.

So the recorded surface is frozen at the commit it was taken from. Adding
a classification means editing `mapping.json` and `evidence.json` and
re-running `sync`, which is what every entry so far has done anyway.
Regenerating from a newer Python release means doing it where that
release lives.

## Before committing

Five build lanes, the format and reference checks, the parity ledger, and
the mutation catalogue:

```console
$ for p in cxx-dev cxx-sanitize cxx-tsan cxx-gcc cxx20; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p --no-tests=error; done
```

```console
$ python3 -m tools.parity verify --manifest tools/parity/data/manifest.json --mode structural --allow-pending
```

```console
$ python3 -m tools.mutate --preset cxx-dev
```

The mutation runner edits sources in place and puts each back as it goes.
Do not commit or build from the tree while it runs: a commit taken during a
run once captured a `load-buffer` without the flag naming its buffer, and
the diff read like ordinary work.

## Comments earn their maintenance cost

Keep an implementation comment only when losing it would force a future
maintainer to rediscover a consequential, non-obvious fact that the code,
types, assertions, and tests do not already communicate. It states a
durable truth about the shipped system rather than the author's reasoning,
and it does not restate a value or a fact that can change without it — a
comment that duplicates either goes stale silently. Write it as tersely as
a mature, long-lived library would.

Delete comments that narrate, restate, speculate, excuse, or preserve
development history, and prefer deletion in the borderline case. What
survives is what a reader could not recover from the code.

Doc comments on the public headers — summaries, parameter descriptions, and
examples — are judged on the other axis: what they are worth to a caller,
not whether they are non-obvious. They stay precise, succinct, and
maintainable.
