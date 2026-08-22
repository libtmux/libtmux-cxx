<div align="center">
  <h1>libtmux for C++</h1>
  <p><strong>Drive tmux from C++: typed, value-semantic control over servers, sessions, windows, and panes.</strong></p>
  <p>
    <a href="https://github.com/libtmux/libtmux-cxx/actions/workflows/ci.yml"><img src="https://github.com/libtmux/libtmux-cxx/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
    <a href="#requirements--support"><img src="https://img.shields.io/badge/C%2B%2B-23%20%7C%2020-blue.svg" alt="C++23 or C++20"></a>
    <a href="#compatibility"><img src="https://img.shields.io/badge/tmux-3.2a%20%E2%80%93%20master-1BB91F.svg" alt="tmux 3.2a and newer"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT licence"></a>
  </p>
</div>

> [!WARNING]
> **Alpha.** Releases carry an `-alpha` prerelease tag. The API is not
> settled, and any release may change or remove exported identifiers without a
> deprecation period. Pin an exact version. Not recommended for production.

## What is libtmux?

A C++ port of [libtmux], the Python library behind [tmuxp]. Stop shelling out
to `tmux ls` and parsing the result. Work with `Server`, `Session`, `Window`
and `Pane` as ordinary C++ values instead.

Given a `server` — from `Server::from_env()` inside tmux,
`Server::at_socket_name(...)`, `Server::at_socket_path(...)`, or
`Server::at_default()`:

```cpp
// Every call answers with a value: the result, or the reason there is none.
const auto panes = server.panes();
if (!panes.has_value()) {
  std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
  return 1;
}

// Find the active pane running an editor, and press Escape in it.
auto editing = *panes | libtmux::matching(libtmux::pane::command.starts_with("nv") &&
                                          libtmux::pane::active);

if (auto pane = libtmux::first(editing)) {
  (void)pane->get().send_key("Escape");
}
```

Two things make it different from a wrapper around `system()`: **failure is a
value** — no tmux or transport error is ever thrown, every call hands back
either the answer or the reason there isn't one — and **queries are typed**, so
a filter that asks a number whether it starts with a string does not compile.

(Not a `noexcept` claim on the whole surface: allocation can throw, and so can
a `CommandObserver` you supply. What the library will not do is signal a tmux
failure by throwing.)

### Features

- **Value semantics.** An entity copies, compares, hashes and prints. A pane
  can key a map; a window prints as `Window(@1 2:editor)`.
