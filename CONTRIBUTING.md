# Contributing

Thanks for looking. This is a C++ port of
[libtmux](https://libtmux.git-pull.com), and it is held to the same standard as
the original: nothing merges that is not proven against a real tmux.

The project is an **alpha prerelease**, which is good news for a contributor:
the public surface is still open. A change that improves a name or a signature
does not need a deprecation shim, and "this API is awkward" is a reportable
bug rather than something to work around. That ends at `0.1.0`.

## Getting a build

**Requirements:** a C++23 toolchain (clang 17+ or GCC 13+), CMake 3.25, tmux on
`$PATH`. Nothing else — the library itself has no dependencies.

```console
$ cmake --preset cxx-dev && cmake --build --preset cxx-dev
```

```console
$ ctest --preset cxx-dev --no-tests=error
```

If that passes, you have everything. [`tests/README.md`](tests/README.md)
explains what those tests are and why none of them mock tmux.

## Never share a tmux server

Several libtmux ports run their suites on this kind of machine at once, and a
tmux server is shared state keyed only by its socket. Two suites on one socket
end each other's sessions, and the failure reads like a real bug in whichever
one noticed first.

The fixture already handles this: every test server gets a private tree and its
own socket, with `TMUX` and `TMUX_PANE` erased from the child environment. Use
[`ScopedTmuxServer`](tests/support/scoped_tmux_server.hpp); do not start tmux
another way in a test. For one-off probes, do the same by hand — the rule and
the recipe are in [AGENTS.md](AGENTS.md).

Never run a bare `tmux` command against the default server while working here.
It is somebody's real session.

## What a change needs

**Every capability needs a test against real tmux.** Not a mock, not a recorded
transcript — an integration test that starts a server, does the thing, and reads
back what tmux says happened. That is what makes a passing suite mean anything.

**Errors are returned, never thrown.** The library reports failure as
`expected<T, CommandFailure>`, and that is not a style preference: it is the
whole error model. A `throw` in the library is a design change, not an
implementation detail.

**The typed surface should refuse what tmux would refuse.** If a mistake can be
caught at compile time, catch it there and add a case to
[`tests/compile/`](tests/compile/) proving it stays caught. A refusal that
quietly stops refusing is a silent API regression.

**No dependencies in the core.** The library links nothing. The MCP server is
allowed a JSON parser because it is a program, not a library, and it is off by
default.

## Before you open a pull request

Run what CI runs. The five build lanes disagree often enough to be worth the
time — clang with libc++, GCC with libstdc++, the C++20 build over
`tl::expected`, and the sanitizers:

```console
$ for p in cxx-dev cxx-sanitize cxx-tsan cxx-gcc cxx20; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p --no-tests=error; done
```

```console
$ python3 -m tools.parity verify --manifest tools/parity/data/manifest.json --mode structural --allow-pending
```

```console
$ python3 -m tools.mutate --preset cxx-dev
```

> [!WARNING]
> The mutation runner edits sources in place and puts each back as it goes. Do
> not commit or build from the tree while it runs. A commit taken during a run
> once captured a `load-buffer` without the flag naming its buffer, and the diff
> read like ordinary work.

The [parity ledger](tools/README.md) records what each Python `libtmux` symbol
maps to here and the evidence for it. Adding a capability usually means adding a
row: an entry in `mapping.json`, the test that proves it in `evidence.json`, and
`python3 -m tools.parity sync`. An evidence record cannot cite a test nobody
wrote — every case is resolved in the file it names.

## Style

`.clang-format` and `.clang-tidy` are in the repository and CI enforces both, so
there is nothing to argue about:

```console
$ git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
```

Python tooling is linted by [ruff](https://docs.astral.sh/ruff/), configured in
[`pyproject.toml`](pyproject.toml).

For prose — commit messages, comments, documentation — say what the code does
and why, in plain sentences. No emoji in commits, issues or code.

## Commits

Write the message for someone reading `git log` in a year with no memory of the
discussion. Say what changed and what made it necessary; the diff already shows
how.

Keep the subject under 72 characters and in the imperative. If a change is two
ideas, it is two commits.

## Reporting a bug

Say which tmux version (`tmux -V`), which compiler and standard library, and
which preset. A reproduction that starts its own server is worth more than a
description — [`examples/`](examples/README.md) has the shape of one.

Behaviour that differs from Python libtmux is worth reporting even if this
library's behaviour seems reasonable. Parity is the goal, and a deliberate
divergence should be written down as one.

## Security

Report vulnerabilities privately through
[GitHub's advisory form](https://github.com/libtmux/libtmux-cxx/security/advisories/new)
rather than a public issue.

## License

MIT. By contributing you agree your work is published under it. See
[LICENSE](LICENSE).
