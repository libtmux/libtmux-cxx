# Security

## Reporting a vulnerability

Report privately through
[GitHub's advisory form](https://github.com/libtmux/libtmux-cxx/security/advisories/new),
not a public issue. Include the version, the platform and tmux version, and
what an attacker gains — a reproducer is worth more than a description of one.

Expect an acknowledgement within a week. A fix ships in the next release, and
the advisory is published once it is out.

## What is supported

The most recent release. This package is alpha: releases carry an `-alpha`
prerelease tag, the API may change without a deprecation period, and there are
no patch branches for older tags. A fix means a new release, not a backport.

## What is in scope

The library, `libtmux::testing`, and the MCP server in this repository.

Two things are the caller's rather than this package's, and are documented as
such rather than treated as vulnerabilities:

- **A command a caller composes.** `Server::run` and the chain and batch
  surfaces pass argv to tmux as given. A caller building a command from
  untrusted text is choosing what tmux runs.
- **The tmux server's own configuration.** tmux lets a server-side
  `command-alias` redefine a built-in command, so the typed surface's
  guarantees assume a trusted server configuration. The same holds for psmux
  on Windows.

`ExecutionPolicy::tmux_binary` is how a caller stops `PATH` deciding which tmux
runs; a build that leaves it at its default resolves `tmux` the way any other
program would.
