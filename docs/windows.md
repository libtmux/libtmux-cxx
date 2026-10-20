# Windows through psmux

The Windows preview reaches a Windows terminal through
[psmux][psmux]: it is bounded on purpose, serves what it can prove and refuses
the rest. The [README](../README.md#windows-through-psmux) lists what a caller
needs first; this page is the full boundary. The design is in
[`design/windows-psmux.md`](design/windows-psmux.md).

## Setup

On Windows, install psmux with its `tmux.exe` alias and use
`Server::at_default()` or `Server::at_socket_name(...)`. The latter emits the
separated `-L name` form psmux parses. `Server::from_env()` also recovers that
selector inside a psmux pane. An explicit `-L default` is rejected because
psmux gives it the same environment identity as its unselected default.

## The pinned target

The verified preview target is psmux 3.3.7 at commit
`05cc5d4cded047bd3f3d1955299fd0bd259f2d81`. It is the official
`psmux-v3.3.7-windows-x64.zip`, whose SHA-256 is
`60ff7b236f64184921cef3c1ff2611aa5a36fcc7ed8e2a58e968b8ded57f6028`,
and its `tmux.exe` has SHA-256
`f3a4ad033f0332972bc79b8d695387adb4fcd1d268074e113c1c84e5381051a6`.
Treat any other binary as a new compatibility target until the native smoke
passes against it.

## Launching from WSL

WSL can launch the native library with a `PATHEXT` that omits `.EXE`, which
prevents psmux from finding PowerShell or `cmd`. The subprocess backend repairs
only the psmux child's copy and preserves any custom extensions from the caller.
It resolves `tmux` with the caller's Windows `PATH` and an explicit `.exe`
extension; a WinGet installation normally exposes
`%LOCALAPPDATA%\Microsoft\WinGet\Links\tmux.exe`.
It also disables psmux's warm-claim path for every child: psmux 3.3.7
reserializes the caller's working directory through a command-line protocol,
where a standalone semicolon can become another command.

## How it is checked

psmux is supported on a best-effort basis, and CI coverage for it is highly
limited: CI builds and installs the Windows preview with MSVC but runs no psmux
test, and the release builds the Windows binary and the vcpkg port without
running them against psmux. To check a build, run the `windows-psmux` preset as
the README describes, in C++20 and C++23, and repeat every Windows-labeled
CTest from a WSL import.

## The typed surface

The tested typed surface is deliberately narrower than tmux's. Psmux 3.3.7
starts one independent server for each session and implements many window and
pane targets by temporarily focusing an object. A stale target can otherwise
operate on the unrelated active object while reporting success. The Windows
backend keeps entity calls inside the captured `-L` namespace and verifies the
session ID before and in entity-shaped replies. The typed API rejects
operations that psmux cannot target safely.

Supported typed operations include server liveness and version queries, exact
session listing and lookup, policy-bounded namespace cleanup that rescans ordinary
rename and exit races, session-scoped window and pane listings,
identity-checked session, window, and pane expansion and refresh, and
child-to-parent relations that first prove the child still exists.

`Server::capabilities()` reports this routing contract locally, without
launching `tmux.exe`. Check [`ServerFeature`][ref-serverfeature] before choosing
a workflow; a custom backend is unknown and supports nothing until it identifies
its own contract. Unsupported typed calls return `FailureKind::unsupported` with
`delivery == DeliveryStatus::not_started`.

## What fails before dispatch

The following capability boundaries fail before dispatch on Windows:

- `-S` socket paths and persistent control mode;
- unscoped server state such as clients, buffers, bindings, messages,
  configuration, wait channels, and server or global options and hooks;
- captured session mutation, including navigation, option or hook changes,
  rename, kill, and client detachment;
- session creation, because concurrent psmux creators can both report another
  process's session as their own;
- window creation, because psmux can report a failed creation as an existing
  window after renaming that existing window;
- window and pane calls that psmux redirects through the active object,
  including split, rename, move, link, swap, layout, resize, input, capture,
  pipe, respawn, and window or pane option calls;
- cross-object pane and window operations that psmux cannot address by stable
  IDs;
- pane selection and killing, session option and hook reads, reusable attach
  commands, and window targets.

`Session::attach_command()` and `Window::checked_target()` return an explicit
unsupported failure on Windows. `Window::target()` retains its empty
compatibility sentinel. Ambiguous `-L default`, unsafe registry names, and
typed arguments containing psmux command separators or line breaks are
malformed requests instead, so they report `FailureKind::validation`.

## Identity and renames

With an intact psmux registry, captured handles fail closed after an external
session rename. Reacquire the session from `Server::session()` before
continuing; an old handle will not follow an ordinary renamed or same-name
replacement session. Corrupting psmux's global session-ID counter or registry
falls outside this preview's identity guarantee.

In pinned psmux 3.3.7, a renamed server retains its startup-name guard until
it exits, so that old name cannot be reused while the renamed server lives.
Upstream commit
[`b3d55f88`](https://github.com/psmux/psmux/commit/b3d55f88e5066cc7f87536fe9acae8570f145a39)
fixes that limitation after 3.3.7.

## Raw commands

`Server::run`, batches, and chains remain raw escape hatches. They use psmux's
mutable most-recent session when the command has no exact target. Psmux 3.3.7
also prefix-matches `-L` registry names, so raw `list-sessions` can include a
selector such as `name__nested`, and raw `kill-server` can kill it. Typed
listing and cleanup filter and target sessions individually; raw commands do
not provide that isolation. Psmux also joins raw argv back into a line protocol;
semicolons and newlines can start another command. Raw callers own that parsing
boundary.

## Trust

Typed method names and MCP `readOnlyHint` annotations describe the command the
library requests; they do not stop a hostile server configuration from changing
what it does. Tmux and psmux both expand server-side `command-alias` entries before built-in lookup,
so an administrator can redefine an apparent read. Use only servers and
configurations you trust. `PSMUX_CONFIG_FILE` controls a server created with
it; it does not attest an already-running server's live alias map.

## Build and test

The native smoke test needs the Visual Studio 2022 C++ workload and a psmux
`tmux.exe` discoverable through `PATH`. Run the preset with Windows CMake from
a Developer PowerShell in a Windows-path checkout; Linux CMake inside WSL
cannot select the Visual Studio generator:

```console
$ cmake --preset windows-psmux
```

```console
$ cmake --build --preset windows-psmux
```

```console
$ ctest --preset windows-psmux --no-tests=error
```

The same build installs and links through `find_package`:

```console
$ cmake --install build/windows-psmux --config Debug --prefix "$PWD/build/windows-prefix"
```

```console
$ cmake -S examples/consume -B build/windows-consume -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$PWD/build/windows-prefix"
```

```console
$ cmake --build build/windows-consume --config Debug
```

```console
$ .\build\windows-consume\Debug\consume.exe
```

## What the preview does not build

Windows runs each call as a `tmux.exe` subprocess and has no control-mode
transport. It builds the dedicated psmux example and the capability-aware MCP server, but not the
POSIX examples, fuzzers, or `libtmux::testing` fixture. Asking CMake for either
of the latter two fails during configuration. The Windows MCP catalog exposes
the same 45 names listed in the MCP guide; the native smoke records which of
those operations psmux 3.3.7 actually supports and requires unsupported
commands to fail explicitly.

Psmux reports a tmux-shaped version but does not promise every command has
identical behavior. The native smoke checks behavior and claims no compatibility
level; it has run only against psmux 3.3.7. See its
[integration guide][psmux-integration] and
[compatibility notes][psmux-compatibility], and test the operations your program
depends on against the installed psmux release.

## Cleanup and the psmux registry

Psmux 3.3.7 keeps its routing registry in the Windows profile's `.psmux`
directory and does not honor `PSMUX_DATA_DIR`. The smoke therefore uses a
high-entropy `-L` selector, clears inherited routing variables, performs only
exact per-session cleanup, and verifies that no exact registry file for its
selector and sessions remains. A deliberately nested prefix is kept alive. It
never issues `kill-server`. Psmux itself scans that global registry and reaps
entries it considers orphaned before it parses `-L`; the library cannot isolate
or disable that upstream maintenance in 3.3.7. Cleanup is bounded and
best-effort if another process continuously renames or creates sessions. Its
deadline is the Server's [`ExecutionPolicy`][ref-executionpolicy]; an explicitly
unbounded policy is unbounded here too. Do not treat a psmux namespace as a lock
or security boundary from other local psmux users.

[psmux]: https://github.com/psmux/psmux
[psmux-compatibility]: https://github.com/psmux/psmux/blob/master/docs/compatibility.md
[psmux-integration]: https://github.com/psmux/psmux/blob/master/docs/integration.md
[ref-executionpolicy]: https://libtmux.org/en/cxx/latest/reference/libtmux-executionpolicy/
[ref-serverfeature]: https://libtmux.org/en/cxx/latest/reference/libtmux-serverfeature/
