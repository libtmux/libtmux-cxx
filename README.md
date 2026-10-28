<!-- libtmux-logo -->
<div align="center">
  <picture>
    <source srcset="assets/logo.svg" type="image/svg+xml">
    <img src="assets/logo.png" width="128" height="128" alt="libtmux for C++">
  </picture>
</div>
<!-- /libtmux-logo -->

<div align="center">
  <h1>libtmux for C++</h1>
  <p><strong>Drive tmux from C++: typed, value-semantic control over servers, sessions, windows, and panes.</strong></p>
  <p>
    <a href="https://github.com/libtmux/libtmux-cxx/actions/workflows/ci.yml"><img src="https://github.com/libtmux/libtmux-cxx/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
    <a href="https://libtmux.org/en/cxx/"><img src="https://img.shields.io/badge/docs-libtmux.org-0b7285" alt="libtmux.org"></a>
    <a href="https://github.com/libtmux/libtmux-cxx/blob/master/LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT licence"></a>
    <a href="#requirements--support"><img src="https://img.shields.io/badge/C%2B%2B-23%20%7C%2020-blue.svg" alt="C++23 or C++20"></a>
  </p>
</div>

> [!WARNING]
> **Alpha.** Releases carry an `-alpha` prerelease tag. The API is not
> settled, and any release may change or remove exported identifiers without a
> deprecation period. Pin an exact version. Not recommended for production.

## What is libtmux?

A C++ port of [libtmux], the Python library behind [tmuxp]. Work with
[`Server`][ref-server], [`Session`][ref-session], [`Window`][ref-window] and
[`Pane`][ref-pane] as ordinary C++ values, instead of running `tmux ls` and
parsing its output.

- **Failure is a value.** No tmux or transport error is thrown; a call returns
  the answer or a [`CommandFailure`][ref-commandfailure] saying why not.
- **Queries are typed.** A [`FilterExpr`][ref-filterexpr] is built from typed
  fields, so a filter that asks a number whether it starts with a string does
  not compile.
- **Nothing to link.** The core has no dependencies, in C++23 over
  `std::expected` or C++20 over a pinned `tl::expected`.
- **Value semantics.** An entity copies, compares, hashes, prints and formats:
  `std::format("{}", window)` writes [`Window(@1 2:editor)`][ref-window].

