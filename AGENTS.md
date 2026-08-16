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

- **Tests** already do this. `ScopedTmuxServer`, in
  `include/libtmux/testing/scoped_server.hpp`, makes a private tree with
  `mkdtemp` at `$TMPDIR/<namespace>-XXXXXX`, points `TMUX_TMPDIR` inside it,
  and addresses the server with `-L <namespace>` or `-S <tree>/socket` under
  that tree. It erases `TMUX` and `TMUX_PANE` from the child environment so a
  suite run from inside tmux cannot reach the outer server. Use the fixture;
  do not start tmux another way in a test.

  It is shipped, as `libtmux::testing` — a separate target behind
  `find_package(libtmux COMPONENTS testing)`, so the examples' own suite and
  any outside consumer run on the same one rather than reinventing it. That
  makes the namespace load-bearing rather than cosmetic: pass
  `SocketNamespace::consumer("your-suite")` so a stray directory names who
  left it.
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

That limit is 104 bytes on macOS against 108 on Linux, and macOS spends around
sixty of them on `$TMPDIR` alone — `/var/folders/...` canonicalises to
`/private/var/...`. Ten bytes of socket name, or one nested directory, is the
difference between passing every Linux lane and failing there with only "File
name too long". Run the suite under a directory as long as macOS gives before
believing a green board:

```console
$ BASE=/tmp/$(printf 'm%.0s' $(seq 1 52)) && mkdir -p "$BASE" && TMPDIR="$BASE" ctest --preset cxx-dev --no-tests=error; rmdir "$BASE"
```

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
