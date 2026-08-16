# Testing the examples

A separate project, on purpose.

Everything here arrives through `find_package(libtmux COMPONENTS testing)`.
There is no path into the library's build tree, no private header, and no
compile definition carrying an answer the library's own configure step already
worked out. If this suite can test tmux code with nothing but the installed
package, so can anybody — and that claim is only worth something if it is
checked the hard way, from outside.

```console
$ cmake -S examples -B build/examples -DCMAKE_PREFIX_PATH="$PWD/build/prefix"
```

```console
$ ctest --test-dir build/examples --output-on-failure
```

The parent build also adds this directory, so `ctest --preset cxx-dev -R
examples` runs it in-tree. Both paths compile the same sources; only the first
proves the package.

## What it asserts

| Per example | Why it is not covered elsewhere |
|---|---|
| Exits zero | The in-tree `example.*` tests already do this |
| Says what the prose claims | An example can exit zero and print nothing |
| Leaves no server and no directory | Nothing checked this before |

The third is the one worth having. Every example takes its private tree from
`ScopedTmuxServer`, and a fixture tree lives under `$TMPDIR` — so the harness
points `TMPDIR` at a directory it owns, and afterwards that directory is either
empty or the example leaked. There is no third possibility, which is what makes
the assertion worth writing.

## Namespaces

Every server started here is filed under a label that reaches the socket path
and the `tmux -L` name:

```console
$ ls -d /tmp/libtmux-cxx-*
```

```
/tmp/libtmux-cxx-examples-KUk4sp
/tmp/libtmux-cxx-workspace-X1Nyxi
```

A stray directory names the suite that left it. That matters on a machine
running several libtmux ports at once: a server nobody can attribute is a
server nobody dares kill.

Pass one with `SocketNamespace::consumer("your-suite")`, or set
`LIBTMUX_EXAMPLE_NAMESPACE` to relabel a run of the examples without rebuilding
them.

## The duplication is deliberate

[`run_program.hpp`](run_program.hpp) re-implements a subprocess runner that
`libtmux::testing` already has a far better version of — pidfd reaping,
descriptor accounting, interposed-syscall failure injection — and does not
export.

That is the right trade. A tmux fixture is not a subprocess library and should
not become one because a test needed forty lines. Writing those forty lines
here is also part of what is being tested: it is what an outside project would
have to write, so it should be visible.

## Related

- [`../README.md`](../README.md) — the examples themselves
- [`../../tests/README.md`](../../tests/README.md) — the library's own suite,
  which runs on the same fixture
- [`../../include/libtmux/testing/scoped_server.hpp`](../../include/libtmux/testing/scoped_server.hpp)
  — what `libtmux::testing` exports
