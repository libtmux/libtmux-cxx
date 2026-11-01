# Tests

Behaviour tests against a real tmux, plus contract and parser checks.
`ctest -N` lists the configured tests.

```console
$ ctest --preset cxx-dev --no-tests=error
```

```console
$ ctest --preset cxx-dev -R libtmux.buffer --output-on-failure
```

A test that says a pane received text starts a tmux server, sends the text,
and reads the pane back. Contract tests use scripted executors to check argv,
and parser tests also use recorded output. The full suite needs tmux on
`$PATH`; scripted and recorded inputs do not establish real-tmux behaviour.

## Every server is private, and it ships

[`ScopedTmuxServer`](../include/libtmux/testing/scoped_server.hpp) is the only
way a test starts tmux. It makes a private directory with `mkdtemp` at
`$TMPDIR/libtmux-cxx-test-XXXXXX`, points `TMUX_TMPDIR` inside it, addresses
the server by a socket under that tree, and erases `TMUX` and `TMUX_PANE` from
the child environment so a suite run from inside tmux cannot reach the outer
server. It kills the server and removes the tree on the way out, including
when a test fails.

This matters more than it looks: several libtmux ports run their suites on one
machine, and a tmux server is shared state keyed only by its socket. Two suites
on one socket end each other's sessions, and the failure surfaces as a bug in
whichever noticed first. See
[`.github/CONTRIBUTING.md`](../.github/CONTRIBUTING.md).

Which is why it is not kept here. It is `libtmux::testing`, installed with the
package and documented in [`docs/api-testing.md`](../docs/api-testing.md), so a
consumer's suite can use the same fixture instead of writing a worse one:

```cmake
find_package(libtmux REQUIRED COMPONENTS testing)
target_link_libraries(your_tests PRIVATE libtmux::testing)
```

Every server it starts is filed under a namespace that reaches the socket path
and the `tmux -L` name, so a stray directory says which suite left it —
`/tmp/libtmux-cxx-examples-KUk4sp` rather than one more `tmux-1000` nobody
dares delete. This suite uses the default; pass your own with
`SocketNamespace::consumer("your-suite")`.

The framework-free half is deliberate. `libtmux::testing` names no test
framework, so a suite on Catch2 or doctest uses the same fixture;
[`capabilities.hpp`](../include/libtmux/testing/capabilities.hpp) carries the
GoogleTest skip macros and is a header you choose to include.

`support/` is what is left: the headers that mean nothing outside this
repository — descriptor accounting, `/proc` platform guards, the differential
wire codec, and the fake tmux the fixture's own failure tests drive.

## Nothing selects a server

A command that names no server follows `TMUX` to the one its caller is inside,
or lands on the default socket under `TMUX_TMPDIR`. ctest therefore clears
`TMUX`, `TMUX_PANE`, `LIBTMUX_SOCKET` and `LIBTMUX_SOCKET_PATH` for every test
and points `TMUX_TMPDIR` at `tmux-sandbox/` in the build tree
([`cmake/LibtmuxTestSandbox.cmake`](../cmake/LibtmuxTestSandbox.cmake)).

A test binary started by hand has not been through ctest. The `main` it links,
[`support/test_main.cpp`](support/test_main.cpp), refuses to run and names each
variable it found, until the same variables are cleared and `TMUX_TMPDIR` is a
private directory that `LIBTMUX_TEST_SANDBOX` also names.
[`sandbox_canary_test.cpp`](sandbox_canary_test.cpp) asserts what ctest left and
resolves the default socket path. ctest runs it a second time in the
environment a tmux pane has, and that run must fail, and a nested ctest hands
it a pane's `TMUX` and expects it to pass.

A test that starts a server in that directory and forgets it would outlive the
suite, whether it used tmux's own socket or `-S <sandbox>/custom.sock`.
`CTestCustom.cmake` in the build tree runs a script before and after the tests
that looks at every socket in the sandbox, links not followed. Before the tests
it refuses while a server answers on one. After them it stops each live server
with `kill-server -S`, checks that a connect then fails for up to about three
seconds, and fails the run, naming the sockets, even when every test passed. A
socket file nothing answers on is not a leak and is removed; a server that does
not stop leaves its socket, and the sandbox, in place. `ctest -N` runs both too,
so use one ctest at a time per build tree.

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
$ (set -e; for p in \
    cxx-dev \
    cxx-sanitize \
    cxx-sanitize-cxx20 \
    cxx-tsan \
    cxx-gcc \
    cxx20; do \
    cmake --preset "$p"; \
    cmake --build --preset "$p"; \
    ctest \
      --preset "$p" \
      --no-tests=error; \
  done)
```

clang with libc++, GCC with libstdc++, the C++20 build over `tl::expected`,
address and undefined-behaviour sanitizers, and the thread sanitizer. The
C++20 sanitizer lane repeats ASan/UBSan with libstdc++ so allocator matching
remains strict despite Ubuntu's split libc++/libc++abi exception ownership.

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