- **Typed queries.** [`FilterExpr`](#query-with-typed-filters) over tmux's own
  fields, composed with `&&`, `||` and `!`, working with standard ranges.
- **Errors as values.** One `CommandFailure` type with a
  [kind](#when-things-fail), so a caller can tell a malformed request from
  tmux refusing from tmux never answering.
- **No dependencies.** The core links nothing. Not even a JSON parser.
- **Two standards.** C++23 over `std::expected`, or C++20 over pinned
  `tl::expected`, each in its own ABI namespace so they cannot be mixed by
  accident.
- **A faster transport for typed entities.**
  [Control mode](#batches-chains-and-control-mode) keeps one connection open —
  about 4.6× faster per listing.
- **An [MCP server](#the-mcp-server)** so an agent can drive tmux directly.
- **Compile-time refusals.** `pane::active.starts_with("x")` is a build error,
  and [a test proves it stays one](tests/README.md#the-compile-tests-are-the-interesting-ones).

## Requirements & support

| | |
|---|---|
| **tmux** | 3.2a or newer, through `master` — [with these exceptions](#compatibility) |
| **C++** | C++23 (clang 17+, GCC 13+, Visual Studio 2022 17.3+), or C++20 with `LIBTMUX_CXX_STANDARD=20` |
| **CMake** | 3.25 |
| **Platforms** | Linux, macOS; experimental x64 desktop Windows support with Visual Studio 2022 through [psmux] |
| **Dependencies** | None for the library |

## Installation

Nothing to install alongside it: the library has no dependencies. Pick
whichever of these your project already uses.

### CMake FetchContent

No install step, and nothing to vendor:

```cmake
include(FetchContent)
FetchContent_Declare(
  libtmux
  GIT_REPOSITORY https://github.com/libtmux/libtmux-cxx.git
  GIT_TAG        v0.1.0-alpha.2  # or a commit; never a moving branch
)
FetchContent_MakeAvailable(libtmux)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

Embedded this way the library builds alone: its tests, examples and the MCP
server all default off when it is not the top-level project.

The `v0.1.0-alpha.2` release above is POSIX-only. This checkout adds the
bounded x64 desktop Windows preview for Visual Studio 2022 and the audited
psmux build; pin a commit containing it, or build the checkout directly,
until a later tagged release passes the Windows publication gate.

### Build and install from source

```console
$ cmake -S . -B build -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build
```

```console
$ cmake --install build --prefix ~/.local
```

Then, from any project:

```cmake
find_package(libtmux REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

[`examples/consume/`](examples/consume/) is exactly that project, three files
long, and CI builds it against a real install so the package config cannot rot.

### Git submodule

```console
$ git submodule add https://github.com/libtmux/libtmux-cxx.git third_party/libtmux
```

```cmake
add_subdirectory(third_party/libtmux)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

### vcpkg

This repository is a [vcpkg git registry](docs/vcpkg-registry.md), so vcpkg
resolves `libtmux` by version the way it resolves anything else. It is not in
the curated registry — the project is far too new to meet vcpkg's maturity bar,
and serving your own registry is what vcpkg's documentation recommends instead.

Declare the dependency in `vcpkg.json` first, with no registry file yet:

```json
{
  "name": "your-project",
  "version": "0.1.0",
  "dependencies": ["libtmux"]
}
```

Pin vcpkg's own baseline before naming any registry. **The order matters**:
once a configuration names a registry, vcpkg refuses to load it until the
default registry has a baseline — including when you run the command that
would add one.

```console
$ vcpkg x-update-baseline --add-initial-baseline
```

Now add `vcpkg-configuration.json` beside the manifest, leaving `baseline`
empty for the next command to fill:

```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/libtmux/libtmux-cxx",
      "baseline": "",
      "packages": ["libtmux"]
    }
  ]
}
```

```console
$ vcpkg x-update-baseline
```

That writes a commit of this repository into `baseline`, which is what your
build resolves against from then on. Re-run it to move to a newer release.

```cmake
find_package(libtmux CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

[The MCP server](#the-mcp-server) is a feature rather than a second package,
because it ships from this same source. Ask for it by name:

```json
{
  "dependencies": [{ "name": "libtmux", "features": ["mcp"] }]
}
```

It installs as a program at
`<installed-root>/<triplet>/tools/libtmux/libtmux-mcp-server[.exe]`; manifest
mode normally uses `vcpkg_installed` as the root, classic mode uses
`<vcpkg-root>/installed`, and `--x-install-root` overrides both. Nothing links
the executable. The current checkout accepts `--socket-name` or `--socket-path`
and retains one positional path on POSIX; without a selector, only a valid
inherited `TMUX` route is accepted. Alpha.2's legacy POSIX server instead takes
one socket path as its sole argument. Always supply that release's exact route
because omitting it falls back to inherited or default tmux.

The immutable `0.1.0-alpha.2` registry entry predates the Windows backend and
remains Windows-disabled. A later tagged release can enable the standard x64
desktop target only after passing the pinned native psmux and package gates,
advancing its `version-semver` and archive hash, and resetting its packaging
revision. MinGW, UWP, Xbox, and non-x64 Windows triplets remain excluded from
the vcpkg package; the other packaged targets are Linux and macOS.

### Build options

| Option | Default | What it does |
|---|---|---|
| `LIBTMUX_CXX_STANDARD` | `23` | `20` uses `tl::expected`: a system one, or the pinned fallback |
| `LIBTMUX_BUILD_TESTS` | on if top-level | POSIX suite with tmux and GoogleTest; native psmux smoke on Windows |
| `LIBTMUX_BUILD_EXAMPLES` | on if top-level | Six POSIX programs or the native Windows psmux preview in [`examples/`](examples/README.md) |
| `LIBTMUX_BUILD_TESTING_LIBRARY` | on with tests or examples on POSIX; off on Windows | Install the POSIX private-server fixture |
| `LIBTMUX_BUILD_MCP_SERVER` | off | Build the [MCP server](#the-mcp-server), which needs a JSON parser; Windows exposes its psmux-safe subset |
| `LIBTMUX_BUILD_FUZZERS` | off | Build the POSIX parser fuzzers |
| `LIBTMUX_FETCH_DEPS` | off | Download what is missing instead of failing |

`LIBTMUX_FETCH_DEPS` is off on purpose: a build that would reach the network
says so and stops, rather than downloading quietly. That covers every
dependency, the C++20 lane's `tl::expected` included — it looks for an
installed one first, and says what to install when there is none.

## Quickstart

Every C++ example below is compiled and run against a real tmux in the POSIX
example and CI lanes; the Windows preview has its own executed psmux example.
The snippets are not written here — they live in
[`examples/05-readme.cpp`](examples/05-readme.cpp), and
[a check](tools/docs/check_readme.py) fails the build if this file and that one
ever disagree.

On native Windows, begin with the capability-aware
[`07-windows-psmux.cpp`](examples/07-windows-psmux.cpp) and its
[paste-and-run fixture](examples/windows/README.md). The workspace, pane input,
and capture sections below require POSIX tmux; the psmux preview deliberately
rejects those operations. Query `Server::capabilities()` before selecting a
Windows workflow.

### Connect and look around

```cpp
// No tmux failure is thrown. Every call answers with a value that is either
// the result or the reason there isn't one.
const auto sessions = server.sessions();
if (!sessions.has_value()) {
  std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
  return 1;
}

for (const libtmux::Session& session : *sessions) {
  std::printf("%s has %lld window(s)\n", std::string{session.name()}.c_str(),
              session.window_count());
}
```

`Server::at_socket_path(...)` and `Server::at_socket_name(...)` reach a
specific server; `Server::from_env()` reaches the one the calling program is
running inside, and `Server::at_default()` the one a person means by "my tmux".

### Build a workspace

```cpp
// Build an arrangement without composing a single tmux argument.
const auto editor = session.new_window({.name = "editor"});
if (!editor.has_value()) {
  std::fprintf(stderr, "%s\n", editor.error().diagnostic.c_str());
  return 1;
}

const auto logs = editor->split({.horizontal = true, .percentage = 30});
if (!logs.has_value()) {
  std::fprintf(stderr, "%s\n", logs.error().diagnostic.c_str());
  return 1;
}

(void)logs->send_text("journalctl -f");
(void)logs->send_key("Enter");
```

No tmux argument is composed anywhere in that. Every target is the id tmux
gave the object, so a window called `my:window` cannot redirect an operation.

### Query with typed filters

```cpp
// A filter is a value built from typed fields, not a string tmux parses.
// They compose with `&&`, `||` and `!`, and the result is a standard range.
const auto interesting =
    (libtmux::pane::command == "bash" || libtmux::pane::command == "zsh") &&
    !libtmux::pane::dead;

for (const libtmux::Pane& shell : *panes | libtmux::matching(interesting)) {
  std::printf("%s is a live shell, %lld columns wide\n",
              std::string{shell.id()}.c_str(), shell.width());
}

// An expression owns what it compares against, so this one still works
// after the string it was built from has gone out of scope.
const auto by_name = [] {
  const std::string wanted = std::string{"edi"} + "tor";
  return libtmux::window::name == wanted;
}();

const auto windows = server.windows();
if (windows.has_value()) {
  const auto found = std::ranges::distance(*windows | libtmux::matching(by_name));
  std::printf("%td window(s) called editor\n", found);
}
```

Fields are typed, so the compiler rejects a question the field cannot answer.
Each of these is a build error, and a test in
[`tests/compile/`](tests/compile/) proves it stays one:

| Will not compile | Why |
|---|---|
| [`pane::active.starts_with("1")`](tests/compile/flag_field_has_no_string_operations.cpp) | A flag renders as `1` or `0`; comparing that text is the mistake |
| [`pane::width.contains("8")`](tests/compile/numeric_field_has_no_string_operations.cpp) | A size compares as a number, where 9 is less than 10 rather than after it |
| [`window::name > 5`](tests/compile/string_field_has_no_ordering.cpp) | Ordering a name as text is a different question from ordering numbers |

### Exactly one, or say why not

```cpp
// "Exactly one, or say why not" is a question the library answers directly,
// rather than one every caller reimplements around `.size() == 1`.
auto addressed = *panes | libtmux::matching(libtmux::pane::id == panes->at(0).id());

if (const auto one = libtmux::exactly_one(addressed); one.has_value()) {
  std::printf("exactly one: %s\n", std::string{one->get().id()}.c_str());
}

// And when it is not one, the answer says which way it went wrong.
auto absent = *panes | libtmux::matching(libtmux::pane::command == "no-such-command");

if (const auto none = libtmux::exactly_one(absent); !none.has_value()) {
  std::printf("not one: %s\n", std::string{libtmux::to_string(none.error())}.c_str());
}
```

`first` and `exactly_one` return a reference *into* the range you gave them, so
they refuse a temporary rather than dangle at the semicolon. Name the range,
and the storage you have to keep alive is visible in your own code.

### Read a pane

```cpp
// Read a pane's visible contents, or its scrollback.
const libtmux::Pane& pane = panes->at(0);

const auto visible = pane.capture();
if (visible.has_value()) {
  std::printf("%zu bytes on screen\n", visible->size());
}

const auto history = pane.capture({.whole_history = true});
if (history.has_value()) {
  std::printf("%zu bytes of scrollback\n", history->size());
}
```

A scrollback can be far larger than the default bound. A capture that does not
fit is **reported, not truncated** — `CaptureOptions::output_limit` says how
much you are prepared to hold.

### Traverse the hierarchy

```cpp
// Every entity knows the server it came from, so it can reach its children
// and its parents without a target string.
const auto window = pane.window();
const auto owner = pane.session();
if (window.has_value() && owner.has_value()) {
  std::printf("%s is in %s, in %s\n", std::string{pane.id()}.c_str(),
              std::string{window->name()}.c_str(),
              std::string{owner->name()}.c_str());
}
```

### A snapshot is a moment, not a handle

```cpp
// An entity is one row of the listing that produced it: a moment, not a
// live handle. Ask again for the present.
(void)editor->rename("renamed");

std::printf("held: %s\n", std::string{editor->name()}.c_str()); // still "editor"

const auto now = editor->refresh();
if (now.has_value()) {
  std::printf("now: %s\n", std::string{now->name()}.c_str()); // "renamed"
}
```

This is the one thing most likely to surprise. An entity is one row of the
listing that produced it, so reading a field costs nothing and never fails —
but it answers about the moment it was listed. `refresh()` returns a *new*
value rather than mutating the old one.

### When things fail

```cpp
// Failures are values with a kind, so a caller can tell "you asked wrongly"
// from "tmux said no" from "tmux never answered".
const auto gone = server.run({"kill-session", "-t", "=no-such-session"});
if (!gone.has_value()) {
  switch (gone.error().kind) {
  case libtmux::FailureKind::validation:
    std::printf("the request was malformed before it was sent\n");
    break;
  case libtmux::FailureKind::unsupported:
    std::printf("this backend cannot provide the operation safely\n");
    break;
  case libtmux::FailureKind::refused:
    std::printf("tmux refused it: %s\n", gone.error().diagnostic.c_str());
    break;
  case libtmux::FailureKind::timeout:
    std::printf("tmux did not answer in time\n");
    break;
  default:
    std::printf("%s\n", gone.error().diagnostic.c_str());
    break;
  }
}
```

| Kind | Means |
|---|---|
| `validation` | The request was malformed; it never left the process |
| `unsupported` | The selected backend cannot provide the operation safely; nothing was dispatched |
| `spawn`, `pre_exec`, `pipe` | tmux could not be started or talked to |
| `timeout` | Dispatched, no answer — tmux may already have acted |
| `refused` | tmux ran and said no |
| `missing` | The object is gone. tmux itself reports this as empty fields and exit zero |
| `truncated` | The answer did not fit the limit given |

### Escape hatch

```cpp
// Anything tmux knows and this library does not name yet: ask it directly,
// with a format string expanded against a pane.
const auto running = pane.expand("#{pane_current_command}");
if (running.has_value()) {
  std::printf("running %s\n", running->c_str());
}

// Or run a command and read its output.
const auto answer = server.run({"display-message", "-p", "#{version}"});
if (answer.has_value()) {
  std::printf("tmux %s", answer->c_str()); // tmux's answer ends in a newline
}
```

Nothing here is a dead end: anything tmux can do is reachable, whether or not
the typed surface has named it yet.

### Options

```cpp
// Options are read and written where tmux scopes them.
(void)session.set_option("@project", "libtmux");

const auto project = session.option("@project");
if (project.has_value()) {
  std::printf("@project is %s\n", project->value.c_str());
}
```

Options are read and written at the scope that holds them — server, session,
window, pane. A value inherited from a wider scope is reported as inherited
rather than as one set here, because those differ when you are deciding
whether to write. POSIX tmux preserves arbitrary option text. The Windows
psmux preview rejects typed option and hook access because psmux keeps that
state in separate per-session servers and cannot target it atomically; see
[Windows through psmux](#windows-through-psmux).

### Batches, chains and control mode

```cpp
// A chain refuses a target it cannot address before reaching tmux at all,
// so a malformed batch costs nothing.
libtmux::Chain chain;
chain.new_window("a:b", "unreachable");
std::printf("chain valid: %s\n", chain.valid() ? "yes" : "no"); // no
```

Three ways to send work, and the difference matters:

| | What it is | Failure |
|---|---|---|
| **Call** | One command, one process | Reported on the call |
| **Batch** | One tmux invocation, one fail-fast group | Partially applied, not rolled back |
| **Chain** | Validated as it is built | A bad target is caught before tmux is reached |
| **Control** | One connection held open | Every command gets its own reply block |

`Server::over_control(session)` returns a `Server` that dispatches every entity
operation over a held-open connection — the same entity calls, without a
process each, about 4.6× faster per listing. `source_file` is deliberately
unsupported there because a file can add an unknowable number of control reply
blocks; `check_file` remains available. Direct raw commands that synchronously
insert reply blocks are rejected unless the low-level connection is given their
exact reply count. The inferred-count path does not inspect live aliases, so an
alias that changes the count must use that overload. A control-backed `Server`
has no count override and requires aliases to preserve that count for every
command it dispatches; otherwise use a low-level `Connection` or a
subprocess-backed `Server`. See
[`docs/design/control-transport.md`](docs/design/control-transport.md).

Streaming policy can enter through either Server doorway without rebuilding
its socket route: use `control_with_options` or `over_control_with_options`.
The Server supplies the socket and the first argument supplies the session
name; the caller-selected executable, deadlines, limits, and pane-output policy
are preserved. Windows psmux rejects both forms as unsupported before launching
a control client.

## Core concepts

| C++ type | tmux concept | Notes |
|---|---|---|
| [`Server`](include/libtmux/server.hpp) | tmux server / socket | Entry point; owns sessions |
| [`Session`](include/libtmux/entities.hpp) | session (`$0`, `$1`, …) | Owns windows |
| [`Window`](include/libtmux/entities.hpp) | window (`@1`, `@2`, …) | Owns panes |
| [`Pane`](include/libtmux/entities.hpp) | pane (`%1`, `%2`, …) | Where commands run |
| [`Client`](include/libtmux/entities.hpp) | an attached terminal | Read-only |
| [`Buffer`](include/libtmux/entities.hpp) | the server's clipboard | Named text outliving its pane |
| [`Connection`](include/libtmux/control.hpp) | a control-mode session | Reply blocks, notifications |

## tmux, libtmux, and tmuxp

| Tool | Layer | Use it for |
|---|---|---|
| [tmux] | The terminal multiplexer | Everyday manual use |
| **libtmux (C++)** | This library | Programmatic control from C++ |
| [libtmux (Python)][libtmux] | The original | The same, from Python |
| [tmuxp] | An app on libtmux | Declarative workspaces from YAML |

This port aims at practical parity with the Python library. Where they differ
the difference is written down — see [`tools/`](tools/README.md) for the ledger
that records what maps to what, and the evidence for each entry.

## The MCP server

A separate program, in its own package: **[`apps/mcp/`](apps/mcp/README.md)**.

It gives an agent hands inside the terminal, speaking the
[Model Context Protocol](https://modelcontextprotocol.io) over stdio from one
installed executable.

```console
$ cmake -S . -B build/mcp -DLIBTMUX_BUILD_MCP_SERVER=ON -DLIBTMUX_FETCH_DEPS=ON -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/mcp && cmake --install build/mcp --prefix ~/.local
```

```console
$ claude mcp add tmux -- ~/.local/bin/libtmux-mcp-server --socket-name libtmux-agent
```

On POSIX its deliberately small catalog covers inspection, creation, pane
capture and input, search, and bounded waits. Windows advertises only four
read-only operations proven safe through psmux: `inspect_tmux`, `list_sessions`,
`list_windows`, and `list_session_panes`. It does not advertise creation, pane
capture, input, search, or streaming there. For full coverage use the Python
[libtmux-mcp](https://github.com/tmux-python/libtmux-mcp).
**[Full documentation →](apps/mcp/README.md)**

## Testing your own tmux tools

Writing something that drives tmux on POSIX? The fixture this project's own
suite runs on is installed with the package. Ask for it by name:

```cmake
find_package(libtmux REQUIRED COMPONENTS testing)
target_link_libraries(your_tests PRIVATE libtmux::testing)
```

```cpp
// A private tmux for a suite of your own, gone when the scope ends.
auto fixture = libtmux::test::ScopedTmuxServer::start(
    {.socket_namespace = libtmux::test::SocketNamespace::consumer("my-suite")});
if (!fixture.has_value()) {
  std::fprintf(stderr, "%s\n", fixture.error().c_str());
  return 1;
}
const auto under_test =
    libtmux::Server::at_socket_path(fixture->socket_path().string());
std::printf("sessions on it: %zu\n", under_test->sessions()->size());
```

That gives you a private server on its own socket, under its own
`TMUX_TMPDIR`, with `TMUX` and `TMUX_PANE` erased from the child environment —
then kills it and removes the tree, including when the test fails.

That last part matters more than it looks: a tmux server is shared state keyed
only by its socket, so a suite that uses the default server will end somebody
else's session, and the failure will look like a bug in whatever noticed first.

It links no test framework, so GoogleTest, Catch2 and doctest all work.

**[Testing API reference →](docs/api-testing.md)** ·
**[How this project tests →](tests/README.md)** ·
**[A worked consumer →](examples/tests/README.md)**

## Compatibility

tmux 3.2a and newer, matching the Python package. `Version` orders releases
correctly: `3.7 < 3.7a < 3.7b`, `next-3.8` precedes `3.8`, and `master` sorts
above every numbered release. CI builds and tests pinned compatibility cells
from 3.2a through 3.7b, plus `master`, on every pull request and master change.

Across that range tmux does not answer identically, and where it cannot the
library cannot either. These are the exceptions, each covered by a test that
skips below the release that provides it and runs everywhere above:

| Needs | Capability |
|---|---|
| tmux 3.3 | `display-message -c` addressed at a control client |
| tmux 3.3 | `-x` and `-y` honoured on a detached `new-session` |
| tmux 3.4 | `%message` delivered to an attached control client |
| tmux 3.4 | notification-shaped command output kept in the block body |
| tmux 3.6 | The created-window report when breaking a window's only pane |

Two are defects rather than missing features — tmux answered wrongly and a
later release fixed it, so the affected window is closed at both ends:

| Broken on | What tmux does |
|---|---|
| tmux 3.4 – 3.5 | Echoes a non-UTF-8 byte back as its octal escape rather than the byte |
| tmux 3.4 | Adds a backslash before `$` when an option value it printed is set again |

Raw tmux 3.7 also crashes an unnamed multi-pane `break-pane` and ignores an
explicit name there. `Pane::break_out` selects the safe unnamed form inside
one server command and repairs a requested name by stable window ID; tmux
3.7a fixes both defects upstream.

Everything else holds across the whole range. A `split` carrying a percentage
is spelled `-l 25%` rather than the `-p 25` that tmux 3.4 removed, so it works
on every supported release — Python libtmux still emits `-p` and does not.

### Windows through psmux

On Windows, install psmux with its `tmux.exe` alias and use
`Server::at_default()` or `Server::at_socket_name(...)`. The latter emits the
separated `-L name` form psmux parses. `Server::from_env()` also recovers that
selector inside a psmux pane. An explicit `-L default` is rejected because
psmux gives it the same environment identity as its unselected default.

The verified preview target is psmux 3.3.7 at commit
`05cc5d4cded047bd3f3d1955299fd0bd259f2d81`. CI downloads the official
`psmux-v3.3.7-windows-x64.zip`, requires SHA-256
`60ff7b236f64184921cef3c1ff2611aa5a36fcc7ed8e2a58e968b8ded57f6028`,
and runs its `tmux.exe`, whose SHA-256 is
`f3a4ad033f0332972bc79b8d695387adb4fcd1d268074e113c1c84e5381051a6`.
Treat any other binary as a new compatibility target until the native smoke
passes against it.

WSL can launch the native library with a `PATHEXT` that omits `.EXE`, which
prevents psmux from finding PowerShell or `cmd`. The subprocess backend repairs
only the psmux child's copy and preserves any custom extensions from the caller.
It resolves `tmux` with the caller's Windows `PATH` and an explicit `.exe`
extension; a WinGet installation normally exposes
`%LOCALAPPDATA%\Microsoft\WinGet\Links\tmux.exe`.
It also disables psmux's warm-claim path for every child: psmux 3.3.7
reserializes the caller's working directory through a command-line protocol,
where a standalone semicolon can become another command.

The Windows workflow runs the native MSVC gates in C++20 and C++23, then
repeats every Windows-labeled CTest from a task-owned Alpine WSLv1 import. The
rootfs and psmux archive and executable are all pinned by SHA-256.

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
launching `tmux.exe`. Check `ServerFeature` before choosing a workflow; a
custom backend is unknown and supports nothing until it identifies its own
contract. Unsupported typed calls return `FailureKind::unsupported` with
`dispatched == false`.

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

The legacy `Session::attach_command()` and `Window::target()` keep their POSIX
signatures and return empty values on Windows. New code should use
`checked_attach_command()` and `checked_target()` to receive the explicit
unsupported failure. Ambiguous `-L default`, unsafe registry names, and typed
arguments containing psmux command separators or line breaks are malformed
requests instead, so they report `FailureKind::validation`.

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

`Server::run`, batches, and chains remain raw escape hatches. They use psmux's
mutable most-recent session when the command has no exact target. Psmux 3.3.7
also prefix-matches `-L` registry names, so raw `list-sessions` can include a
selector such as `name__nested`, and raw `kill-server` can kill it. Typed
listing and cleanup filter and target sessions individually; raw commands do
not provide that isolation. Psmux also joins raw argv back into a line protocol;
semicolons and newlines can start another command. Raw callers own that parsing
boundary.

Typed method names and MCP `readOnlyHint` annotations describe the command the
library requests, not a sandbox against hostile server configuration. Tmux and
psmux both expand server-side `command-alias` entries before built-in lookup,
so an administrator can redefine an apparent read. Use only servers and
configurations you trust. `PSMUX_CONFIG_FILE` controls a server created with
it; it does not attest an already-running server's live alias map.

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

This is subprocess compatibility, not transport equivalence. Windows builds
the dedicated psmux example and the capability-aware MCP server, but not the
POSIX examples, fuzzers, or `libtmux::testing` fixture. Asking CMake for either
of the latter two fails during configuration. The Windows MCP catalog exposes
only the four psmux-safe read operations listed above; creation, pane output,
input, search, and streaming tools remain POSIX-only.

Psmux reports a tmux-shaped version but does not promise every command has
identical behavior. The native smoke is pinned by behavior, not by a claimed
compatibility level, and has been exercised against psmux 3.3.7. See its
[integration guide][psmux-integration] and
[compatibility notes][psmux-compatibility], and test the operations your program
depends on against the installed psmux release.

Psmux 3.3.7 keeps its routing registry in the Windows profile's `.psmux`
directory and does not honor `PSMUX_DATA_DIR`. The smoke therefore uses a
high-entropy `-L` selector, clears inherited routing variables, performs only
exact per-session cleanup, and verifies that no exact registry file for its
selector and sessions remains. A deliberately nested prefix is kept alive. It
never issues `kill-server`. Psmux itself scans that global registry and reaps
entries it considers orphaned before it parses `-L`; the library cannot isolate
or disable that upstream maintenance in 3.3.7. Cleanup is bounded and
best-effort if another process continuously renames or creates sessions. Its
deadline is the Server's `ExecutionPolicy`; an explicitly unbounded policy is
unbounded here too. Do not treat a psmux namespace as a lock or security
boundary from other local psmux users.

## What the design commits to

**An entity is one row of a shared snapshot.** It holds the listing it came
from and the index of its row, so it reads its fields without reaching tmux:
the process ran once, when the snapshot was taken.

**Traversal and mutation go through the entity**, addressed by the id tmux gave
the object, so no operation can be redirected by a name containing a target
separator. A command that creates something returns it; a command that changes
something returns nothing, so nothing is re-read that the caller did not ask
for.

**Filters are values, not expression templates.** A `FilterExpr` owns every
operand it compares against, so an expression built from a local string still
works after that string dies.

**One failure type.** Every call reports `CommandFailure`, including the
factories, so `Server::from_env().and_then(...)` composes. The one exception
follows a rule worth stating: a call reports the failure type its *result*
speaks, so `Server::control` — which hands back a `Connection` — reports
`ProtocolError` like the `Connection` does.

**The transport is private, and replaceable.** No process type appears in any
installed header. The suite substitutes a scripted executor and runs the whole
public surface with no tmux present, which is also how the argv each operation
sends is pinned.

**[The full rationale, and what was measured →](docs/README.md)**

## Repository layout

| Where | What |
|---|---|
| [`include/libtmux/`](include/libtmux/README.md) | The public headers. If it is not here, it is not the contract. |
| [`src/`](src/README.md) | Implementation, and the private headers the transport seam hides behind. |
| [`tests/`](tests/README.md) | The suite, against real tmux, plus programs that must *not* compile. |
| [`examples/`](examples/README.md) | Programs that read top to bottom, and the tmuxp workspace builder. |
| [`apps/`](apps/README.md) | Programs built on the library — chiefly [the MCP server](apps/mcp/README.md). |
| [`tools/`](tools/README.md) | The parity ledger, the mutation catalogue, the reference generator. Never installed. |
| [`docs/`](docs/README.md) | Design notes, bakeoffs, and the generated [API reference](docs/api.md). |
| [`ports/`](ports/libtmux/) | The vcpkg port, served by [this repository's registry](docs/vcpkg-registry.md). |
| [`versions/`](versions/) | The registry's versions database. Generated, never edited by hand. |
| [`cmake/`](cmake/README.md) | Helper modules and the package config template. |

## How it is checked

Pull requests and master changes run against real tmux under clang with
libc++, GCC with libstdc++, the C++20 build over `tl::expected`, address and
undefined-behaviour sanitizers, the thread sanitizer, a locale that is not
UTF-8, and macOS with Apple Clang. The tmux matrix spans pinned compatibility
cells from 3.2a through 3.7b, plus `master`. The examples run as tests, and so
do the programs that must fail to compile.

The four core parsers for tmux-controlled text — rows, options, version output,
and control-mode frames — each have a fuzz harness and checked-in corpus. The
MCP server's JSON framing and protocol validation have their own hostile-input
and recovery tests.

A green suite is only evidence once it has been shown able to fail, so
[a mutation catalogue](tools/README.md#the-mutation-catalogue) breaks one guard
at a time and reports any that nothing notices.

## When you might not need this

- **You want a workspace from a config file.** [tmuxp] already does that, and
  does it well. [`examples/workspace/`](examples/workspace/README.md) reads the
  same documents if you need it inside a C++ program.
- **You are scripting, not building.** A shell script calling `tmux` is a fine
  answer to a one-off. This earns its keep when the thing you are writing has
  to *read* tmux's state and decide something.
- **You want it from Python.** Use [libtmux] — it is the original, has a wider
  surface, and this port is measured against it.
- **You need persistent control mode on Windows.** The psmux integration uses
  one `tmux.exe` subprocess per call.

## Project links

**Reference:**
[API](docs/api.md) ·
[Examples](examples/README.md) ·
[MCP server](apps/mcp/README.md) ·
[Tests](tests/README.md) ·
[Tooling](tools/README.md) ·
[vcpkg registry](docs/vcpkg-registry.md)

**Design notes:**
[Entity behaviour](docs/bakeoffs/entity-behavior/scorecard.md) ·
[Control transport](docs/design/control-transport.md) ·
[C++20 fallback](docs/design/cxx20-fallback.md) ·
[Engine-ops study](docs/design/engine-ops-study.md) ·
[Reviews](docs/evidence/library-review.md) ·
[Prerelease audit](docs/evidence/prerelease-audit.md)

**Related:**
[libtmux (Python)][libtmux] ·
[tmuxp] ·
[libtmux-mcp](https://github.com/tmux-python/libtmux-mcp) ·
[The Tao of tmux](https://leanpub.com/the-tao-of-tmux)

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers getting a build, what a change needs
to carry, and the gate to run before opening a pull request. The short version:
every capability needs a test against real tmux, failures are returned rather
than thrown, and no test starts a tmux server the fixture did not make.

## License

MIT. See [LICENSE](LICENSE).

[libtmux]: https://libtmux.git-pull.com
[tmuxp]: https://tmuxp.git-pull.com
[tmux]: https://github.com/tmux/tmux
[psmux]: https://github.com/psmux/psmux
[psmux-integration]: https://github.com/psmux/psmux/blob/master/docs/integration.md
[psmux-compatibility]: https://github.com/psmux/psmux/blob/master/docs/compatibility.md
