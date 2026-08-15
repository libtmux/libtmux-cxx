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

## Before committing

Five build lanes, the format and reference checks, the parity ledger, and
the mutation catalogue:

```console
$ for p in cxx-dev cxx-sanitize cxx-tsan cxx-gcc cxx20; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p --no-tests=error; done
```

```console
$ python3 -m cxx.tools.parity verify --manifest cxx/parity/manifest.json --mode structural --allow-pending
```

```console
$ python3 -m cxx.tools.mutate --preset cxx-dev
```

The mutation runner edits sources in place and puts each back as it goes.
Do not commit or build from the tree while it runs: a commit taken during a
run once captured a `load-buffer` without the flag naming its buffer, and
the diff read like ordinary work.
