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
> **Alpha prerelease — `0.1.0-alpha.1`.** The API is not stable. Names,
> signatures and behaviour may change between alphas with no deprecation
> period, and there is no upgrade path promised until `0.1.0`. It is tested
> hard — against real tmux, across compilers and tmux versions — but "tested"
> and "settled" are different claims, and only the first one is being made.

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
- **A faster transport, same calls.** [Control mode](#batches-chains-and-control-mode)
  keeps one connection open — about 4.6× faster per listing.
- **An [MCP server](#the-mcp-server)** so an agent can drive tmux directly.
- **Compile-time refusals.** `pane::active.starts_with("x")` is a build error,
  and [a test proves it stays one](tests/README.md#the-compile-tests-are-the-interesting-ones).

## Requirements & support

| | |
|---|---|
| **tmux** | 3.2a or newer, through `master` — [with these exceptions](#compatibility) |
| **C++** | C++23 (clang 17+, GCC 13+), or C++20 with `LIBTMUX_CXX_STANDARD=20` |
| **CMake** | 3.25 |
| **Platforms** | Linux, macOS |
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
  GIT_TAG        v0.1.0-alpha.1  # or a commit; never a moving branch
)
FetchContent_MakeAvailable(libtmux)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

Embedded this way the library builds alone: its tests, examples and the MCP
server all default off when it is not the top-level project.

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

Put a `vcpkg-configuration.json` beside your manifest. Leave `baseline` empty;
the next command fills it in:

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

Depend on it from `vcpkg.json` as usual:

```json
{
  "name": "your-project",
  "version": "0.1.0",
  "dependencies": ["libtmux"]
}
```

Then let vcpkg pin both baselines — this registry's, and its own:

```console
$ vcpkg x-update-baseline --add-initial-baseline
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

It installs as a program, at
`installed/<triplet>/tools/libtmux/libtmux-mcp-server` — nothing links it.

### Build options

| Option | Default | What it does |
|---|---|---|
| `LIBTMUX_CXX_STANDARD` | `23` | `20` uses `tl::expected`: a system one, or the pinned fallback |
| `LIBTMUX_BUILD_TESTS` | on if top-level | The suite, which needs tmux and GoogleTest |
| `LIBTMUX_BUILD_EXAMPLES` | on if top-level | The programs in [`examples/`](examples/README.md) |
| `LIBTMUX_BUILD_MCP_SERVER` | off | [The MCP server](#the-mcp-server), which needs a JSON parser |
| `LIBTMUX_FETCH_DEPS` | off | Download what is missing instead of failing |

`LIBTMUX_FETCH_DEPS` is off on purpose: a build that would reach the network
says so and stops, rather than downloading quietly. That covers every
dependency, the C++20 lane's `tl::expected` included — it looks for an
installed one first, and says what to install when there is none.

## Quickstart

Every C++ example below is compiled and run against a real tmux on every build.
They are not written here — they live in
[`examples/05-readme.cpp`](examples/05-readme.cpp), and
[a check](tools/docs/check_readme.py) fails the build if this file and that one
ever disagree.

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
// Options are read and written where tmux scopes them, and a value survives
// the round trip whatever is in it.
(void)session.set_option("@project", "libtmux");

const auto project = session.option("@project");
if (project.has_value()) {
  std::printf("@project is %s\n", project->value.c_str());
}
```

Options are read and written at the scope that holds them — server, session,
window, pane. A value inherited from a wider scope is reported as inherited
rather than as one set here, because those differ when you are deciding
whether to write.

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
operation over a held-open connection — the same calls, without a process
each, about 4.6× faster per listing. See
[`docs/design/control-transport.md`](docs/design/control-transport.md).

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
[Model Context Protocol](https://modelcontextprotocol.io) over stdio from a
single binary with no runtime.

```console
$ cmake -S . -B build/mcp -DLIBTMUX_BUILD_MCP_SERVER=ON -DLIBTMUX_FETCH_DEPS=ON -DLIBTMUX_BUILD_TESTS=OFF -DLIBTMUX_BUILD_EXAMPLES=OFF
```

```console
$ cmake --build build/mcp && cmake --install build/mcp --prefix ~/.local
```

```console
$ claude mcp add tmux -- ~/.local/bin/libtmux-mcp-server
```

Five tools: `list_sessions`, `list_panes`, `capture_pane`, `send_text`,
`new_window`. A deliberately small surface — for full coverage use the Python
[libtmux-mcp](https://github.com/tmux-python/libtmux-mcp).
**[Full documentation →](apps/mcp/README.md)**

## Testing your own tmux tools

Writing something that drives tmux? The fixture this project's own suite runs
on is installed with the package. Ask for it by name:

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
above every numbered release. CI builds and tests against every supported
version, on every change.

Across that range tmux does not answer identically, and where it cannot the
library cannot either. These are the exceptions, each covered by a test that
skips below the release that provides it and runs everywhere above:

| Needs | Capability |
|---|---|
| tmux 3.3 | `display-message -c` addressed at a control client |
| tmux 3.3 | `-x` and `-y` honoured on a detached `new-session` |
| tmux 3.4 | `%message` delivered to an attached control client |
| tmux 3.4 | notification-shaped command output kept in the block body |

Two are defects rather than missing features — tmux answered wrongly and a
later release fixed it, so the affected window is closed at both ends:

| Broken on | What tmux does |
|---|---|
| tmux 3.4 – 3.5 | Echoes a non-UTF-8 byte back as its octal escape rather than the byte |
| tmux 3.4 | Adds a backslash before `$` when an option value it printed is set again |

Everything else holds across the whole range. A `split` carrying a percentage
is spelled `-l 25%` rather than the `-p 25` that tmux 3.4 removed, so it works
on every supported release — Python libtmux still emits `-p` and does not.

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

Every change runs against real tmux under clang with libc++, GCC with
libstdc++, the C++20 build over `tl::expected`, address and
undefined-behaviour sanitizers, the thread sanitizer, a locale that is not
UTF-8, and macOS with Apple Clang. The tmux matrix covers every supported
release from 3.2a to `master`. The examples run as tests, and so do the
programs that must fail to compile.

Every parser reads input the library does not choose — what tmux printed, what
a program inside a pane put in a title, a version string a distribution may
have patched, whatever arrives on a control socket. Each has a fuzz harness and
a checked-in corpus.

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
- **You need Windows.** tmux does not run there, so neither does this.

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
