# Tests

Against a real tmux, on every build. `ctest -N` counts them.

```console
$ ctest --preset cxx-dev --no-tests=error
```

```console
$ ctest --preset cxx-dev -R libtmux.buffer --output-on-failure
```

Nothing here mocks tmux. A test that says a pane received text starts a tmux
server, sends the text, and reads the pane back. The cost is that the suite
needs tmux on `$PATH`; the benefit is that a passing test means the thing
works against the program people actually run.

## Every server is private

[`support/scoped_tmux_server.hpp`](support/scoped_tmux_server.hpp) is the only
way a test starts tmux. It makes a private directory with `mkdtemp` at
`$TMPDIR/libtmux-cxx-test-XXXXXX`, points `TMUX_TMPDIR` inside it, addresses
the server by a socket under that tree, and erases `TMUX` and `TMUX_PANE` from
the child environment so a suite run from inside tmux cannot reach the outer
server. It kills the server and removes the tree on the way out, including
when a test fails.

This matters more than it looks: several libtmux ports run their suites on one
machine, and a tmux server is shared state keyed only by its socket. Two suites
on one socket end each other's sessions, and the failure surfaces as a bug in
whichever noticed first. See [AGENTS.md](../AGENTS.md).

## What each kind proves

| Directory | Kind | What it is for |
|---|---|---|
| `*_test.cpp` | Unit and contract | One subsystem each: entities, options, cardinality, relations, the control parser, value semantics. |
| [`integration/`](integration/) | Behaviour | One case per capability, against real tmux. These are what the [parity ledger](../tools/README.md) cites as evidence, so each is named for the claim it makes. |
| [`compile/`](compile/) | Refusals | Programs that **must not compile**. `pane::active.starts_with(...)` and a view outliving its snapshot are errors, and these prove it rather than asserting it in prose. |
| [`fuzz/`](fuzz/) | Parsers | libFuzzer over everything that reads what tmux printed. |
| [`differential/`](differential/) | Cross-implementation | The same question asked of this library and of Python libtmux, compared. |
| [`data/`](data/) | Recordings | Captured tmux output, so a parser can be tested without a server. |

## The compile tests are the interesting ones

Most suites check what the library does. [`compile/`](compile/) checks what it
refuses to let you write:

```cpp
// tests/compile/string_field_has_no_ordering.cpp — must not compile
auto wrong = window::name < "b";
```

Each is built as a target that CMake expects to fail. A refusal that stops
being a refusal is a silent API regression, and this is the only way to notice.

## Running more than one configuration

The suite runs under every preset in
[`CMakePresets.json`](../CMakePresets.json), and they disagree often enough to
be worth the time:

```console
$ for p in cxx-dev cxx-sanitize cxx-tsan cxx-gcc cxx20; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p --no-tests=error; done
```

clang with libc++, GCC with libstdc++, the C++20 build over `tl::expected`,
address and undefined-behaviour sanitizers, and the thread sanitizer.

## Proving a test can fail

A green suite is evidence only once it has been shown capable of failing. The
[mutation catalogue](../tools/README.md) breaks one guard at a time and reports
any that nothing notices:

```console
$ python3 -m tools.mutate --preset cxx-dev
```

A survivor means something is untested. So does a mutation that would not
apply or would not build — that one is reported as *not a result* rather than
counted as a pass.

## Related

- [The library](../README.md)
- [`examples/`](../examples/README.md) — where behaviour is shown rather than pinned
- [`tools/`](../tools/README.md) — the ledger and the mutation runner