[Find what a server holds](#find-sessions-windows-and-panes),
[filter it](#query-with-typed-filters),
[type into a pane and wait](#type-into-a-pane-and-wait), and
[hear about changes](#batches-chains-and-control-mode) as tmux makes them.

It is for tools that drive a terminal program, test suites that need a real
tmux, and agents.

## Requirements & support

| | |
|---|---|
| **tmux** | 3.3a or newer, through `master` — [with these exceptions](#compatibility) |
| **C++** | C++23 (clang 17+, GCC 13+, Visual Studio 2022 17.3+), or C++20 with `LIBTMUX_CXX_STANDARD=20` |
| **CMake** | 3.25 |
| **Platforms** | Linux, including WSL; macOS; experimental x64 desktop Windows support with Visual Studio 2022 through [psmux] |
| **Dependencies** | None for the library |

- tmux releases below 3.3a are refused with
  [`FailureKind::unsupported`][ref-failurekind] before a command has any effect.
- The library and its examples need POSIX tmux; the
  [Windows preview](#windows-through-psmux) is read-mostly.
- Child launches do not inherit unrelated descriptors; the
  [details](docs/runtime.md#child-process-descriptors) cover each platform.

## Packages

- [`libtmux`](#libtmux): the library. Typed, value-semantic servers, sessions,
  windows and panes; the core links nothing.
- [`libtmux::testing`](#libtmuxtesting): a private tmux server for your own
  tests, installed as a CMake component.
- [`tmux-workspace`](#tmux-workspace): a command that loads, captures and
  converts tmuxp-style workspace files.
- [`libtmux-mcp-server`](#libtmux-mcp-server): a Model Context Protocol server,
  so an agent can drive tmux.

**Contents** — [Quick start](#quick-start) ·
[Find things](#find-sessions-windows-and-panes) ·
[Query](#query-with-typed-filters) ·
[Type and wait](#type-into-a-pane-and-wait) ·
[Failure](#when-things-fail) · [Options](#options) ·
[Control and async](#batches-chains-and-control-mode) ·
[Testing](#testing-your-own-tmux-tools) · [Install](#installation) ·
[Compatibility](#compatibility) · [Leaves out](#what-it-leaves-out)

## Quick start

Type a command into tmux and wait for its answer. This is the whole program:

<!-- cpp-file: examples/08-quickstart.cpp -->

```cpp
// Command quickstart types a command into tmux and waits for its answer.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>

#include <unistd.h>

#include <libtmux/libtmux.hpp>

namespace {

// Stops the server on every path out of `run`.
struct StopOnExit {
  const libtmux::Server& server;
  ~StopOnExit() { (void)server.run({"kill-server"}); }
};

int run(const std::filesystem::path& socket) {
  // A server on a socket of its own, with no configuration file, so this
  // neither touches nor is changed by a tmux you already use.
  const auto server = libtmux::Server::startable_at_socket_path(
      socket, std::filesystem::path{"/dev/null"});
  if (!server.has_value()) {
    std::cerr << std::format("{}\n", server.error());
    return 1;
  }
  const StopOnExit stop{*server};

  // The first session starts the server. Its pane runs `sh`, so the prompt
  // does not depend on your login shell.
  const auto session =
      server->new_session({.name = "demo", .shell_command = "exec sh"});
  const auto pane = session.and_then(
      [](const libtmux::Session& created) { return created.active_pane(); });
  if (!pane.has_value()) {
    std::cerr << std::format("{}\n", pane.error());
    return 1;
  }

  // The terminal echoes the typed line, so wait for the answer, which the
  // typed line never contains.
  (void)pane->send_line("echo \"6 x 7 = $((6 * 7))\"");
  const auto answered =
      pane->wait_for_text("6 x 7 = 42", {.timeout = std::chrono::seconds{5}});
  if (!answered.has_value() || !answered->matched) {
    std::cerr << "the pane never printed the answer\n";
    return 1;
  }
  std::cout << "the pane answered 6 x 7 = 42\n";
  return 0;
}

} // namespace

int main() {
  char directory[] = "/tmp/libtmux-quickstart-XXXXXX";
  if (mkdtemp(directory) == nullptr) {
    return 1;
  }
  const int status = run(std::filesystem::path{directory} / "tmux.sock");
  std::filesystem::remove_all(directory);
  return status;
}
```

It prints:

<!-- text-file: examples/08-quickstart.out -->

```text
the pane answered 6 x 7 = 42
```

Save it as `main.cpp`, add `add_executable(quickstart main.cpp)` and
`target_link_libraries(quickstart PRIVATE libtmux::libtmux)` to the project in
[CMake FetchContent](#cmake-fetchcontent), and build:

```console
$ cmake -S . -B build && cmake --build build && ./build/quickstart
```

- [`Server::startable_at_socket_path`][ref-server-startable_at_socket_path]
  names a socket of its own and `/dev/null` as the configuration, so the
  program neither touches nor is changed by a tmux you already use.
- [`Server::new_session`][ref-server-new_session] starts that server;
  [`Pane::send_line`][ref-pane-send_line] types, and
  [`Pane::wait_for_text`][ref-pane-wait_for_text] waits for the output.
- Use [`Server::at_default()`][ref-server-at_default] to drive the server your
  own `tmux` command does.

Every C++ block below is quoted from a program the suite compiles and runs
against a real tmux, in
[`examples/05-readme.cpp`](examples/05-readme.cpp), and
[a check](tools/docs/check_readme.py) fails the build if they drift apart.

## Find sessions, windows and panes

### [`Server::sessions`][ref-server-sessions]: what tmux has now

- [`Server::at_socket_path`][ref-server-at_socket_path] and
  [`Server::at_socket_name`][ref-server-at_socket_name] reach a specific server.
- [`Server::from_env`][ref-server-from_env] reaches the one the program runs
  inside, and [`Server::at_default`][ref-server-at_default] the one a person
  means by "my tmux".

[`Server::sessions`][ref-server-sessions] lists sessions;
[`Server::windows`][ref-server-windows] and
[`Server::panes`][ref-server-panes] are its siblings:

```cpp
// Given: const libtmux::Server& server
// No tmux failure is thrown. Every call answers with a value that is either
// the result or the reason there isn't one.
const auto sessions = server.sessions();
if (!sessions.has_value()) {
  std::cerr << std::format("{}\n", sessions.error());
  return 1;
}

for (const libtmux::Session& session : *sessions) {
  std::cout << std::format("{} has {} window(s)\n", session.name(),
                           session.window_count());
}
```

This runs against a private server and prints one line per session. A
[`Session`][ref-session] holds what tmux said when you listed it;
[`Window::refresh`](#windowrefresh-read-an-entity-again) reads it again.

### [`Pane::window`][ref-pane-window]: walk up and down

Every entity knows the server it came from, so [`Pane::window`][ref-pane-window]
and [`Pane::session`][ref-pane-session] reach its parents, and
[`Session::windows`][ref-session-windows] and
[`Window::panes`][ref-window-panes] its children, without a target string:

```cpp
// Given: const libtmux::Pane& pane
// Every entity knows the server it came from, so it can reach its children
// and its parents without a target string.
const auto window = pane.window();
const auto owner = pane.session();
if (window.has_value() && owner.has_value()) {
  std::cout << std::format("{} is in {}, in {}\n", pane, *window, *owner);
}
```

This prints which window and session a pane is in. Each relation call is a new
read; the entity you started from still holds its earlier snapshot.

### [`Window::refresh`][ref-window-refresh]: read an entity again

An entity is one row of the listing that produced it, so reading a field costs
nothing and never fails, and answers for the moment it was listed.
[`Window::refresh`][ref-window-refresh] returns a new value and leaves the old
one as it was:

```cpp
// Given: const libtmux::Session& session
// An entity holds the row its listing returned. Ask again to read the
// present.
const auto editor = session.new_window({.name = "editor"});
if (!editor.has_value()) {
  std::cerr << std::format("{}\n", editor.error());
  return 1;
}
(void)editor->rename("renamed");

std::cout << std::format("held: {}\n", editor->name()); // still "editor"

const auto now = editor->refresh();
if (now.has_value()) {
  std::cout << std::format("now: {}\n", now->name()); // "renamed"
}
```

This prints `held: editor`, then `now: renamed`. It is the one thing most likely
to surprise.

## Query with typed filters

A [`FilterExpr`][ref-filterexpr] is built from typed fields. Fields compose
with `&&`, `||` and `!`, and [`matching`][ref-matching] applies an expression to
a listing you hold, as a standard range.
[Concepts][topic-queries] explains how a query narrows from a server to a pane.

### [`matching`][ref-matching]: find the pane you mean

[`first`][ref-first] takes the first match of a [`matching`][ref-matching]
range. This finds the active pane running an editor and presses Escape in it:

```cpp
// Given: const libtmux::Server& server
// Every call answers with a value: the result, or the reason there is none.
{
  const auto panes = server.panes();
  if (!panes.has_value()) {
    std::cerr << std::format("{}\n", panes.error());
    return 1;
  }

  // Find the active pane running an editor, and press Escape in it.
  auto editing =
      *panes | libtmux::matching(libtmux::pane::command.starts_with("nv") &&
                                 libtmux::pane::active);

  if (auto pane = libtmux::first(editing)) {
    (void)pane->get().send_key("Escape");
  }
}
```

Expressions combine, and an expression owns what it compares against, so one
built from a local string still works after that string has gone:

```cpp
// Given: const libtmux::Server& server
// A filter is built from typed fields. Filters compose with `&&`, `||` and
// `!`, and applying one gives a standard range.
{
  const auto panes = server.panes();
  if (!panes.has_value()) {
    std::cerr << std::format("{}\n", panes.error());
    return 1;
  }
  const auto interesting =
      (libtmux::pane::command == "bash" || libtmux::pane::command == "zsh") &&
      !libtmux::pane::dead;

  for (const libtmux::Pane& shell : *panes | libtmux::matching(interesting)) {
    std::cout << std::format("{} is a live shell, {} columns wide\n",
                             shell.id().value(), shell.width());
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
    std::cout << std::format("{} window(s) called editor\n", found);
  }
}
```

This prints one line per live shell, then how many windows are called `editor`.

### [`window::index`][ref-numberfieldhandle]: sort, count and project

A field handle ([`NumberFieldHandle`][ref-numberfieldhandle],
[`BoolFieldHandle`][ref-boolfieldhandle],
[`StringFieldHandle`][ref-stringfieldhandle]) is the accessor as well as the
name, so the vocabulary that filters also sorts, counts and projects:

```cpp
// Given: const libtmux::Server& server
// A handle reads a row, so the name that filters also projects, and a flag
// handle is a predicate on its own. The standard algorithms take them as
// they are, with no lambda naming the accessor a second time.
const auto windows = server.windows();
if (windows.has_value() && !windows->empty()) {
  std::vector<libtmux::Window> ordered = *windows;
  std::ranges::sort(ordered, {}, libtmux::window::index);

  const auto active = std::ranges::count_if(ordered, libtmux::window::active);
  const libtmux::Window widest =
      std::ranges::max(ordered, {}, libtmux::window::width);
  std::cout << std::format("{} of {} active, widest {}\n", active, ordered.size(),
                           widest);
}
```

A [`FilterExpr`][ref-filterexpr] is itself a predicate, so
`std::ranges::count_if(*windows, libtmux::window::width > 40)` asks a typed
question of any standard algorithm.

### [`exactly_one`][ref-exactly_one]: exactly one, or say why not

[`exactly_one`][ref-exactly_one] answers a question every caller reimplements
around `.size() == 1`; [`first`][ref-first] is its lenient sibling:

```cpp
// Given: const libtmux::Server& server
// `exactly_one` replaces a `.size() == 1` check, and says why it failed.
const auto panes = server.panes();
if (!panes.has_value()) {
  std::cerr << std::format("{}\n", panes.error());
  return 1;
}
auto addressed =
    *panes | libtmux::matching(libtmux::pane::id == panes->at(0).id().value());

if (const auto one = libtmux::exactly_one(addressed); one.has_value()) {
  std::cout << std::format("exactly one: {}\n", one->get());
}

// And when it is not one, the answer says which way it went wrong.
auto absent = *panes | libtmux::matching(libtmux::pane::command == "no-such-command");

if (const auto none = libtmux::exactly_one(absent); !none.has_value()) {
  std::cout << std::format("not one: {}\n", libtmux::to_string(none.error()));
}
```

This prints `exactly one:` with the pane, then `not one:` with the reason.
[`first`][ref-first] and `exactly_one` take a named range and return a reference
into it, so a temporary range does not compile.
[`first_owned`][ref-first_owned] and [`exactly_one_owned`][ref-exactly_one_owned]
take ownership instead.

### Fields refuse the wrong question

The compiler rejects a question a field cannot answer. Each of these is a build
error, and a test in [`tests/compile/`](tests/compile/) proves it stays one:

| Will not compile | Why |
|---|---|
| [`pane::active.starts_with("1")`](tests/compile/flag_field_has_no_string_operations.cpp) | A flag renders as `1` or `0`; comparing that text is the mistake |
| [`pane::width.contains("8")`](tests/compile/numeric_field_has_no_string_operations.cpp) | A size compares as a number, where 9 is less than 10 rather than after it |
| [`window::name > 5`](tests/compile/string_field_has_no_ordering.cpp) | Ordering a name as text is a different question from ordering numbers |

## Type into a pane and wait

### [`Session::new_window`][ref-session-new_window]: build a layout

[`Session::new_window`][ref-session-new_window] and
[`Window::split`][ref-window-split] build an arrangement without composing a tmux
argument, and [`Pane::send_line`][ref-pane-send_line] types text and Enter in one
call:

```cpp
// Given: const libtmux::Session& session
// Build an arrangement without composing a single tmux argument.
{
  const auto editor = session.new_window({.name = "editor"});
  if (!editor.has_value()) {
    std::cerr << std::format("{}\n", editor.error());
    return 1;
  }

  const auto logs = editor->split({.horizontal = true, .percentage = 30});
  if (!logs.has_value()) {
    std::cerr << std::format("{}\n", logs.error());
    return 1;
  }

  // Text and Enter in one invocation, which is what running a command in a
  // pane means. `send_text` and `send_key` stay available for the times the
  // two halves are separate acts.
  (void)logs->send_line("journalctl -f");
}
```

Every target is the id tmux gave the object, so a window called `my:window`
cannot redirect an operation.

### [`Pane::wait_for_text`][ref-pane-wait_for_text]: wait for what a command prints

[`Pane::wait_for_text`][ref-pane-wait_for_text] returns a
[`WaitResult`][ref-waitresult]; [`WaitOptions`][ref-waitoptions] sets the
timeout, and [`WaitPath`][ref-waitpath] says whether a control-mode `%output`
notification or a capture answered:

```cpp
// Given: const libtmux::Pane& shell
// Type a command, then wait for what it prints. The terminal echoes the typed
// line too, so wait for text the answer has and the typed line lacks.
(void)shell.send_line("echo \"6 x 7 = $((6 * 7))\"");

const auto answered =
    shell.wait_for_text("6 x 7 = 42", {.timeout = std::chrono::seconds{5}});
if (!answered.has_value() || !answered->matched) {
  std::cerr << "the pane never printed the answer\n";
  return 1;
}
std::cout << "the pane answered 6 x 7 = 42\n";
```

This runs against a pane running `sh` and prints `the pane answered 6 x 7 = 42`.
The terminal echoes the typed line, so the example waits for the answer, which
the typed line never contains.

[Pane interaction][topic-pane] and [waiting and retrying][topic-waiting] cover
the rest.

### [`Pane::capture`][ref-pane-capture]: read what a pane shows

[`Pane::capture`][ref-pane-capture] reads the visible screen, or the scrollback
when [`CaptureOptions`][ref-captureoptions] asks for it:

```cpp
// Given: const libtmux::Pane& pane
// Read a pane's visible contents, or its scrollback.
const auto visible = pane.capture();
if (visible.has_value()) {
  std::cout << std::format("{} bytes on screen\n", visible->size());
}

const auto history = pane.capture({.whole_history = true});
if (history.has_value()) {
  std::cout << std::format("{} bytes of scrollback\n", history->size());
}
```

A capture larger than [`CaptureOptions::output_limit`][ref-captureoptions]
fails with [`FailureKind::truncated`][ref-failurekind]; it never returns shortened
text.

## When things fail

No tmux or transport failure is thrown. A call returns an
[`expected`][ref-expected] holding the result or a
[`CommandFailure`][ref-commandfailure]; [errors and exceptions][topic-errors]
has the model.

### [`FailureKind`][ref-failurekind]: why a call did not answer

A caller can tell "you asked wrongly" from "tmux said no" from "tmux never
answered" by the [`FailureKind`][ref-failurekind] of a
[`CommandFailure`][ref-commandfailure]:

```cpp
// Given: const libtmux::Server& server
// Failures are values with a kind, so a caller can tell "you asked wrongly"
// from "tmux said no" from "tmux never answered".
const auto gone = server.run({"kill-session", "-t", "=no-such-session"});
if (!gone.has_value()) {
  switch (gone.error().kind) {
  case libtmux::FailureKind::validation:
    std::cout << "the request was malformed before it was sent\n";
    break;
  case libtmux::FailureKind::unsupported:
    std::cout << "this backend cannot provide the operation safely\n";
    break;
  case libtmux::FailureKind::refused:
    std::cout << std::format("tmux refused it: {}\n", gone.error().diagnostic);
    break;
  case libtmux::FailureKind::timeout:
    std::cout << "tmux did not answer in time\n";
    break;
  default:
    std::cout << std::format("{}\n", gone.error());
    break;
  }
}
```

This prints that tmux refused to kill a session that does not exist.

| Kind | Means |
|---|---|
| `validation` | The request was malformed; it never left the process |
| `unsupported` | The selected backend cannot provide the operation safely |
| `spawn`, `pre_exec`, `pipe` | tmux could not be started or talked to |
| `timeout` | A deadline expired |
| `refused` | tmux ran and said no |
| `missing` | The object is gone. tmux itself reports this as empty fields and exit zero |
| `truncated` | The answer did not fit the limit given |

[`DeliveryStatus`][ref-deliverystatus] says how far the command got:

| Delivery | Means |
|---|---|
| `not_started` | tmux definitely did not receive the command; blind retry is safe |
| `written` | the complete request reached the transport, but no terminal reply did |
| `replied` | tmux produced a terminal reply |
| `indeterminate` | the transport cannot prove whether tmux saw or completed it |

### [`expected`][ref-expected]: compose calls without nesting

One failure type covers the whole surface, so calls compose: each step runs only
when the last one answered, and the first failure is what comes out:

```cpp
// Given: const libtmux::Server& server; const libtmux::Session& session
// One failure type covers the whole surface, so calls chain without nesting:
// each step runs only when the last one answered, and the first failure is
// what comes out.
const auto columns =
    server.session(session.name())
        .and_then([](const libtmux::Session& found) { return found.active_pane(); })
        .transform([](const libtmux::Pane& active) { return active.width(); });
std::cout << std::format("the active pane is {} columns wide\n",
                         columns.value_or(-1));
```

A caller who wants an exception at a boundary asks for one: `.value()` throws
[`bad_expected_access`][ref-bad_expected_access] of
[`CommandFailure`][ref-commandfailure], the same type in both builds. The library
never throws it itself.

### [`Server::run`][ref-server-run]: anything tmux can do

[`Server::run`][ref-server-run] sends any tmux command, and
[`Pane::expand`][ref-pane-expand] expands a format against a pane. Nothing here
is a dead end, whether or not the typed surface has named it yet:

```cpp
// Given: const libtmux::Pane& pane; const libtmux::Server& server
// Anything tmux knows and this library does not name yet: ask it directly,
// with a format string expanded against a pane.
const auto running = pane.expand("#{pane_current_command}");
if (running.has_value()) {
  std::cout << std::format("running {}\n", *running);
}

// Or run a command and read its output.
const auto answer = server.run({"display-message", "-p", "#{version}"});
if (answer.has_value()) {
  std::cout << std::format("tmux {}", *answer); // tmux's answer ends in a newline
}
```

This prints the command running in the pane, then `tmux ` and its version.

## Options

### [`Session::set_option`][ref-session-set_option]: set an option, read it back

Options are read and written where tmux scopes them: server, session, window,
pane.

[`Session::set_option`][ref-session-set_option] writes one and
[`Session::option`][ref-session-option] reads it. A value inherited from a wider
scope is reported as inherited ([`OptionEntry`][ref-optionentry]):

```cpp
// Given: const libtmux::Session& session
// Options are read and written where tmux scopes them.
(void)session.set_option("@project", "libtmux");

const auto project = session.option("@project");
if (project.has_value()) {
  std::cout << std::format("@project is {}\n", project->value);
}
```

This prints `@project is libtmux`. POSIX tmux preserves arbitrary option text;
[options and hooks][topic-options] has the rest.

## Batches, chains and control mode

Four ways to send typed work, plus a raw control stream:

| | What it is | Failure |
|---|---|---|
| **[Call][ref-server-run]** | One command, one process | Reported on the call |
| **[Async][ref-commandruntime]** | One process per accepted command in an explicit runtime | Immediate refusal outside; later failure on the operation |
| **[Batch][ref-commandbatch]** | One tmux invocation, one fail-fast group | Partially applied, not rolled back |
| **[Chain][ref-chain]** | Validated as it is built | A bad target is caught before tmux is reached |
| **[Control][ref-connection]** | One connection held open | Ordered reply blocks before a private request boundary |

[Control mode versus one-shot commands][topic-transports] explains when to
choose each.

### [`Chain`][ref-chain]: refuse a bad batch before tmux

A [`Chain`][ref-chain] refuses a target it cannot address before reaching tmux
at all, so a malformed batch costs nothing:

```cpp
// A chain refuses a target it cannot address before reaching tmux at all,
// so a malformed batch costs nothing.
libtmux::Chain chain;
chain.new_window("a:b", "unreachable");
std::cout << std::format("chain valid: {}\n", chain.valid()); // false
```

This prints `chain valid: false`.

### [`Server::control`][ref-server-control]: hear changes as they happen

[`Server::control`][ref-server-control] opens a [`Connection`][ref-connection]
to one session. [`Connection::events`][ref-connection-events] waits for what
tmux reports, so nothing polls:

```cpp
// Given: const libtmux::Server& server
// Hold one control connection open, then let tmux say what changed.
auto connected = server.control("example");
if (!connected.has_value()) {
  std::cerr << std::format("{}\n", connected.error().message);
  return 1;
}
auto connection = *std::move(connected);

(void)server.run({"new-window", "-d", "-n", "watched"});

std::string added;
const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
for (const auto& event : connection.events(deadline)) {
  if (event.kind == libtmux::NotificationKind::window_add) {
    added = std::string{event.window};
    break;
  }
}
if (added.empty()) {
  std::cerr << "tmux never reported the new window\n";
  return 1;
}
std::cout << std::format("window added: {}\n", added);
```

This prints `window added:` and the id tmux gave the new window.

- Library-managed control mode needs tmux 3.7. An older daemon is refused
  before any client attaches; see
  [optional features by release](#optional-features-by-release).
- A `Server` runs ordinary tmux client processes, which return tmux's final exit
  status; use a `Connection` for notification streams and protocol tooling.
- `take_notifications` is a single draining cursor. For two independent
  consumers, open a [`NotificationWatch`][ref-notificationwatch].
- The [control transport design](docs/design/control-transport.md) has the
  rest. Windows psmux rejects control mode before launching a client.

## Bounded asynchronous commands

### [`Server::try_submit`][ref-server-try_submit]: submit, wait, or walk away

A [`CommandRuntime`][ref-commandruntime] owns the transport threads and a fixed
admission capacity. [`Server::try_submit`][ref-server-try_submit] returns a
[`CommandOperation`][ref-commandoperation] to wait on, detach or cancel:

```cpp
// Given: const libtmux::Server& server
// Submit one command to a bounded runtime, then wait for its result.
auto started =
    libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 1U});
if (!started.has_value()) {
  std::cerr << std::format("{}\n", started.error());
  return 1;
}
auto pool = *std::move(started);

auto job = server.try_submit(pool, {"display-message", "-p", "submitted"});
if (!job.has_value()) { // Refused before admission.
  std::cerr << std::format("{}\n", job.error());
  return 1;
}
const auto reply = std::move(*job).wait();
if (!reply.has_value()) { // Failed after admission.
  std::cerr << std::format("{}\n", reply.error());
  return 1;
}
std::cout << std::format("answered: {}", *reply);

if (const auto shutdown = pool.close(); shutdown.failure.has_value()) {
  std::cerr << std::format("{}\n", *shutdown.failure);
  return 1;
}
```

This prints `answered: submitted`. What a caller acts on:

- A refusal before admission (a full, stopped or moved-from runtime, an invalid
  command) is the outer `unexpected`, and safe to retry once its cause is fixed.
- After admission, every failure is the operation's eventual result; its
  [`DeliveryStatus`][ref-deliverystatus] says what the transport can prove.
- [`CommandOperation::wait_for`][ref-commandoperation-wait_for] expiring leaves
  the command running; [`request_cancel`][ref-commandoperation-request_cancel]
  asks the transport to withdraw it.
- Observers run only when you call
  [`CommandRuntime::dispatch_ready`][ref-commandruntime-dispatch_ready].
- [`CommandRuntime::close`][ref-commandruntime-close] blocks until the threads
  stop; `safe_to_unload` is true only once pending work is discharged.

[`docs/runtime.md`](docs/runtime.md) is the full contract: admission, the
three capacity legs, cancellation, counters and shutdown.

## Testing your own tmux tools

Writing something that drives tmux on POSIX? The fixture this project's own
suite runs on is installed with the package. Ask for it by name:

```cmake
find_package(libtmux REQUIRED COMPONENTS testing)
target_link_libraries(your_tests PRIVATE libtmux::testing)
```

### [`ScopedTmuxServer::start`][ref-test-scopedtmuxserver-start]: a private server for a suite

[`ScopedTmuxServer`][ref-test-scopedtmuxserver] gives a suite a server on its own
socket, and [`SocketNamespace::consumer`][ref-test-socketnamespace] names the
suite that left a stray directory:

```cpp
// A private tmux for a suite of your own, gone when the scope ends.
auto fixture = libtmux::test::ScopedTmuxServer::start(
    {.socket_namespace = libtmux::test::SocketNamespace::consumer("my-suite")});
if (!fixture.has_value()) {
  std::cerr << std::format("{}\n", fixture.error());
  return 1;
}
const auto under_test =
    libtmux::Server::at_socket_path(fixture->socket_path().string());
std::cout << std::format("sessions on it: {}\n", under_test->sessions()->size());
```

This prints the number of sessions on the private server.

- The server has its own `TMUX_TMPDIR`, with `TMUX` and `TMUX_PANE` erased from
  the child environment, and is killed with its tree removed when the scope
  ends, including when the test fails.
- A suite that uses the default server ends somebody else's session, and the
  failure looks like a bug in whatever noticed first.
- It links no test framework, so GoogleTest, Catch2 and doctest all work.

**[Testing API reference →](docs/api-testing.md)** ·
**[How this project tests →](tests/README.md)** ·
**[A worked consumer →](examples/tests/README.md)**

## Installation

Nothing to install alongside it: the library has no dependencies. Pick whichever
of these your project already uses.

### CMake FetchContent

No install step, and nothing to vendor:

```cmake
include(FetchContent)
FetchContent_Declare(
  libtmux
  GIT_REPOSITORY https://github.com/libtmux/libtmux-cxx.git
  GIT_TAG        v0.1.0-alpha.14  # or a commit; never a moving branch
)
FetchContent_MakeAvailable(libtmux)
target_link_libraries(your_target PRIVATE libtmux::libtmux)
```

- Embedded this way the library builds alone: its tests, examples and the MCP
  server all default off when it is not the top-level project.
- `v0.1.0-alpha.14` requires tmux 3.3a and carries the optional `tmux-workspace`
  CLI and saved-layout validation.
- It also carries the experimental x64 desktop Windows preview for Visual Studio
  2022 and the audited psmux build in its source.
- Whether a release's vcpkg port carries Windows is a separate transition its
  tagged gates must clear, so check the `supports` field of the version you pin.

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
resolves `libtmux` by version the way it resolves anything else.

It is not in the curated registry: the project is far too new to meet vcpkg's
maturity bar, and serving your own registry is what vcpkg's documentation
recommends instead.

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

The [MCP server](#libtmux-mcp-server) is a vcpkg feature of `libtmux`, since it
ships from this same source. Ask for it by name:

```json
{
  "dependencies": [{ "name": "libtmux", "features": ["mcp"] }]
}
```

[`docs/vcpkg-registry.md`](docs/vcpkg-registry.md#the-mcp-feature) covers where
the program installs, its socket options, and the Windows triplets.

### Build options

| Option | Default | What it does |
|---|---|---|
| `LIBTMUX_CXX_STANDARD` | `23` | `20` uses `tl::expected`: a system one, or the pinned fallback |
| `LIBTMUX_BUILD_TESTS` | on if top-level | POSIX suite with tmux and GoogleTest; native psmux smoke on Windows |
| `LIBTMUX_BUILD_EXAMPLES` | on if top-level | Six POSIX programs or the native Windows psmux preview in [`examples/`](examples/README.md) |
| `LIBTMUX_BUILD_TESTING_LIBRARY` | on with tests or examples on POSIX; off on Windows | Install the POSIX private-server fixture |
| `LIBTMUX_BUILD_MCP_SERVER` | off | Build the [MCP server](#libtmux-mcp-server), which needs a JSON parser; Windows exposes its psmux-safe subset |
| `LIBTMUX_BUILD_FUZZERS` | off | Build the POSIX parser fuzzers |
| `LIBTMUX_FETCH_DEPS` | off | Download what is missing instead of failing |

- `LIBTMUX_FETCH_DEPS` is off by default: a build that needs the network says so
  and stops.
- It covers every dependency, the C++20 lane's `tl::expected` included: the
  build looks for an installed one first, and says what to install when there is
  none.

## Core concepts

| C++ type | tmux concept | Notes |
|---|---|---|
| [`Server`](include/libtmux/server.hpp) | tmux server / socket | Entry point; owns sessions |
| [`Session`](include/libtmux/entities.hpp) | session (`$0`, `$1`, …) | Owns windows |
| [`Window`](include/libtmux/entities.hpp) | window (`@1`, `@2`, …) | Owns panes |
| [`Pane`](include/libtmux/entities.hpp) | pane (`%1`, `%2`, …) | Where commands run |
| [`Client`](include/libtmux/entities.hpp) | an attached terminal | Read-only |
| [`Buffer`](include/libtmux/entities.hpp) | the server's clipboard | Named text outliving its pane |
| [`CommandRuntime`](include/libtmux/async.hpp) | bounded asynchronous execution | Owns threads, admission, and observer disposition |
| [`CommandOperation`](include/libtmux/async.hpp) | one accepted command result | Waited or detached independently of observation |
| [`Connection`](include/libtmux/control.hpp) | a control-mode session | Guarded blocks, outside-block events |
| [`NotificationWatch`](include/libtmux/notification.hpp) | one outside-block event consumer | Independent cursor, wait, and readiness descriptor |

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

## Package guides

### [`libtmux`](https://github.com/libtmux/libtmux-cxx/tree/master/include/libtmux)

Typed, value-semantic control of tmux servers, sessions, windows and panes.
Reference: the [API reference][ref-index], [getting started][guide-start] and
[concepts][guide-concepts]. The
[`include/libtmux/` README](include/libtmux/README.md) says what each public
header holds.

[`Server::capabilities`][ref-server-capabilities] reports what a
[`Server`][ref-server] handle can promise without starting tmux, so a program
can choose a workflow before its first command.
[`ServerCapabilities`][ref-servercapabilities] answers `supports(...)` for each
[`ServerFeature`][ref-serverfeature]:

```cpp
// Given: const libtmux::Server& server
// Ask what this server promises before choosing a workflow. The answer comes
// from the handle: it starts no process and cannot fail.
const libtmux::ServerCapabilities capabilities = server.capabilities();
const bool pane_io = capabilities.supports(libtmux::ServerFeature::pane_io);
std::cout << std::format("{} over {}: pane input and output supported: {}\n",
                         libtmux::to_string(capabilities.implementation),
                         libtmux::to_string(capabilities.backend), pane_io);
```

### [`libtmux::testing`](https://github.com/libtmux/libtmux-cxx/tree/master/include/libtmux/testing)

A private tmux server for your own tests, removed when the scope ends. POSIX
only. Reference: [Testing with libtmux][guide-testing] and the
[`ScopedTmuxServer`][ref-test-scopedtmuxserver] page;
[Testing your own tmux tools](#testing-your-own-tmux-tools) shows it in a suite.

A [`TeardownReport`][ref-test-teardownreport] passed to
[`ScopedTmuxServer::start`][ref-test-scopedtmuxserver-start] collects what teardown
did, including anything it could not clean up:

```cpp
// A private server, and the messages its teardown recorded.
{
  const auto report = std::make_shared<libtmux::test::TeardownReport>();
  {
    auto scoped = libtmux::test::ScopedTmuxServer::start({
        .socket_namespace = libtmux::test::SocketNamespace::consumer("report"),
        .teardown_report = report,
    });
    if (!scoped.has_value()) {
      std::cerr << std::format("{}\n", scoped.error());
      return 1;
    }
  }
  for (const std::string& message : report->messages) {
    std::cout << std::format("teardown: {}\n", message);
  }
}
```

### [`tmux-workspace`](https://github.com/libtmux/libtmux-cxx/tree/master/apps/workspace)

Loads, captures, converts and imports tmuxp-style workspace files from the
command line. Optional and POSIX only. Reference: the [command manual][ws-cli],
[`load`][ws-load] and [workspace configuration][ws-configuration]. The
[`apps/workspace/` README](apps/workspace/README.md) walks through each command.

Build and install it:

```console
$ cmake -S . -B build/workspace \
    -DLIBTMUX_BUILD_WORKSPACE_CLI=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    && cmake --build build/workspace \
    && cmake --install build/workspace --prefix ~/.local
```

With a workspace file saved as `project.yaml`, load it on a socket named
`project` without attaching:

```console
$ tmux-workspace load \
    -L project \
    -d \
    ./project.yaml
```

### [`libtmux-mcp-server`](https://github.com/libtmux/libtmux-cxx/tree/master/apps/mcp)

A [Model Context Protocol](https://modelcontextprotocol.io) server over stdio,
so an agent can drive tmux. Reference: the [MCP overview][mcp-index], the
[tool reference][mcp-tools] and [connecting a client][mcp-guide]. The
[`apps/mcp/` README](apps/mcp/README.md) lists the toolsets and the socket
options.

Build and install it:

```console
$ cmake -S . -B build/mcp \
    -DLIBTMUX_BUILD_MCP_SERVER=ON \
    -DLIBTMUX_FETCH_DEPS=ON \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_BUILD_EXAMPLES=OFF \
    && cmake --build build/mcp \
    && cmake --install build/mcp --prefix ~/.local
```

Register it with Claude Code, limited to the
[`inspect`][mcp-toolset-inspect] toolset:

```console
$ claude mcp add tmux \
    -e LIBTMUX_TOOLSETS=inspect \
    -- ~/.local/bin/libtmux-mcp-server --socket-name libtmux-agent
```

## Compatibility

tmux 3.3a and newer. The floor is the complete release: tmux 3.3 is below it.

- [`Version`][ref-version] orders releases correctly: `3.7 < 3.7a < 3.7b`, and
  `next-3.8` and `3.8-rc2` precede `3.8`.
- CI builds and tests pinned cells from 3.3a through 3.8, plus `master`, on
  every pull request and master change.
- The daemon a command reaches decides: a release below the floor, an unreadable
  version and an undated `master` are refused with
  [`FailureKind::unsupported`][ref-failurekind] before the command has any
  effect.
- [`docs/compatibility.md`](docs/compatibility.md) covers the probe, older
  daemons, [`Server::check_release()`][ref-server-check_release] and the
  `command-alias` caveat.

Across that range tmux does not answer identically, and where it cannot the
library cannot either. Each exception is covered by a test that skips below the
release that provides it:

| Needs | Capability |
|---|---|
| tmux 3.4 | `%message` delivered to an attached control client |
| tmux 3.4 | notification-shaped command output kept in the block body |
| tmux 3.6 | The created-window report when breaking a window's only pane |
| tmux 3.8 | JSON saved layouts, `{"V":2,...` |

Two are tmux defects that a later release fixed, so the affected window is
closed at both ends:

| Broken on | What tmux does |
|---|---|
| tmux 3.4 – 3.5 | Echoes a non-UTF-8 byte back as its octal escape rather than the byte |
| tmux 3.4 | Adds a backslash before `$` when an option value it printed is set again |

- Raw tmux 3.7 crashes an unnamed multi-pane `break-pane` and ignores an
  explicit name there. [`Pane::break_out`][ref-pane-break_out] selects the safe
  unnamed form and repairs a requested name by stable window ID; tmux 3.7a
  fixes both upstream.
- A `split` carrying a percentage is spelled `-l 25%` rather than the `-p 25`
  that tmux 3.4 removed, so it works on every supported release. Python libtmux
  still emits `-p`.

### Optional features by release

The floor above is for ordinary commands. A feature that needs a newer daemon
says so itself, is refused before it has an effect, and does not raise the floor
for anything else. The daemon decides, not the client executable.

| Feature | Daemon | Below it | State |
|---|---|---|---|
| Control mode: `Server::control`, `control_with_options`, `over_control`, `Connection::connect` | 3.7 | Refused, naming 3.7, before any client attaches ([why](docs/compatibility.md#control-mode)) | Qualified; attach on 3.7 and newer, idle only |
| `wait_for_text` streaming | 3.7 | Polls the pane; control mode is never tried | Operative with synchronization |
| `Buffer::remove`, a literal `delete-buffer -b` or `set-buffer -b -n`, the MCP `paste_text` staging buffer | 3.4 | `FailureKind::unsupported`, before any buffer is touched ([why](docs/compatibility.md#named-buffers)) | Qualified |
| `:` or `.` in session and window names, typed or literal | not exactly 3.7 | `FailureKind::validation`, before dispatch ([why](docs/compatibility.md#names-with-a-colon-or-a-period)) | Qualified |
| Mirrored `main-*` presets, typed or in a literal `select-layout` | 3.5 | `FailureKind::validation`, before dispatch | Qualified |
| JSON layouts, typed or in a literal `select-layout` | 3.8 | `FailureKind::validation`, before dispatch | Qualified |
| A layout string tmux cannot read, typed or literal | never sent | `FailureKind::validation` on every release, before dispatch ([why](docs/compatibility.md#unreadable-layout-strings)) | Qualified |
| A literal `refresh-client -l`, under any alias or unique prefix | 3.6a | `FailureKind::unsupported`, before dispatch; no typed call ([why](docs/compatibility.md#clipboard-query)) | Qualified |
| A literal `break-pane` without `-n`, through `Server::run`, a batch or `Connection::execute` | not exactly 3.7 | `FailureKind::validation`, in any spelling and in a `;` list; `Pane::break_out` names the window itself ([why](docs/compatibility.md#break-pane)) | Qualified on 3.7 |

The gates that have a machine-readable entry are in
[`capabilities.json`](capabilities.json).

A literal command is read the way tmux reads it: under its alias or any unique
prefix, and in each position of a `;` list. That holds for
[`Server::run`][ref-server-run], batches and chains, asynchronous submissions,
[`Server::over_control`][ref-server-over_control] and
[`Connection::execute`][ref-connection-execute].

A command carried inside a string that tmux parses itself is sent as written.
That covers:

- the body of `if-shell`, the argument of `run-shell -C`, and a brace block;
- a command stored in a hook, a key binding or a sourced file.

These are the refused commands, the releases they affect, and what happens:

- `break-pane` without `-n`: exact tmux 3.7 only. The server exits. 3.7a fixes
  it ([`CHANGES`](https://github.com/tmux/tmux/blob/3.7a/CHANGES#L3), [the
  inverted name test in 3.7](https://github.com/tmux/tmux/blob/3.7/cmd-break-pane.c#L117-L124)).
- `select-layout` with a string tmux cannot parse: tmux 3.3 and 3.3a. The
  server exits. 3.4 answers "invalid layout".
- `refresh-client -l`: below 3.6a, with an attached client. The server exits at
  once on 3.3a, and on 3.4 to 3.6 when the client leaves before its terminal
  answers ([why](docs/compatibility.md#clipboard-query)).
- `delete-buffer -b NAME`, and `set-buffer -b NAME -n`: below 3.4. No crash;
  the wrong buffer is changed ([why](docs/compatibility.md#named-buffers)).
- `split-window -p`: exactly 3.4. tmux answers "size missing" and changes
  nothing. This port does not refuse a raw `-p`; typed splits send `-l N%`.
- Names containing `:` or `.`: exactly 3.7. tmux answers "invalid session name"
  or "invalid window name" and changes nothing.

On exact tmux 3.7, name every `break-pane`, nested or not, or upgrade to 3.7a.
The [default `prefix !` binding](https://github.com/tmux/tmux/blob/3.7/key-bindings.c#L358)
is itself a nameless `break-pane`, which no library check can reach.

[`Server`][ref-server]`::run_shell` does not capture the job's output, so the
3.3 to 3.4 `run-shell` stdout defect does not reach it. Percentage splits are
always `-l N%`, and layout strings are validated before dispatch.

### Windows through psmux

An experimental x64 desktop preview, built with Visual Studio 2022, reaching a
Windows terminal through [psmux]. It serves what it can prove and refuses the
rest; [`docs/windows.md`](docs/windows.md) lists what it serves and what fails
before dispatch.

- Install psmux with its `tmux.exe` alias, and use
  [`Server::at_default()`][ref-server-at_default] or
  [`Server::at_socket_name(...)`][ref-server-at_socket_name].
- The verified target is psmux 3.3.7; any other binary is a new compatibility
  target until the native smoke passes against it.
- Ask [`Server::capabilities()`][ref-server-capabilities] first: a typed call
  psmux cannot target safely returns
  [`FailureKind::unsupported`][ref-failurekind] with `delivery == not_started`.
- Not available there: `-S` socket paths, persistent control mode, clients,
  buffers, options and hooks, session and window creation, and pane input and
  capture.
- [`Server::run`][ref-server-run], batches and chains stay raw escape hatches;
  psmux joins raw argv into a line protocol, so a semicolon can start another
  command.
- A server-side `command-alias` can redefine a command, so `readOnlyHint` and a
  typed method name do not guarantee a read is a read; use only servers you
  trust.

psmux is supported on a best-effort basis, and CI coverage for it is highly
limited: CI builds the Windows preview but runs no psmux test.

To check a build, run the native smoke. It needs the Visual Studio 2022 C++
workload and a psmux `tmux.exe` on `PATH`. Run the preset with Windows CMake
from a Developer PowerShell in a Windows-path checkout:

```console
$ cmake --preset windows-psmux
```

```console
$ cmake --build --preset windows-psmux
```

```console
$ ctest --preset windows-psmux --no-tests=error
```

## What it leaves out

- **Workspaces from a config file.** [tmuxp] already does that;
  [`examples/workspace/`](examples/workspace/README.md) reads the same documents
  if you need it inside a C++ program, and [`tmux-workspace`](#tmux-workspace)
  loads them from the command line.
- **Scripting.** A shell script calling `tmux` is a fine answer to a one-off.
  This earns its keep when the thing you are writing has to *read* tmux's state
  and decide something.
- **Python.** Use [libtmux] — it is the original, has a wider surface, and this
  port is measured against it.
- **Live objects.** An entity is a snapshot taken when it was listed; ask again
  with [`Window::refresh`][ref-window-refresh] for the present.
- **Exceptions for tmux failures.** Failure is a [`CommandFailure`][ref-commandfailure]
  value; nothing throws on a refusal.
- **Persistent control mode on Windows.** The psmux integration uses one
  `tmux.exe` subprocess per call.

## Support and status

- Alpha: any release may change or remove exported identifiers without a
  deprecation period. Pin an exact version.
- tmux 3.3a through `master`, on Linux (including WSL) and macOS; experimental
  x64 desktop Windows through psmux.
- CI runs Linux on `ubuntu-latest` and builds the Windows preview with MSVC on
  `windows-2022` without starting psmux. macOS and psmux are supported on a
  best-effort basis, and CI coverage for them is highly limited.
- C++23 (clang 17+, GCC 13+, Visual Studio 2022 17.3+), or C++20 with
  `LIBTMUX_CXX_STANDARD=20`. The core links nothing.
- Every pull request runs against real tmux under clang with libc++, GCC with
  libstdc++, the C++20 build over `tl::expected`, the sanitizers and a locale
  that is not UTF-8.
- The tmux matrix spans pinned cells from 3.3a through 3.8, plus `master`, and
  refusal cells for 3.0a, 3.1c, 3.2a and 3.3.
- Four parsers for tmux-controlled text have fuzz harnesses, and
  [a mutation catalogue](tools/README.md#the-mutation-catalogue) breaks one guard
  at a time and reports any that nothing notices.
- [The design commitments](docs/design/commitments.md) and
  [what was measured](docs/README.md) are in `docs/`.

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

### What it is called

One artifact, four spellings, because each namespace has its own rules and
none of them can be the same string.

| Spelling | Where it is the name |
|---|---|
| `libtmux-cxx` | the repository and its directory |
| `libtmux_cxx` | the CMake project |
| `libtmux::libtmux` | the CMake target to link, and `libtmux` the vcpkg port |
| `libtmuxpp` | the built library — `libtmuxpp.a`, `-ltmuxpp` |

Link the target, not the file: `find_package(libtmux)` then
`target_link_libraries(you PRIVATE libtmux::libtmux)`.

Outside CMake the
installed `libtmux.pc` carries the same answer, including the standard and the
`LIBTMUX_USE_TL_EXPECTED` the C++20 build needs.

## Project links

**Reference:**
[API](docs/api.md) ·
[Changelog](CHANGELOG.md) ·
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

## Attribution

Please use the following BibTeX template to cite libtmux-cxx in scientific discourse:

```bibtex
@misc{libtmux-cxx,
   author = {Tony Narlock},
   year = {2026},
   note = {https://libtmux.org/en/cxx/},
   title = {libtmux-cxx: C++ wrapper for tmux}
}
```

## Contributing

- [CONTRIBUTING.md](.github/CONTRIBUTING.md) covers getting a build, what a
  change needs to carry, and the gate to run before opening a pull request.
- Every capability needs a test against real tmux, failures are returned rather
  than thrown, and no test starts a tmux server the fixture did not make.
- [WRITING.md](.github/WRITING.md) covers the prose: the README, the changelog,
  release notes, commit messages, and what a header comment owes a caller.

## License

MIT. See [LICENSE](LICENSE).

[guide-concepts]: https://libtmux.org/en/cxx/latest/concepts/
[guide-start]: https://libtmux.org/en/cxx/latest/guides/getting-started/
[guide-testing]: https://libtmux.org/en/cxx/latest/guides/testing-with-libtmux/
[libtmux]: https://libtmux.git-pull.com
[mcp-guide]: https://libtmux.org/en/cxx/latest/mcp/guides/
[mcp-index]: https://libtmux.org/en/cxx/latest/mcp/
[mcp-tools]: https://libtmux.org/en/cxx/latest/mcp/tools/
[mcp-toolset-inspect]: https://libtmux.org/en/cxx/latest/mcp/reference/libtmux-mcp-toolset-inspect/
[psmux]: https://github.com/psmux/psmux
[ref-bad_expected_access]: https://libtmux.org/en/cxx/latest/reference/libtmux-bad_expected_access/
[ref-boolfieldhandle]: https://libtmux.org/en/cxx/latest/reference/libtmux-boolfieldhandle/
[ref-captureoptions]: https://libtmux.org/en/cxx/latest/reference/libtmux-captureoptions/
[ref-chain]: https://libtmux.org/en/cxx/latest/reference/libtmux-chain/
[ref-commandbatch]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandbatch/
[ref-commandfailure]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandfailure/
[ref-commandoperation-request_cancel]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandoperation-request_cancel/
[ref-commandoperation-wait_for]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandoperation-wait_for/
[ref-commandoperation]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandoperation/
[ref-commandruntime-close]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandruntime-close/
[ref-commandruntime-dispatch_ready]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandruntime-dispatch_ready/
[ref-commandruntime]: https://libtmux.org/en/cxx/latest/reference/libtmux-commandruntime/
[ref-connection-events]: https://libtmux.org/en/cxx/latest/reference/libtmux-connection-events/
[ref-connection-execute]: https://libtmux.org/en/cxx/latest/reference/libtmux-connection-execute/
[ref-connection]: https://libtmux.org/en/cxx/latest/reference/libtmux-connection/
[ref-deliverystatus]: https://libtmux.org/en/cxx/latest/reference/libtmux-deliverystatus/
[ref-exactly_one]: https://libtmux.org/en/cxx/latest/reference/libtmux-exactly_one/
[ref-expected]: https://libtmux.org/en/cxx/latest/reference/libtmux-expected/
[ref-failurekind]: https://libtmux.org/en/cxx/latest/reference/libtmux-failurekind/
[ref-filterexpr]: https://libtmux.org/en/cxx/latest/reference/libtmux-filterexpr/
[ref-first]: https://libtmux.org/en/cxx/latest/reference/libtmux-first/
[ref-index]: https://libtmux.org/en/cxx/latest/reference/
[ref-matching]: https://libtmux.org/en/cxx/latest/reference/libtmux-matching/
[ref-notificationwatch]: https://libtmux.org/en/cxx/latest/reference/libtmux-notificationwatch/
[ref-numberfieldhandle]: https://libtmux.org/en/cxx/latest/reference/libtmux-numberfieldhandle/
[ref-optionentry]: https://libtmux.org/en/cxx/latest/reference/libtmux-optionentry/
[ref-pane-break_out]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-break_out/
[ref-pane-capture]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-capture/
[ref-pane-expand]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-expand/
[ref-pane-send_line]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-send_line/
[ref-pane-session]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-session/
[ref-pane-wait_for_text]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-wait_for_text/
[ref-pane-window]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane-window/
[ref-pane]: https://libtmux.org/en/cxx/latest/reference/libtmux-pane/
[ref-server-at_default]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-at_default/
[ref-server-at_socket_name]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-at_socket_name/
[ref-server-at_socket_path]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-at_socket_path/
[ref-server-capabilities]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-capabilities/
[ref-server-check_release]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-check_release/
[ref-server-control]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-control/
[ref-server-from_env]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-from_env/
[ref-server-new_session]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-new_session/
[ref-server-over_control]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-over_control/
[ref-server-panes]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-panes/
[ref-server-run]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-run/
[ref-server-sessions]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-sessions/
[ref-server-startable_at_socket_path]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-startable_at_socket_path/
[ref-server-try_submit]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-try_submit/
[ref-server-windows]: https://libtmux.org/en/cxx/latest/reference/libtmux-server-windows/
[ref-server]: https://libtmux.org/en/cxx/latest/reference/libtmux-server/
[ref-servercapabilities]: https://libtmux.org/en/cxx/latest/reference/libtmux-servercapabilities/
[ref-serverfeature]: https://libtmux.org/en/cxx/latest/reference/libtmux-serverfeature/
[ref-session-new_window]: https://libtmux.org/en/cxx/latest/reference/libtmux-session-new_window/
[ref-session-option]: https://libtmux.org/en/cxx/latest/reference/libtmux-session-option/
[ref-session-set_option]: https://libtmux.org/en/cxx/latest/reference/libtmux-session-set_option/
[ref-session-windows]: https://libtmux.org/en/cxx/latest/reference/libtmux-session-windows/
[ref-session]: https://libtmux.org/en/cxx/latest/reference/libtmux-session/
[ref-stringfieldhandle]: https://libtmux.org/en/cxx/latest/reference/libtmux-stringfieldhandle/
[ref-test-scopedtmuxserver-start]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-scopedtmuxserver-start/
[ref-test-scopedtmuxserver]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-scopedtmuxserver/
[ref-test-socketnamespace]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-socketnamespace/
[ref-test-teardownreport]: https://libtmux.org/en/cxx/latest/reference/libtmux-test-teardownreport/
[ref-version]: https://libtmux.org/en/cxx/latest/reference/libtmux-version/
[ref-waitoptions]: https://libtmux.org/en/cxx/latest/reference/libtmux-waitoptions/
[ref-waitpath]: https://libtmux.org/en/cxx/latest/reference/libtmux-waitpath/
[ref-waitresult]: https://libtmux.org/en/cxx/latest/reference/libtmux-waitresult/
[ref-window-panes]: https://libtmux.org/en/cxx/latest/reference/libtmux-window-panes/
[ref-window-refresh]: https://libtmux.org/en/cxx/latest/reference/libtmux-window-refresh/
[ref-window-split]: https://libtmux.org/en/cxx/latest/reference/libtmux-window-split/
[ref-window]: https://libtmux.org/en/cxx/latest/reference/libtmux-window/
[tmux]: https://github.com/tmux/tmux
[tmuxp]: https://tmuxp.git-pull.com
[topic-errors]: https://libtmux.org/en/cxx/latest/topics/errors-and-exceptions/
[topic-options]: https://libtmux.org/en/cxx/latest/topics/options-and-hooks/
[topic-pane]: https://libtmux.org/en/cxx/latest/topics/pane-interaction/
[topic-queries]: https://libtmux.org/en/cxx/latest/concepts/queries/
[topic-transports]: https://libtmux.org/en/cxx/latest/concepts/transports/
[topic-waiting]: https://libtmux.org/en/cxx/latest/topics/waiting-and-retry/
[ws-cli]: https://libtmux.org/en/cxx/latest/workspace/cli/
[ws-configuration]: https://libtmux.org/en/cxx/latest/workspace/configuration/
[ws-load]: https://libtmux.org/en/cxx/latest/workspace/cli/load/
