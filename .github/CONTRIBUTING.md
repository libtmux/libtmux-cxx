# Contributing

Thanks for looking. This is a C++ port of
[libtmux](https://libtmux.git-pull.com), and it is held to the same standard as
the original: nothing merges that is not proven against a real tmux.

The project is an **alpha prerelease**, which is good news for a contributor:
the public surface is still open. A change that improves a name or a signature
does not need a deprecation shim, and "this API is awkward" is a reportable bug
rather than something to work around. That ends at `0.1.0`.

How this project writes prose — README, changelog, release notes, commit
messages, API documentation, and source comments — is set out separately in
[`WRITING.md`](WRITING.md). Read that before changing any of it.

## Building

**Requirements:** a C++23 toolchain (clang 17+ or GCC 13+), CMake 3.25, tmux on
`$PATH`. Nothing else — the library itself has no dependencies.

```console
$ cmake --preset cxx-dev && cmake --build --preset cxx-dev
```

## Running the tests

```console
$ ctest --preset cxx-dev --no-tests=error
```

If that passes, you have everything. [`tests/README.md`](../tests/README.md)
explains what those tests are and why none of them mock tmux.

### Never share a tmux server

Several libtmux ports run their suites on this kind of machine at once, and a
tmux server is shared state keyed only by its socket. Two suites on one socket
end each other's sessions, and the failure reads like a real bug in whichever
one noticed first.

The fixture already handles this: every test server gets a private tree and its
own socket, with `TMUX` and `TMUX_PANE` erased from the child environment. Use
[`ScopedTmuxServer`](../include/libtmux/testing/scoped_server.hpp); do not start
tmux another way in a test. It ships as `libtmux::testing`, behind
`find_package(libtmux COMPONENTS testing)`, so a suite outside this repository
uses the same one rather than reinventing it. Pass
`SocketNamespace::consumer("your-suite")` so a stray directory names who left
it.

A one-off probe does the same by hand:

```console
$ export TMUX_TMPDIR="$(mktemp -d -p /tmp libtmux-cxx-spike-XXXXXX)"
```

Then `unset TMUX TMUX_PANE`, address the server with `-L` or `-S` under that
directory, `kill-server` when finished, and remove the tree.

Never run a bare `tmux` command against the default server while working here.
It is somebody's real session, and a stray `kill-server` takes their work with
it.

### Check the socket-path budget before macOS does

`sockaddr_un::sun_path` holds 104 bytes on macOS and 108 on Linux, and macOS
spends around sixty of them on `$TMPDIR` before this fixture adds anything:
`/var/folders/...` canonicalises to `/private/var/...`. A name or a nested
directory that costs ten bytes passes every Linux lane and fails there with
nothing but `File name too long`. `socket_path_fits` in the fixture reports that
case rather than letting tmux fail obscurely.

Run the suite under a temporary directory as long as the one macOS gives:

```console
$ BASE=/tmp/$(printf 'm%.0s' $(seq 1 52)) && mkdir -p "$BASE" && TMPDIR="$BASE" ctest --preset cxx-dev --no-tests=error; rmdir "$BASE"
```

## Checks that must pass

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

Formatting and static analysis are configured in the repository and enforced by
CI, so there is nothing to argue about:

```console
$ git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
```

Python tooling is linted by [ruff](https://docs.astral.sh/ruff/), configured in
[`pyproject.toml`](../pyproject.toml).

The API reference is generated from the public headers, and CI fails when it
drifts from them:

```console
$ python3 tools/docs/api_index.py \
    --include include/libtmux \
    --output docs/api.md
```

This repository is also a vcpkg registry, and the versions database can drift
from the ports beside it:

```console
$ python3 -m tools.vcpkg check
```

### The parity ledger is read here, not written

The ledger in `tools/parity/data` records what each Python `libtmux` symbol maps
to and the evidence for it. `verify`, `gaps`, `coverage` and `record-evidence`
read the recorded artifacts and this tree, and the gate above runs them.
`generate` and `drift` observe the Python library at a pinned commit and need
that repository's files and its history, none of which exist here.

So the recorded surface is frozen at the commit it was taken from. Adding a
classification means editing `mapping.json` and `evidence.json` and re-running
`python3 -m tools.parity sync`. Regenerating from a newer Python release means
doing it where that release lives. An evidence record cannot cite a test nobody
wrote — every case is resolved in the file it names.

## Pull requests

**Every capability needs a test against real tmux.** Not a mock, not a recorded
transcript — an integration test that starts a server, does the thing, and reads
back what tmux says happened. That is what makes a passing suite mean anything.

**A passing gate is evidence only once it has been shown able to fail.** Pair a
new test with a deliberate break that proves it bites; that is what
[the mutation catalogue](../tools/README.md#the-mutation-catalogue) does for the
suite as a whole.

**Errors are returned, never thrown.** The library reports failure as
`expected<T, CommandFailure>`, and that is not a style preference: it is the
whole error model. A `throw` in the library is a design change, not an
implementation detail.

**The typed surface should refuse what tmux would refuse.** If a mistake can be
caught at compile time, catch it there and add a case to
[`tests/compile/`](../tests/compile/) proving it stays caught. A refusal that
quietly stops refusing is a silent API regression.

**No dependencies in the core.** The library links nothing. The MCP server is
allowed a JSON parser because it is a program, not a library, and it is off by
default.

Keep the change narrowly scoped. Unrelated cleanup belongs in its own commit, or
its own pull request. Commit format is in [`WRITING.md`](WRITING.md).

## Review

A reviewer is asking five questions, in this order:

1. **Would the test fail without the fix?** If the change is a bug fix and the
   test passes on the unfixed tree, the test is not testing it.
2. **Which compatibility class is this?** Source, ABI, behavioural or wire — the
   vocabulary is in [`WRITING.md`](WRITING.md#compatibility-vocabulary). A
   change that moves one of them says so in its commit message, and lands in the
   changelog saying so.
3. **Does the parity ledger need a row?** A new capability usually means an
   entry in `mapping.json`, the test that proves it in `evidence.json`, and a
   `sync`.
4. **Is anything here unrelated to the stated change?** Formatting, renames and
   drive-by cleanup get their own commits so the behavioural diff stays
   reviewable.
5. **Does every commit stand on its own?** Not just the tip. A commit that
   builds only once the next one lands turns a `git bisect` report into a
   wrong answer, which is worse than no answer.

## Releases

**Never create a tag, and never push one.** The maintainer tags, and a tag
triggers the publish workflow.

The order a release keeps is fixed by vcpkg, because the portfile fetches a
release tarball by hash and that hash cannot exist before the tag does:

1. `VERSION` is bumped and committed.
2. The maintainer pushes the tag.
   [`release.yml`](workflows/release.yml) refuses a tag that disagrees with
   `VERSION`.
3. GitHub generates the tarball; only now does its hash exist.
4. The port's `version-semver` and `SHA512` are updated, and `x-add-version`
   records the new git-tree.

Step 4 therefore lands on a commit after the tag it describes, which is why a
consumer's baseline is the registry commit that follows the tag rather than the
tag itself. [`docs/vcpkg-registry.md`](../docs/vcpkg-registry.md) has the whole
of it.

### What a release ships

The library is consumed through the registry, so its release is a git-tree
rather than a file to download. The MCP server is a program, and is published
as one:

- A `libtmux-mcp-server-<version>-<platform>` binary per platform, each with a
  `.sha256` beside it.
- An SPDX SBOM describing what went into the archive.
- A build provenance attestation, signed with a short-lived workflow identity
  rather than a stored key.

The notes carry the source archive's `SHA512` and the line that checks a
binary, so a consumer never has to work out how:

```console
$ gh attestation verify <file> --repo libtmux/libtmux-cxx
```

A claim about an artifact belongs in the notes only when a reader can check it
with a command the notes give them.

## Compatibility

tmux 3.2a and newer, through `master`, matching the Python package. CI builds
and tests against every supported release on every change, and the handful of
capabilities that need a later tmux are covered by tests that skip below the
release providing them — [the README](../README.md#compatibility) lists them.

Two standards are supported: C++23 over `std::expected`, and C++20 over pinned
`tl::expected` with `LIBTMUX_CXX_STANDARD=20`. They are not ABI-compatible, so
each lives in its own inline namespace and mixing them is a link error rather
than a program that reads the wrong bytes.

None of this is promised until `0.1.0`. Alpha releases may change or remove
exported identifiers with no deprecation period.

The support policy, stated rather than implied: the newest release is the only
one that gets fixes, there are no backports, and the tmux range moves only when
a release leaves upstream support or a capability the library needs arrives.
A change that would move either floor says so in its commit message and in the
changelog, because moving a floor breaks somebody's build by definition.

## Reporting a bug

Say which tmux version (`tmux -V`), which compiler and standard library, and
which preset. A reproduction that starts its own server is worth more than a
description — [`examples/`](../examples/README.md) has the shape of one.

Behaviour that differs from Python libtmux is worth reporting even if this
library's behaviour seems reasonable. Parity is the goal, and a deliberate
divergence should be written down as one.

## Security

Report vulnerabilities privately through
[GitHub's advisory form](https://github.com/libtmux/libtmux-cxx/security/advisories/new)
rather than a public issue.

## License

MIT. By contributing you agree your work is published under it. See
[LICENSE](../LICENSE).
