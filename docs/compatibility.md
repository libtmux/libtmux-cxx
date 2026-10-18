# Compatibility

The tmux releases the library accepts, how it refuses the rest, and what it
cannot promise. The [README](../README.md#compatibility) keeps the floor, the
capability table and the known defects; this page is the rest of the contract.

## The floor

tmux 3.3a and newer. The floor is the complete release: tmux 3.3 is below it.
[`Version`][ref-version] orders releases correctly: `3.7 < 3.7a < 3.7b`, and `next-3.8` and
`3.8-rc2` precede `3.8`. CI builds and tests pinned compatibility cells from
3.3a through 3.8, plus `master`, on every pull request and master change.

## How a release is refused

A command is checked against the version of the daemon it reaches. The
first command to a server asks the daemon for its version without running a
hook, and a release below the floor, an unreadable version, `next-3.3a` and an
undated `master` are refused with `FailureKind::unsupported` before the command
has any effect. A tmux older than 3.2 cannot run that probe: a 3.0a daemon
answers "server version is too old for client", a 3.1c daemon prints the
`run-shell` usage, and a 3.1c or older client rejects `-N`. All three are
refused the same way, promptly, though those daemons keep the client's stdout
open after it exits. The message says to upgrade tmux and restart the server,
or stay on the previous libtmux release. Every later command to that daemon
carries a check that fails the whole command list if another daemon has taken
its place. `-V` and `-h` stay available below the floor as diagnostics and
authorize nothing, and `tmux_version()` reports a daemon from 3.2 up that is
below it. Cleanup of an endpoint that is already gone, `kill-server` included,
is a no-op. A program that launches tmux itself asks `Server::check_release()`
first, which refuses a release below the floor as the first command would and
sends nothing.

This assumes tmux's built-in commands keep their meaning: a `command-alias`
that redefines one of the commands the library uses to ask is outside what is
qualified.

[ref-version]: https://libtmux.org/en/cxx/latest/reference/libtmux-version/
