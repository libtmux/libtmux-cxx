# Changelog

What changed in each release, for someone deciding whether to take it. The
conventions are in [`.github/WRITING.md`](.github/WRITING.md#changelog).

The `0.1.0-alpha.1` and `0.1.0-alpha.2` entries were written after those
releases, from the commits each tag carries. Everything from `0.1.0-alpha.3` on
was recorded as it landed.

## Unreleased

## 0.1.0-alpha.3 (2026-08-22)

### Breaking

- `FailureKind` gains `unsupported`, reported when a backend will not provide
  an operation rather than approximating it. Source-breaking for an exhaustive
  switch built with `-Werror=switch`; add the case, or a `default`. (#4)

  ```cpp
  case libtmux::FailureKind::unsupported:
    std::printf("this backend cannot provide the operation safely\n");
    break;
  ```

- A raw command that synchronously inserts control reply blocks is now rejected
  on a control connection unless the call declares its exact reply count.
  Behaviourally breaking for a caller relying on the inferred count, which does
  not inspect live aliases. Use the counting overload, a low-level `Connection`,
  or a subprocess-backed `Server`. (#4)

### Windows

- Add an experimental x64 desktop preview, built with Visual Studio 2022 against
  [psmux](README.md#windows-through-psmux) 3.3.7 at a pinned commit and archive
  hash. It answers exact read-only entity queries and declines the rest with
  `unsupported` rather than approximating it: typed option and hook access, pane
  input and capture, and control mode are all refused. Ask
  `Server::capabilities()` before choosing a workflow. (#4)
- The MCP server advertises four read-only tools there — `inspect_tmux`,
  `list_sessions`, `list_windows` and `list_session_panes` — and builds on MSVC
  now that the protocol version is compared as text. (#4)

### Control mode

- Inserted control replies are attributed without stealing concurrent output.
  `source_file` is declined, because a file can add an unknowable number of
  reply blocks; `check_file` remains. (#4)
- `Pane::break_out` works around raw tmux 3.7, which crashes on an unnamed
  multi-pane `break-pane` and ignores an explicit name. It selects the safe form
  inside one server command and repairs a requested name by stable window ID.
  tmux 3.7a fixes both upstream. (#4)
- Add `control_with_options` and `over_control_with_options`, so streaming
  policy enters through either `Server` doorway without rebuilding the socket
  route. (#4)

### MCP server

- A wait whose deadline expired during target lookup no longer publishes an
  empty `pane_id`, which its own output schema refuses. The field is emitted
  only once a target resolves, and is no longer required. (#4)

### vcpkg

- Release updates are monotonic, locked, recoverable and opt-in, and the exact
  tagged port is gated across Linux, macOS, Windows and WSL. Windows support in
  the port is an opt-in transition a release's tagged gates must clear; the
  immutable `0.1.0-alpha.2` entry stays POSIX-only whatever a later one does.
  (#4)

## 0.1.0-alpha.2 (2026-08-17)

The library is unchanged from `0.1.0-alpha.1`. This release is packaging: the
repository became a vcpkg registry, and two faults a user would meet before
reaching the library were fixed.

### vcpkg

- This repository now serves a vcpkg git registry, so installing needs neither
  an overlay port nor a clone of this tree. The project is too new for the
  curated registry.
- Add the versions database and `python3 -m tools.vcpkg check`, which fails when
  the git-tree recorded for a port's declared version is not that port's
  git-tree at `HEAD`. `x-add-version` does not report that case loudly enough to
  gate on, and the registry then serves the old port while the repository shows
  the new one.
- Add the `mcp` feature to the port.
- Fix the port's usage text, which vcpkg had been generating heuristically and
  getting wrong: it named `libtmux::testing` beside the library, but the package
  config defines that target only under `COMPONENTS testing`, so the snippet a
  consumer was handed failed to configure.
- Fix the README's install sequence, which did not run. Naming a registry makes
  the default registry's baseline mandatory, and vcpkg enforces that while
  loading the configuration — before `x-update-baseline --add-initial-baseline`
  could supply it. The manifest and the initial baseline now come first.
- Pushing a tag now rewrites the port and publishes it from the registry in the
  same run. The port fetches its archive by hash, and that hash cannot exist
  before the tag does, so the step necessarily follows tagging and had been
  getting forgotten.
- `probe` takes `--repository`, so the gate can resolve the port over the public
  URL instead of only through a local path. A local path resolves a commit that
  was never pushed, which is the failure a first consumer meets.
- Keep the port's debug tree out of `share`.
- The release notes now name the archive hash the port actually needs, rather
  than the `git archive` hash, which differs.

### MCP server

- `--prefix` now searches `tools/libtmux/` as well as `bin/`. The vcpkg port
  installs the server there, because a static triplet has no `bin/` for it, so
  pointing an agent CLI at a vcpkg install failed with `is not an executable`
  naming a path the package never wrote.

## 0.1.0-alpha.1 (2026-08-16)

First release: the library, the MCP server, and the test fixture, against tmux
3.2a through `master`.

- `Server`, `Session`, `Window` and `Pane` as value types that copy, compare,
  hash and print. An entity is one row of a shared snapshot and reads its fields
  without reaching tmux; the process ran once, when the snapshot was taken.
- Failure is a value. Every call answers `expected<T, CommandFailure>`, and
  nothing throws to report a tmux or transport failure.
- Typed queries over tmux's own fields: `FilterExpr`, composed with `&&`, `||`
  and `!`, working with standard ranges. A filter that asks a number whether it
  starts with a string does not compile, and `tests/compile/` holds the programs
  proving it stays that way.
- Control mode as a second transport behind the same calls, holding one
  connection open.
- Two standards. C++23 over `std::expected`, or C++20 over pinned `tl::expected`
  with `LIBTMUX_CXX_STANDARD=20`, each in its own inline namespace so objects
  built against one cannot link against the other.
- `libtmux::testing`, installed beside the library behind
  `find_package(libtmux COMPONENTS testing)`, so a consumer's suite gets the
  same private-socket tmux fixture this one uses.
- An MCP server, so an agent can drive tmux directly.
- No dependencies in the core.
